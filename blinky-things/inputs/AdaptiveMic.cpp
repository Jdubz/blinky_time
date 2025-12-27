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
  pdmAlive = false;
  recentAverage = 0.0f;
  previousLevel = 0.0f;

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

  // CRITICAL: Reset transient at start of EVERY update, not just when samples available
  // This ensures transient is a single-frame pulse that doesn't persist across frames
  transient = 0.0f;

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

    // Store instantaneous raw level for debugging
    rawInstantLevel = normalized;

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

    // Simplified amplitude spike detection
    detectTransients(nowMs, dt);
  }

  if (!pdmAlive) return;

  // Hardware gain adaptation (PRIMARY - optimizes ADC signal quality)
  // Skip if gain is locked for testing
  if (!hwGainLocked_) {
    hardwareCalibrate(nowMs, dt);
  }
}

void AdaptiveMic::lockHwGain(int gain) {
  // Lock hardware gain at specific value for testing (disables AGC)
  hwGainLocked_ = true;
  currentHardwareGain = constrainValue(gain, HW_GAIN_MIN, HW_GAIN_MAX);
  pdm_.setGain(currentHardwareGain);
}

void AdaptiveMic::unlockHwGain() {
  // Unlock hardware gain and re-enable AGC
  hwGainLocked_ = false;
  // Reset calibration timer to trigger immediate recalibration
  lastHwCalibMs = time_.millis() - MicConstants::HW_CALIB_PERIOD_MS;
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
  // Goal: Keep raw input at target level (hwTarget) for best SNR
  // This ensures high-quality signal into ADC before software processing

  // Adaptation period must be >= tracking window for stable measurements
  // HW_TRACKING_TAU = 30s, so check every 30s (matching tracking window for stability)
  // Use signed arithmetic to handle millis() wraparound at 49.7 days
  if ((int32_t)(nowMs - lastHwCalibMs) < (int32_t)MicConstants::HW_CALIB_PERIOD_MS) return;

  // Calculate error from target (signed: negative = too quiet, positive = too loud)
  constexpr float HW_TARGET_DEADZONE = 0.01f;  // ±0.01 dead zone around target
  float error = rawTrackedLevel - hwTarget;
  float errorMagnitude = fabsf(error);

  // Dead zone: Don't adjust if within ±0.01 of target
  if (errorMagnitude <= HW_TARGET_DEADZONE) {
    lastHwCalibMs = nowMs;
    return;
  }

  // Determine direction: negative error = too quiet → increase gain
  int direction = (error < 0.0f) ? +1 : -1;

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
  }

  // Note: We're already in ISR context, so interrupts are disabled.
  // Direct access to static volatiles is safe here without additional guards.
  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_isrCount++;

  s_instance->lastIsrMs = s_instance->time_.millis();
}

/**
 * SIMPLIFIED TRANSIENT DETECTION - "The Drummer's Algorithm"
 *
 * Detects MUSICAL hits (kicks, snares, bass drops) by looking for:
 * 1. LOUD: Significantly louder than recent average (3x default)
 * 2. SUDDEN: Rapidly rising (30% increase from previous frame)
 * 3. INFREQUENT: Cooldown prevents double-triggers
 *
 * This replaces 170 lines of DSP complexity with 15 lines of
 * "did someone hit a drum?" logic.
 */
void AdaptiveMic::detectTransients(uint32_t nowMs, float dt) {
  // Note: transient is reset at the start of update(), not here
  // This ensures it resets even when no audio samples are available (n==0)

  // Track recent average with exponential moving average
  float alpha = 1.0f - expf(-dt / averageTau);
  recentAverage += alpha * (level - recentAverage);

  // Detect transient: LOUD + SUDDEN + not in cooldown
  bool isLoudEnough = level > recentAverage * transientThreshold;
  bool isAttacking = level > previousLevel * attackMultiplier;
  bool cooldownElapsed = (int32_t)(nowMs - lastTransientMs) > cooldownMs;

  if (isLoudEnough && isAttacking && cooldownElapsed) {
    // Calculate strength: 0.0 at threshold, 1.0 at 2x threshold
    float ratio = level / maxValue(recentAverage, 0.001f);
    transient = clamp01((ratio - transientThreshold) / transientThreshold);
    lastTransientMs = nowMs;
  }

  // Store for next frame
  previousLevel = level;
}
