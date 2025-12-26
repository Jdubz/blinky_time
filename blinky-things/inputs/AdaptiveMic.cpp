#include "AdaptiveMic.h"
#include "../hal/PlatformConstants.h"
#include <math.h>

// Helper for constrain
template<typename T>
static T constrainValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

// Helper for max
template<typename T>
static T maxValue(T a, T b) {
    return (a > b) ? a : b;
}

// Helper for min
template<typename T>
static T minValue(T a, T b) {
    return (a < b) ? a : b;
}

// -------- Window/Range and tracking constants --------
constexpr float MIN_TAU_HARDWARE = 1.0f;      // Minimum hardware tracking tau (1s) to prevent instability
constexpr float MIN_TAU_RANGE = 0.1f;         // Minimum peak/valley tracking tau (100ms)
constexpr float MIN_NORMALIZATION_RANGE = 0.01f; // Minimum range to prevent division by zero (peak must be valley + this)
constexpr float INSTANT_ADAPT_THRESHOLD = 1.3f; // Jump to signal if it exceeds peak * threshold

// Valley tracking constants (for low-noise MEMS microphone)
constexpr float VALLEY_RELEASE_MULTIPLIER = 4.0f; // Valley releases 4x slower than peak (very slow upward drift)
constexpr float VALLEY_FLOOR = 0.001f;            // Minimum valley (0.1% of full scale, suits low-noise mic)

// Onset detection constants
constexpr float BASELINE_TAU = 0.5f;          // 500ms time constant for baseline (slower = more stable)
constexpr float ONSET_FLOOR = 0.001f;         // Minimum energy floor to prevent noise triggers
constexpr float MAX_ONSET_RATIO = 3.0f;       // Strength normalization: 1.0 at 3x threshold

// Alias for brevity (hardware gain limits are in PlatformConstants.h)
using Platform::Microphone::HW_GAIN_MIN;
using Platform::Microphone::HW_GAIN_MAX;

// -------- Static ISR accumulators --------
AdaptiveMic* AdaptiveMic::s_instance = nullptr;
volatile uint32_t AdaptiveMic::s_isrCount   = 0;
volatile uint64_t AdaptiveMic::s_sumAbs     = 0;
volatile uint32_t AdaptiveMic::s_numSamples = 0;
volatile uint16_t AdaptiveMic::s_maxAbs     = 0;
volatile uint32_t AdaptiveMic::s_zeroCrossings = 0;
volatile int16_t AdaptiveMic::s_lastSample     = 0;

// ---------- Public ----------
AdaptiveMic::AdaptiveMic(IPdmMic& pdm, ISystemTime& time)
    : pdm_(pdm), time_(time) {
}

bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate    = sampleRate;
  currentHardwareGain  = constrainValue(gainInit, HW_GAIN_MIN, HW_GAIN_MAX);
  s_instance     = this;

  pdm_.onReceive(AdaptiveMic::onPDMdata);

  bool ok = pdm_.begin(1, _sampleRate); // mono @ 16k
  if (!ok) return false;

  pdm_.setGain(currentHardwareGain);

  // Initialize state
  level = 0.0f;
  valleyLevel = VALLEY_FLOOR;  // Start valley very low for low-noise microphone (0.1% of full scale)
  peakLevel = 0.01f;  // Start peak at 1% of full scale
  transient = 0.0f;
  lastTransientMs = time_.millis();
  lastHwCalibMs = time_.millis();
  lastIsrMs = time_.millis();
  lastLowOnsetMs = time_.millis();
  lastHighOnsetMs = time_.millis();
  pdmAlive = false;

  // Initialize biquad filters for onset detection
  initBiquadFilters();

  return true;
}

void AdaptiveMic::end() {
  pdm_.end();
  s_instance = nullptr;
}

void AdaptiveMic::update(float dt) {
  // Clamp dt to reasonable range
  if (dt < MicConstants::MIN_DT_SECONDS) dt = MicConstants::MIN_DT_SECONDS;
  if (dt > MicConstants::MAX_DT_SECONDS) dt = MicConstants::MAX_DT_SECONDS;

  // Get raw audio samples from ISR
  float avgAbs = 0.0f;
  uint16_t maxAbsVal = 0;
  uint32_t n = 0;
  uint32_t zc = 0;
  consumeISR(avgAbs, maxAbsVal, n, zc);

  // Calculate zero-crossing rate (proportion of zero crossings to total samples)
  zeroCrossingRate = (n > 0) ? (float)zc / (float)n : 0.0f;

  uint32_t nowMs = time_.millis();
  pdmAlive = !isMicDead(nowMs, 250);

  if (n > 0) {
    // Normalize raw samples to 0-1 range
    // avgAbs is average of int16_t samples (0-32768 range)
    float normalized = avgAbs / 32768.0f;

    // Track raw input for hardware AGC (PRIMARY gain control)
    // Hardware gain adapts to keep raw ADC input in optimal range for best SNR
    float alpha = 1.0f - expf(-dt / maxValue(MicConstants::HW_TRACKING_TAU, MIN_TAU_HARDWARE));
    rawTrackedLevel += alpha * (normalized - rawTrackedLevel);

    // Window/Range normalization (SECONDARY - maps to 0-1 output)
    // Track peak with attack/release envelope
    float tau = (normalized > peakLevel) ? peakTau : releaseTau;
    float peakAlpha = 1.0f - expf(-dt / maxValue(tau, MIN_TAU_RANGE));
    peakLevel += peakAlpha * (normalized - peakLevel);

    // Immediate adaptation: jump to signal if far outside current range
    // This ensures loud transients are captured immediately without clipping
    if (normalized > peakLevel * INSTANT_ADAPT_THRESHOLD) {
      peakLevel = normalized;
    }

    // Valley tracking: Track actual signal floor (minimum) for low-noise microphone
    // Use asymmetric attack/release: fast attack to new minimums, slow release upward
    float valleyTau;
    if (normalized < valleyLevel) {
      // Fast attack to new minimum (capture quiet signals quickly)
      valleyTau = peakTau;
    } else {
      // Very slow release upward (valley can rise if noise floor increases)
      valleyTau = releaseTau * VALLEY_RELEASE_MULTIPLIER;
    }
    float valleyAlpha = 1.0f - expf(-dt / maxValue(valleyTau, MIN_TAU_RANGE));

    // Valley tracks toward current signal (with asymmetric response)
    valleyLevel += valleyAlpha * (normalized - valleyLevel);
    // Low-noise mic: Allow valley to go very low (0.1% of full scale)
    valleyLevel = maxValue(valleyLevel, VALLEY_FLOOR);

    // Map current signal to 0-1 range based on peak/valley window
    // Valley tracking serves as adaptive noise floor - no separate gate needed
    float range = maxValue(MIN_NORMALIZATION_RANGE, peakLevel - valleyLevel);
    float mapped = (normalized - valleyLevel) / range;
    level = clamp01(mapped);

    // Two-band onset detection (always enabled)
    detectOnsets(nowMs, dt, n);
  }

  if (!pdmAlive) return;

  // Hardware gain adaptation (PRIMARY - optimizes ADC signal quality)
  hardwareCalibrate(nowMs, dt);
}

// ---------- Private helpers ----------

void AdaptiveMic::consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n, uint32_t& zeroCrossings) {
  time_.noInterrupts();
  uint64_t sum = s_sumAbs;
  uint32_t cnt = s_numSamples;
  uint16_t m   = s_maxAbs;
  uint32_t zc  = s_zeroCrossings;
  s_sumAbs = 0;
  s_numSamples = 0;
  s_maxAbs = 0;
  s_zeroCrossings = 0;
  time_.interrupts();

  n = cnt;
  maxAbsVal = m;
  zeroCrossings = zc;
  avgAbs = (cnt > 0) ? float(sum) / float(cnt) : 0.0f;
}

void AdaptiveMic::hardwareCalibrate(uint32_t nowMs, float /*dt*/) {
  // PRIMARY GAIN CONTROL: Adjust hardware gain based on raw ADC input level
  // Goal: Keep raw input in optimal range (hwTargetLow - hwTargetHigh) for best SNR
  // This ensures high-quality signal into ADC before software processing

  // Adaptation period must be >= tracking window for stable measurements
  // HW_TRACKING_TAU = 30s, so check every 30s (matching tracking window for stability)
  // Use signed arithmetic to handle millis() wraparound at 49.7 days
  if ((int32_t)(nowMs - lastHwCalibMs) < (int32_t)MicConstants::HW_CALIB_PERIOD_MS) return;

  // Calculate how far we are from target range
  float errorMagnitude = 0.0f;
  int direction = 0;

  if (rawTrackedLevel < hwTargetLow) {
    // Raw input too quiet → increase hardware gain for better SNR
    errorMagnitude = hwTargetLow - rawTrackedLevel;
    direction = +1;
  } else if (rawTrackedLevel > hwTargetHigh) {
    // Raw input too loud → decrease hardware gain to prevent clipping
    errorMagnitude = rawTrackedLevel - hwTargetHigh;
    direction = -1;
  } else {
    // In target range - no adjustment needed
    lastHwCalibMs = nowMs;
    return;
  }

  // Adaptive step size: take bigger steps when far from target
  // Small error (< 0.05): +/- 1 step (fine-tuning)
  // Medium error (0.05-0.15): +/- 2 steps (adjusting)
  // Large error (> 0.15): +/- 4 steps (rapid correction)
  int stepSize;
  if (errorMagnitude > 0.15f) {
    stepSize = 4;  // Large error: 4 steps for fast convergence
  } else if (errorMagnitude > 0.05f) {
    stepSize = 2;  // Medium error: 2 steps for moderate correction
  } else {
    stepSize = 1;  // Small error: 1 step for fine-tuning
  }

  int delta = direction * stepSize;
  int oldGain = currentHardwareGain;
  currentHardwareGain = constrainValue(currentHardwareGain + delta, HW_GAIN_MIN, HW_GAIN_MAX);

  if (currentHardwareGain != oldGain) {
    pdm_.setGain(currentHardwareGain);
    // Note: With window/range normalization, no compensation needed
    // The peak tracker will naturally adapt to the new gain level
  }

  lastHwCalibMs = nowMs;
}

// ---------- ISR Callback ----------
// This callback is invoked by the PDM library when audio data is available.
// On nRF52840 with Seeeduino mbed core, PDM.onReceive() callbacks run in
// interrupt context, so interrupts are already disabled during execution.
//
// PERFORMANCE NOTES:
// - Typical execution: 256 samples @ 16kHz = ~1.5-2.0ms per ISR call
// - Biquad filters: 2 filters × 256 samples × ~20 cycles/filter = ~10k cycles
// - ARM Cortex-M4 @ 64MHz with FPU: ~0.16ms for biquad processing
// - Total ISR time: ~0.4-0.6ms (well under 16ms buffer interval)
// - Critical section (noInterrupts): <10µs for atomic variable updates
// - ISR does not block other interrupts except during critical section
void AdaptiveMic::onPDMdata() {
  if (!s_instance) return;
  int bytesAvailable = s_instance->pdm_.available();
  if (bytesAvailable <= 0) return;

  static int16_t buffer[512];
  int toRead = minValue(bytesAvailable, (int)sizeof(buffer));
  int bytesRead = s_instance->pdm_.read(buffer, toRead);
  if (bytesRead <= 0) return;

  int samples = bytesRead / (int)sizeof(int16_t);
  uint64_t localSumAbs = 0;
  uint16_t localMaxAbs = 0;

  for (int i = 0; i < samples; ++i) {
    int16_t s = buffer[i];
    // Use int32_t to avoid overflow when s == INT16_MIN (-32768)
    uint16_t a = (uint16_t)((s < 0) ? -((int32_t)s) : s);
    localSumAbs += a;
    if (a > localMaxAbs) localMaxAbs = a;

    // Count zero crossings for classification
    // A zero crossing occurs when sign changes between consecutive samples
    if ((s_lastSample >= 0 && s < 0) || (s_lastSample < 0 && s >= 0)) {
      s_zeroCrossings++;
    }
    s_lastSample = s;

    // Two-band filtering for onset detection
    // Process normalized sample through biquad filters and accumulate band energies
    float normalized = (float)s / 32768.0f;

    // Process through each frequency band filter
    float lowOut = s_instance->processBiquad(normalized, s_instance->lowZ1, s_instance->lowZ2,
                                              s_instance->lowB0, s_instance->lowB1, s_instance->lowB2,
                                              s_instance->lowA1, s_instance->lowA2);
    float highOut = s_instance->processBiquad(normalized, s_instance->highZ1, s_instance->highZ2,
                                               s_instance->highB0, s_instance->highB1, s_instance->highB2,
                                               s_instance->highA1, s_instance->highA2);

    // Accumulate energy (absolute value)
    s_instance->lowEnergy += (lowOut < 0.0f) ? -lowOut : lowOut;
    s_instance->highEnergy += (highOut < 0.0f) ? -highOut : highOut;
  }

  // Note: We're already in ISR context, so interrupts are disabled.
  // Direct access to static volatiles is safe here without additional guards.
  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_isrCount++;

  s_instance->lastIsrMs = s_instance->time_.millis();
}

// ---------- Biquad Filter Implementation ----------

void AdaptiveMic::calcBiquadBPF(float fc, float Q, float fs, float& b0, float& b1, float& b2, float& a1, float& a2) {
  // Bandpass filter using Audio EQ Cookbook formulas
  // fc = center frequency, Q = quality factor, fs = sample rate
  float omega = 2.0f * M_PI * fc / fs;
  float sinW = sinf(omega);
  float cosW = cosf(omega);
  float alpha = sinW / (2.0f * Q);

  float a0 = 1.0f + alpha;
  b0 = alpha / a0;
  b1 = 0.0f;
  b2 = -alpha / a0;
  a1 = -2.0f * cosW / a0;
  a2 = (1.0f - alpha) / a0;
}

void AdaptiveMic::initBiquadFilters() {
  // Low band filter: 50-200 Hz bandpass for bass transients
  // Center = 100 Hz, Q = 0.7 for wide bandwidth (~50-200 Hz)
  calcBiquadBPF(100.0f, 0.7f, (float)_sampleRate, lowB0, lowB1, lowB2, lowA1, lowA2);

  // High band filter: 2-8 kHz bandpass for brightness/attack transients
  // Center = 4 kHz, Q = 0.5 for very wide bandwidth (~2-8 kHz)
  calcBiquadBPF(4000.0f, 0.5f, (float)_sampleRate, highB0, highB1, highB2, highA1, highA2);

  // Reset filter state
  lowZ1 = lowZ2 = 0.0f;
  highZ1 = highZ2 = 0.0f;

  // Reset energy accumulators, baselines, and previous energy
  lowEnergy = highEnergy = 0.0f;
  lowBaseline = highBaseline = 0.0f;
  prevLowEnergy = prevHighEnergy = 0.0f;
}

inline float AdaptiveMic::processBiquad(float input, float& z1, float& z2, float b0, float b1, float b2, float a1, float a2) {
  // Direct Form II implementation (most efficient for embedded)
  // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
  float out = b0 * input + z1;
  z1 = b1 * input - a1 * out + z2;
  z2 = b2 * input - a2 * out;
  return out;
}

void AdaptiveMic::detectOnsets(uint32_t nowMs, float dt, uint32_t sampleCount) {
  // Reset onsets and transient each frame (single-frame pulse behavior)
  lowOnset = false;
  highOnset = false;
  transient = 0.0f;
  lowStrength = 0.0f;
  highStrength = 0.0f;

  // Read energy values atomically (ISR may be updating them)
  float localLowEnergy, localHighEnergy;
  time_.noInterrupts();
  localLowEnergy = lowEnergy;
  localHighEnergy = highEnergy;
  time_.interrupts();

  // Normalize energy by sample count to handle variable frame rates
  if (sampleCount > 0) {
    localLowEnergy /= (float)sampleCount;
    localHighEnergy /= (float)sampleCount;
  } else {
    // No samples processed, skip detection this frame
    time_.noInterrupts();
    lowEnergy = 0.0f;
    highEnergy = 0.0f;
    time_.interrupts();
    return;
  }

  // Update baselines with slow exponential moving average (frame-rate independent)
  float baselineAlpha = 1.0f - expf(-dt / maxValue(BASELINE_TAU, 0.001f));
  lowBaseline += baselineAlpha * (localLowEnergy - lowBaseline);
  highBaseline += baselineAlpha * (localHighEnergy - highBaseline);

  // Calculate rise from previous frame (for onset detection)
  float lowRise = (prevLowEnergy > 0.0001f) ? localLowEnergy / prevLowEnergy : 1.0f;
  float highRise = (prevHighEnergy > 0.0001f) ? localHighEnergy / prevHighEnergy : 1.0f;

  // Store current energy for next frame's rise calculation
  prevLowEnergy = localLowEnergy;
  prevHighEnergy = localHighEnergy;

  // Calculate thresholds (max of absolute floor and relative threshold)
  float lowThresh = maxValue(ONSET_FLOOR, lowBaseline * onsetThreshold);
  float highThresh = maxValue(ONSET_FLOOR, highBaseline * onsetThreshold);

  // Track maximum strength for combined transient output
  float maxOnsetStrength = 0.0f;

  // Detect low band onset (bass transient)
  // Requires: cooldown elapsed AND energy above threshold AND rising sharply
  if ((int32_t)(nowMs - lastLowOnsetMs) > (int32_t)MicConstants::ONSET_COOLDOWN_MS) {
    if (localLowEnergy > lowThresh && lowRise > riseThreshold) {
      lowOnset = true;
      // Normalize strength: 0 at threshold, 1.0 at MAX_ONSET_RATIO × threshold
      float ratio = localLowEnergy / lowThresh;
      lowStrength = clamp01((ratio - 1.0f) / (MAX_ONSET_RATIO - 1.0f));
      maxOnsetStrength = maxValue(maxOnsetStrength, lowStrength);
      lastLowOnsetMs = nowMs;
    }
  }

  // Detect high band onset (brightness transient)
  if ((int32_t)(nowMs - lastHighOnsetMs) > (int32_t)MicConstants::ONSET_COOLDOWN_MS) {
    if (localHighEnergy > highThresh && highRise > riseThreshold) {
      highOnset = true;
      float ratio = localHighEnergy / highThresh;
      highStrength = clamp01((ratio - 1.0f) / (MAX_ONSET_RATIO - 1.0f));
      maxOnsetStrength = maxValue(maxOnsetStrength, highStrength);
      lastHighOnsetMs = nowMs;
    }
  }

  // Set combined transient output
  if (maxOnsetStrength > 0.0f) {
    transient = maxOnsetStrength;
    lastTransientMs = nowMs;
  }

  // Reset energy accumulators atomically for next frame
  time_.noInterrupts();
  lowEnergy = 0.0f;
  highEnergy = 0.0f;
  time_.interrupts();
}
