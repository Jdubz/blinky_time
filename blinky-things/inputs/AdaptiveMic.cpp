#include "AdaptiveMic.h"
#include "../hal/PlatformConstants.h"
#include "../types/BlinkyAssert.h"
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

// Overrun counters (monotonic from boot). See AdaptiveMic.h for rationale.
volatile uint32_t AdaptiveMic::s_overrunCount = 0;
volatile uint32_t AdaptiveMic::s_overrunSamplesLost = 0;

// Clipping telemetry + record tap (see AdaptiveMic.h).
volatile uint32_t AdaptiveMic::s_clipSamples = 0;
volatile uint32_t AdaptiveMic::s_totalSamples = 0;
volatile AdaptiveMic::RecordTap AdaptiveMic::s_recordTap = nullptr;

// ---------- Public ----------
AdaptiveMic::AdaptiveMic(IPdmMic& pdm, ISystemTime& time)
    : pdm_(pdm), time_(time) {
}

bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate = sampleRate;
  s_instance  = this;

  // Set hardware gain to the platform level and leave it fixed.
  // nRF52840: Platform::Microphone::DEFAULT_GAIN (32 = -4 dB; 0x28 = 0 dB)
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
  // Loop dt is not used for envelopes: they advance by the audio actually
  // consumed (audioDt below), so they behave the same at any loop rate.
  (void)dt;

  // On ESP32, poll() drains the I2S DMA buffer synchronously.
  // On nRF52 this is a no-op — the hardware interrupt already fired.
  pdm_.poll();

  // Get raw audio samples from ISR
  float avgAbs = 0.0f;
  uint16_t maxAbsVal = 0;
  uint32_t n = 0;
  consumeISR(avgAbs, maxAbsVal, n);
  if (maxAbsVal > peakAbsSinceTake_) peakAbsSinceTake_ = maxAbsVal;

  uint32_t nowMs = time_.millis();
  pdmAlive = !isMicDead(nowMs, 250);

  if (n > 0) {
    float normalized = avgAbs / 32768.0f;
    rawInstantLevel = normalized;
    // Envelope time = audio consumed, not loop time. update() runs every
    // loop, but samples arrive in PDM blocks: above the block rate most
    // updates consume nothing and return early, so stepping the followers
    // by loop dt slowed every attack/release (and the riser timers) by up
    // to 4x at 250 fps. Same clamp as dt.
    float audioDt = static_cast<float>(n) / static_cast<float>(_sampleRate);
    if (audioDt < MicConstants::MIN_DT_SECONDS) audioDt = MicConstants::MIN_DT_SECONDS;
    if (audioDt > MicConstants::MAX_DT_SECONDS) audioDt = MicConstants::MAX_DT_SECONDS;

    // No absolute noise gate. This MEMS mic's signal is so weak that music
    // sits at the level a fixed gate would call silence (the b200 gate at
    // 0.002 equalled the measured average music level of a mic with
    // enclosure holes), so any fixed threshold also swallows quiet music.
    // Removed 2025-12-26 for that reason, re-added in b200, removed again.
    // With the hardware gain fixed at its SNR optimum, the window/range
    // normalizer adapts to whatever range exists, room noise included.

    // Peak tracking with attack/release envelope
    riserSmooth_ += (1.0f - expf(-audioDt / MicConstants::RISER_SMOOTH_TAU)) * (normalized - riserSmooth_);
    aboveRunS_ = (normalized > peakLevel) ? aboveRunS_ + audioDt : 0.0f;
    if (!riserMode_ && aboveRunS_ >= MicConstants::RISER_HOLD_S) {
        riserMode_ = true;
        riserMax_ = riserSmooth_;
        riserStallS_ = 0.0f;
    }
    if (riserMode_) {
        if (riserSmooth_ > riserMax_) {
            // Rate-limited: a step (a hit) can't jump the target.
            const float cap = riserMax_ * powf(MicConstants::RISER_MAX_GROWTH, audioDt);
            riserMax_ = (riserSmooth_ < cap) ? riserSmooth_ : cap;
            riserStallS_ = 0.0f;
        } else {
            riserStallS_ += audioDt;
        }
        if (riserStallS_ >= MicConstants::RISER_STALL_S) riserMode_ = false;
    }
    if (riserMode_) {
        // Sustained rise (#176): lead the rise instead of trailing it, so
        // level stops clamping at 1.0 through a build-up. Target the
        // riser's running max so within-riser dips don't drag the peak.
        float target = MicConstants::RISER_HEADROOM * riserMax_;
        if (target > peakLevel) {
            float riserAlpha = 1.0f - expf(-audioDt / MicConstants::RISER_TAU);
            peakLevel += riserAlpha * (target - peakLevel);
        } else {
            // Peak above the riser target (e.g. instant adaptation fired on a
            // burst inside the build): release normally toward the larger of
            // target and input instead of freezing it there.
            float floorV = (normalized > target) ? normalized : target;
            float releaseAlpha = 1.0f - expf(-audioDt / maxValue(releaseTau, MicConstants::MIN_TAU_RANGE));
            peakLevel += releaseAlpha * (floorV - peakLevel);
        }
    } else {
        float tau = (normalized > peakLevel) ? peakTau : releaseTau;
        float peakAlpha = 1.0f - expf(-audioDt / maxValue(tau, MicConstants::MIN_TAU_RANGE));
        peakLevel += peakAlpha * (normalized - peakLevel);
    }

    // Instant adaptation for sustained loud jumps (a drop, a new section).
    // Requires INSTANT_ADAPT_BLOCKS consecutive updates: a single knock or
    // shout used to set the peak to 5x the music, and the 5 s release then
    // held level near 0.07 for ~8 s (PLP confidence and visuals collapsed).
    if (normalized > peakLevel * MicConstants::INSTANT_ADAPT_THRESHOLD) {
      if (++instantAdaptRun_ >= MicConstants::INSTANT_ADAPT_BLOCKS) {
        // Big jump after a quiet passage: lift the valley with it.
        // Otherwise the valley (20 s rise) stayed at breakdown level and
        // the drop — the loudest, most important moment — was squeezed
        // into level 0.45..1.0 for ~45 s. A too-high valley corrects
        // quickly: it follows new minimums with VALLEY_ATTACK_TAU.
        if (normalized > peakLevel * MicConstants::SECTION_JUMP_RATIO) {
          float floorV = MicConstants::SECTION_VALLEY_FRACTION * normalized;
          if (valleyLevel < floorV) valleyLevel = floorV;
        }
        peakLevel = normalized;
        instantAdaptRun_ = 0;
      }
    } else {
      instantAdaptRun_ = 0;
    }

    // Valley tracking: genuinely fast attack to new minimums (was peakTau =
    // 2 s, so after a 15-20 dB breakdown the input sat below the valley and
    // level read 0 for >5 s), slow release upward.
    float valleyTau = (normalized < valleyLevel) ? MicConstants::VALLEY_ATTACK_TAU
                    : releaseTau * MicConstants::VALLEY_RELEASE_MULTIPLIER;
    float valleyAlpha = 1.0f - expf(-audioDt / maxValue(valleyTau, MicConstants::MIN_TAU_RANGE));
    valleyLevel += valleyAlpha * (normalized - valleyLevel);
    valleyLevel = maxValue(valleyLevel, MicConstants::VALLEY_FLOOR);

    // Map to 0-1 based on peak/valley window
    float range = maxValue(MicConstants::MIN_NORMALIZATION_RANGE, peakLevel - valleyLevel);
    float mapped = (normalized - valleyLevel) / range;
    level = clamp01(mapped);
  }
}

uint32_t AdaptiveMic::armRecordTap(RecordTap tap, volatile uint32_t& tapWriteIdx) {
  // Interrupts off for a few instructions so the index snapshot, the
  // consumer's index init, and the tap install are atomic w.r.t. onPDMdata.
  noInterrupts();
  uint32_t idx = s_fftWriteIdx;
  tapWriteIdx = idx;
  s_recordTap = tap;
  interrupts();
  return idx;
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
  uint32_t localClips = 0;

  for (int i = 0; i < samples; ++i) {
    int16_t s = buffer[i];
    // |-32768| = 32768 doesn't fit int16 but fits uint16 via int32.
    uint16_t a = (uint16_t)((s < 0) ? -((int32_t)s) : s);
    localSumAbs += a;
    if (a > localMaxAbs) localMaxAbs = a;
    if (a >= CLIP_THRESHOLD) localClips++;
  }

  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_clipSamples  += localClips;
  s_totalSamples += samples;
  s_isrCount++;

  // Record-mode tap sees exactly the samples the FFT ring gets, indexed on
  // the same absolute counter, so record audio and spectral frames share
  // one timeline.
  uint32_t writeIdx = s_fftWriteIdx;
  RecordTap tap = s_recordTap;
  if (tap) tap(buffer, samples, writeIdx);

  // Copy samples to FFT ring buffer for spectral analysis
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

  // If we fell behind by more than a full ring, catch up to avoid stale data.
  //
  // This is a silent fallback in spirit: the FFT window built immediately
  // after this jump will contain audio from two non-contiguous time periods,
  // producing one frame of bogus mel/flux features. The PDM ISR keeps
  // sampling correctly throughout — the loss is purely in the consumer's
  // ability to keep up with the producer.
  //
  // We can't refuse to drop here (no other recovery exists once the ring
  // is overrun), but we MUST count and log so the corruption rate is
  // observable in telemetry. Counters are exposed via SerialConsole's
  // `json info` (see #124, plan-doc 2026-04-29). BLINKY_ASSERT_ONCE
  // surfaces the first occurrence on Serial without flooding if the bug
  // fires every loop iteration.
  if (available > FFT_RING_SIZE) {
    uint32_t lost = available - FFT_RING_SIZE;
    s_overrunCount++;
    s_overrunSamplesLost += lost;
    BLINKY_ASSERT_ONCE(false,
        "AdaptiveMic FFT ring overrun (consumer fell behind producer)");
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
