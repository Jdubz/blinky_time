#include "AdaptiveMic.h"
#include "../hal/PlatformConstants.h"
#include <math.h>

// ============================================================================
// AdaptiveMic — Fixed-gain microphone with window/range normalization
//
// Hardware gain is set once at boot to the platform's optimal level and never
// changed. All dynamic range adaptation is done via peak/valley tracking.
// This eliminates competing AGC systems and creates identical signal
// processing on nRF52840 and ESP32-S3.
// ============================================================================

template<typename T>
static inline T maxValue(T a, T b) { return (a > b) ? a : b; }

template<typename T>
static inline T minValue(T a, T b) { return (a < b) ? a : b; }

template<typename T>
static inline T constrainValue(T value, T minVal, T maxVal) {
    return (value < minVal) ? minVal : ((value > maxVal) ? maxVal : value);
}

// ISR processing constants
constexpr int ISR_BUFFER_SIZE = 512;

// -------- Static ISR accumulators --------
AdaptiveMic* AdaptiveMic::s_instance = nullptr;
volatile uint32_t AdaptiveMic::s_isrCount   = 0;
volatile uint64_t AdaptiveMic::s_sumAbs     = 0;
volatile uint32_t AdaptiveMic::s_numSamples = 0;
volatile uint16_t AdaptiveMic::s_maxAbs     = 0;

// FFT sample ring buffer
volatile int16_t AdaptiveMic::s_fftRing[AdaptiveMic::FFT_RING_SIZE] = {0};
volatile uint32_t AdaptiveMic::s_fftWriteIdx = 0;
uint32_t AdaptiveMic::s_extFftReadIdx = 0;

// ---------- Public ----------
AdaptiveMic::AdaptiveMic(IPdmMic& pdm, ISystemTime& time)
    : pdm_(pdm), time_(time) {
}

bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate = sampleRate;
  s_instance  = this;

  // Set hardware gain to platform optimal level and leave it fixed.
  // nRF52840: gain 40 (hardware PDM, SNR peaks at 25-35, degrades >40)
  // ESP32-S3: gain 30 (software post-decimation, SNR degrades >30)
  currentHardwareGain = constrainValue(gainInit, pdm_.getGainMinDb(), pdm_.getGainMaxDb());

  pdm_.onReceive(AdaptiveMic::onPDMdata);

  bool ok = pdm_.begin(1, _sampleRate); // mono @ 16k
  if (!ok) return false;

  pdm_.setGain(currentHardwareGain);

  // Initialize state
  level = 0.0f;
  valleyLevel = MicConstants::VALLEY_FLOOR;
  peakLevel = 0.01f;
  lastIsrMs = time_.millis();
  pdmAlive = false;

  s_fftWriteIdx = 0;
  s_extFftReadIdx = 0;

  return true;
}

void AdaptiveMic::end() {
  pdm_.end();
  s_instance = nullptr;
}

void AdaptiveMic::update(float dt) {
  if (dt < MicConstants::MIN_DT_SECONDS) dt = MicConstants::MIN_DT_SECONDS;
  if (dt > MicConstants::MAX_DT_SECONDS) dt = MicConstants::MAX_DT_SECONDS;

  // On ESP32, poll() drains the I2S DMA buffer synchronously.
  // On nRF52 this is a no-op — the hardware interrupt already fired.
  pdm_.poll();

  // Get raw audio samples from ISR
  float avgAbs = 0.0f;
  uint16_t maxAbsVal = 0;
  uint32_t n = 0;
  consumeISR(avgAbs, maxAbsVal, n);

  uint32_t nowMs = time_.millis();
  pdmAlive = !isMicDead(nowMs, 250);

  if (n > 0) {
    float normalized = avgAbs / 32768.0f;
    rawInstantLevel = normalized;

    // Peak tracking with attack/release envelope
    float tau = (normalized > peakLevel) ? peakTau : releaseTau;
    float peakAlpha = 1.0f - expf(-dt / maxValue(tau, MicConstants::MIN_TAU_RANGE));
    peakLevel += peakAlpha * (normalized - peakLevel);

    // Instant adaptation for loud transients
    if (normalized > peakLevel * MicConstants::INSTANT_ADAPT_THRESHOLD) {
      peakLevel = normalized;
    }

    // Valley tracking: fast attack to new minimums, slow release upward
    float valleyTau = (normalized < valleyLevel) ? peakTau
                    : releaseTau * MicConstants::VALLEY_RELEASE_MULTIPLIER;
    float valleyAlpha = 1.0f - expf(-dt / maxValue(valleyTau, MicConstants::MIN_TAU_RANGE));
    valleyLevel += valleyAlpha * (normalized - valleyLevel);
    valleyLevel = maxValue(valleyLevel, MicConstants::VALLEY_FLOOR);

    // Map to 0-1 based on peak/valley window
    float range = maxValue(MicConstants::MIN_NORMALIZATION_RANGE, peakLevel - valleyLevel);
    float mapped = (normalized - valleyLevel) / range;
    level = clamp01(mapped);
  }
}

// ---------- ISR data consumption ----------
void AdaptiveMic::consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n) {
  time_.disableInterrupts();
  uint64_t sum = s_sumAbs;
  uint32_t cnt = s_numSamples;
  uint16_t m   = s_maxAbs;
  s_sumAbs = 0;
  s_numSamples = 0;
  s_maxAbs = 0;
  time_.enableInterrupts();

  n = cnt;
  maxAbsVal = m;
  avgAbs = (cnt > 0) ? float(sum) / float(cnt) : 0.0f;
}

// ---------- ISR Callback ----------
void AdaptiveMic::onPDMdata() {
  if (!s_instance) return;
  int bytesAvailable = s_instance->pdm_.available();
  if (bytesAvailable <= 0) return;

  static int16_t buffer[ISR_BUFFER_SIZE];
  int toRead = minValue(bytesAvailable, (int)sizeof(buffer));
  int bytesRead = s_instance->pdm_.read(buffer, toRead);
  if (bytesRead <= 0) return;

  int samples = bytesRead / (int)sizeof(int16_t);
  uint64_t localSumAbs = 0;
  uint16_t localMaxAbs = 0;

  for (int i = 0; i < samples; ++i) {
    int16_t s = buffer[i];
    uint16_t a = (uint16_t)((s < 0) ? -((int32_t)s) : s);
    localSumAbs += a;
    if (a > localMaxAbs) localMaxAbs = a;
  }

  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_isrCount++;

  // Copy samples to FFT ring buffer for spectral analysis
  uint32_t writeIdx = s_fftWriteIdx;
  for (int i = 0; i < samples; ++i) {
    s_fftRing[writeIdx & (FFT_RING_SIZE - 1)] = buffer[i];
    writeIdx++;
  }
  s_fftWriteIdx = writeIdx;

  s_instance->lastIsrMs = s_instance->time_.millis();
}

/**
 * Get samples from ISR ring buffer for external FFT consumers.
 * Returns the number of samples actually copied.
 */
int AdaptiveMic::getSamplesForExternal(int16_t* buffer, int maxCount) {
  if (!buffer || maxCount <= 0) return 0;

  // Snapshot write index with interrupts disabled to prevent ISR from
  // advancing s_fftWriteIdx mid-read (same pattern as consumeISR).
  time_.disableInterrupts();
  uint32_t writeIdx = s_fftWriteIdx;
  time_.enableInterrupts();

  uint32_t available = writeIdx - s_extFftReadIdx;
  if (available == 0) return 0;

  // If we fell behind by more than a full ring, catch up to avoid stale data
  if (available > FFT_RING_SIZE) {
    s_extFftReadIdx = writeIdx - FFT_RING_SIZE;
    available = FFT_RING_SIZE;
  }

  int toCopy = (int)available;
  if (toCopy > maxCount) toCopy = maxCount;

  for (int i = 0; i < toCopy; ++i) {
    buffer[i] = s_fftRing[(s_extFftReadIdx + i) & (FFT_RING_SIZE - 1)];
  }
  s_extFftReadIdx += toCopy;
  return toCopy;
}
