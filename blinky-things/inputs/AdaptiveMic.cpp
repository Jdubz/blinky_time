#include "AdaptiveMic.h"
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

// -------- Static ISR accumulators --------
AdaptiveMic* AdaptiveMic::s_instance = nullptr;
volatile uint32_t AdaptiveMic::s_isrCount   = 0;
volatile uint64_t AdaptiveMic::s_sumAbs     = 0;
volatile uint32_t AdaptiveMic::s_numSamples = 0;
volatile uint16_t AdaptiveMic::s_maxAbs     = 0;

// ---------- Public ----------
AdaptiveMic::AdaptiveMic(IPdmMic& pdm, ISystemTime& time)
    : pdm_(pdm), time_(time) {
}

bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate    = sampleRate;
  currentHardwareGain  = constrainValue(gainInit, hwGainMin, hwGainMax);
  s_instance     = this;

  pdm_.onReceive(AdaptiveMic::onPDMdata);

  bool ok = pdm_.begin(1, _sampleRate); // mono @ 16k
  if (!ok) return false;

  pdm_.setGain(currentHardwareGain);

  // Initialize state
  level = 0.0f;
  trackedLevel = 0.0f;
  slowAvg = 0.0f;
  transient = 0.0f;
  lastTransientMs = time_.millis();
  lastHwCalibMs = time_.millis();
  lastIsrMs = time_.millis();
  pdmAlive = false;

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
  consumeISR(avgAbs, maxAbsVal, n);

  uint32_t nowMs = time_.millis();
  pdmAlive = !isMicDead(nowMs, 250);

  if (n > 0) {
    // Normalize raw samples to 0-1 range
    // avgAbs is average of int16_t samples (0-32768 range)
    float normalized = avgAbs / 32768.0f;

    // Track raw input for hardware AGC (PRIMARY gain control)
    // Hardware gain adapts to keep raw ADC input in optimal range for best SNR
    float alpha = 1.0f - expf(-dt / maxValue(hwTrackingTau, 1.0f));
    rawTrackedLevel += alpha * (normalized - rawTrackedLevel);

    // Apply software gain (SECONDARY - fine adjustments only)
    float afterGain = normalized * globalGain;
    afterGain = clamp01(afterGain);

    // Software AGC fine-tunes output to target (tracks post-gain level)
    if (agEnabled) {
      autoGainTick(afterGain, dt);
    }

    // Apply noise gate
    level = (afterGain < noiseGate) ? 0.0f : afterGain;

    // Transient detection
    detectTransient(level, dt, nowMs);
  }

  if (!pdmAlive) return;

  // Hardware gain adaptation (PRIMARY - optimizes ADC signal quality)
  hardwareCalibrate(nowMs, dt);
}

// ---------- Private helpers ----------

void AdaptiveMic::consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n) {
  time_.noInterrupts();
  uint64_t sum = s_sumAbs;
  uint32_t cnt = s_numSamples;
  uint16_t m   = s_maxAbs;
  s_sumAbs = 0;
  s_numSamples = 0;
  s_maxAbs = 0;
  time_.interrupts();

  n = cnt;
  maxAbsVal = m;
  avgAbs = (cnt > 0) ? float(sum) / float(cnt) : 0.0f;
}

void AdaptiveMic::autoGainTick(float postGainLevel, float dt) {
  // SECONDARY GAIN CONTROL: Software AGC for fine-tuning
  // Hardware gain (PRIMARY) already optimized raw ADC input
  // Software gain just makes small adjustments to hit target output level
  //
  // Design: Peak-based AGC with fast attack, slow release
  // 1. Track peak level with envelope follower
  // 2. Adjust gain to make peaks hit target (1.0 = full dynamic range)

  // Track peak level with envelope follower
  float tau = (postGainLevel > trackedLevel) ? agcAttackTau : agcReleaseTau;
  float alpha = 1.0f - expf(-dt / maxValue(tau, 0.001f));
  trackedLevel += alpha * (postGainLevel - trackedLevel);

  // Compute error: how far are peaks from target?
  float err = AG_PEAK_TARGET - trackedLevel;

  // Adjust gain to correct the error (proportional control)
  float gainAlpha = 1.0f - expf(-dt / maxValue(agcGainTau, 0.1f));
  globalGain += gainAlpha * err * globalGain;  // Logarithmic adjustment

  // Constrain software gain to narrow range (fine adjustments only)
  // Hardware gain handles large changes, software just tweaks around 1.0x
  globalGain = constrainValue(globalGain, 0.1f, 10.0f);
}

void AdaptiveMic::detectTransient(float normalizedLevel, float dt, uint32_t nowMs) {
  // Reset impulse each frame (ensures it's only true for ONE frame)
  transient = 0.0f;

  // Energy envelope first-order difference (use current baseline, before update)
  float energyDiff = maxValue(0.0f, normalizedLevel - slowAvg);

  bool cooldownExpired = (nowMs - lastTransientMs) > transientCooldownMs;

  // Adaptive threshold based on recent activity (use current baseline)
  float adaptiveThreshold = maxValue(loudFloor, slowAvg * transientFactor);

  // Detect onset when:
  // 1. Cooldown has expired
  // 2. Energy difference exceeds adaptive threshold
  // 3. Rising edge check (level 20% above baseline)
  //    - When loudFloor dominates: prevents false triggers during quiet periods
  //    - When transientFactor dominates: redundant but harmless (already > 2.5x baseline)
  if (cooldownExpired && energyDiff > adaptiveThreshold && normalizedLevel > slowAvg * 1.2f) {
      // Set transient strength (0.0-1.0+, clamped to reasonable range)
      // Protect against division by zero
      transient = minValue(1.0f, energyDiff / maxValue(adaptiveThreshold, 0.001f));
      lastTransientMs = nowMs;
  }

  // Update slow baseline AFTER detection (prevents threshold from chasing signal)
  slowAvg += slowAlpha * (normalizedLevel - slowAvg);
}

void AdaptiveMic::hardwareCalibrate(uint32_t nowMs, float /*dt*/) {
  // PRIMARY GAIN CONTROL: Adjust hardware gain based on raw ADC input level
  // Goal: Keep raw input in optimal range (hwTargetLow - hwTargetHigh) for best SNR
  // This ensures high-quality signal into ADC before software processing

  // Adaptation period must be >= tracking window for stable measurements
  // hwTrackingTau = 10s, so check every 30s (3x tracking window for stability)
  if ((nowMs - lastHwCalibMs) < hwCalibPeriodMs) return;

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
  currentHardwareGain = constrainValue(currentHardwareGain + delta, hwGainMin, hwGainMax);

  if (currentHardwareGain != oldGain) {
    pdm_.setGain(currentHardwareGain);

    // Compensate software gain to smooth transition (reduce audio artifacts)
    // Estimate: each HW gain step ≈ 5% change
    float hwGainChange = (currentHardwareGain - oldGain) * 0.05f;
    float softComp = 1.0f / (1.0f + hwGainChange);
    globalGain = constrainValue(globalGain * softComp, 0.1f, 10.0f);
  }

  lastHwCalibMs = nowMs;
}

// ---------- ISR Callback ----------
// This callback is invoked by the PDM library when audio data is available.
// On nRF52840 with Seeeduino mbed core, PDM.onReceive() callbacks run in
// interrupt context, so interrupts are already disabled during execution.
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
  }

  // Note: We're already in ISR context, so interrupts are disabled.
  // Direct access to static volatiles is safe here without additional guards.
  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_isrCount++;

  s_instance->lastIsrMs = s_instance->time_.millis();
}
