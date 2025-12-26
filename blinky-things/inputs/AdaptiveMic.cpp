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

// -------- Window/Range and tracking constants --------
constexpr float MIN_TAU_HARDWARE = 1.0f;      // Minimum hardware tracking tau (1s) to prevent instability
constexpr float MIN_TAU_RANGE = 0.1f;         // Minimum peak/valley tracking tau (100ms)
constexpr float MIN_NORMALIZATION_RANGE = 0.01f; // Minimum range to prevent division by zero (peak must be valley + this)
constexpr float INSTANT_ADAPT_THRESHOLD = 1.3f; // Jump to signal if it exceeds peak * threshold
constexpr float SNARE_DOMINANCE_THRESHOLD = 1.5f;  // Snare must exceed kick by this factor for simultaneous detection

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
  currentHardwareGain  = constrainValue(gainInit, hwGainMin, hwGainMax);
  s_instance     = this;

  pdm_.onReceive(AdaptiveMic::onPDMdata);

  bool ok = pdm_.begin(1, _sampleRate); // mono @ 16k
  if (!ok) return false;

  pdm_.setGain(currentHardwareGain);

  // Initialize state
  level = 0.0f;
  valleyLevel = noiseGate * 0.5f;  // FIX: Start valley at half of noise gate (2% instead of 4%)
  peakLevel = valleyLevel + MIN_NORMALIZATION_RANGE;  // FIX: Start peak just above valley
  transient = 0.0f;
  lastTransientMs = time_.millis();
  lastHwCalibMs = time_.millis();
  lastIsrMs = time_.millis();
  lastKickMs = time_.millis();
  lastSnareMs = time_.millis();
  lastHihatMs = time_.millis();
  pdmAlive = false;

  // Initialize biquad filters for frequency-specific detection
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

    // Enforce minimum peak floor: valley + minimum range
    // This ensures range >= MIN_NORMALIZATION_RANGE without artificially inflating peak
    float minPeak = valleyLevel + MIN_NORMALIZATION_RANGE;
    peakLevel = maxValue(peakLevel, minPeak);

    // Immediate adaptation: jump to signal if far outside current range
    // This ensures loud transients are captured immediately without clipping
    if (normalized > peakLevel * INSTANT_ADAPT_THRESHOLD) {
      peakLevel = normalized;
    }

    // FIX: Valley should track the actual signal floor (minimum), not a percentage
    // Use asymmetric attack/release: fast attack to new minimums, slow release upward
    float valleyTau;
    if (normalized < valleyLevel) {
      // Fast attack to new minimum (capture quiet signals quickly)
      valleyTau = peakTau;
    } else {
      // Very slow release upward (valley can rise if noise floor increases)
      valleyTau = releaseTau * 4.0f;
    }
    float valleyAlpha = 1.0f - expf(-dt / maxValue(valleyTau, MIN_TAU_RANGE));

    // Valley tracks toward current signal (with asymmetric response)
    valleyLevel += valleyAlpha * (normalized - valleyLevel);
    valleyLevel = maxValue(valleyLevel, noiseGate * 0.5f);  // Enforce minimum at half of noise gate

    // Map current signal to 0-1 range based on peak/valley window
    float range = maxValue(MIN_NORMALIZATION_RANGE, peakLevel - valleyLevel);
    float mapped = (normalized - valleyLevel) / range;
    mapped = clamp01(mapped);

    // FIX: Apply noise gate to MAPPED output, not raw signal
    // This allows quiet signals to be visible after normalization
    level = (mapped < 0.05f) ? 0.0f : mapped;  // Gate at 5% of mapped range instead of raw signal

    // Frequency-specific percussion detection (always enabled)
    detectFrequencySpecific(nowMs, dt, n);
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
  currentHardwareGain = constrainValue(currentHardwareGain + delta, hwGainMin, hwGainMax);

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

    // Frequency-specific filtering (always enabled)
    // Process normalized sample through biquad filters and accumulate band energies
    float normalized = (float)s / 32768.0f;

    // Process through each frequency band filter
    float kickOut = s_instance->processBiquad(normalized, s_instance->kickZ1, s_instance->kickZ2,
                                                s_instance->kickB0, s_instance->kickB1, s_instance->kickB2,
                                                s_instance->kickA1, s_instance->kickA2);
    float snareOut = s_instance->processBiquad(normalized, s_instance->snareZ1, s_instance->snareZ2,
                                                 s_instance->snareB0, s_instance->snareB1, s_instance->snareB2,
                                                 s_instance->snareA1, s_instance->snareA2);
    float hihatOut = s_instance->processBiquad(normalized, s_instance->hihatZ1, s_instance->hihatZ2,
                                                 s_instance->hihatB0, s_instance->hihatB1, s_instance->hihatB2,
                                                 s_instance->hihatA1, s_instance->hihatA2);

    // Accumulate energy (absolute value)
    s_instance->kickEnergy += (kickOut < 0.0f) ? -kickOut : kickOut;
    s_instance->snareEnergy += (snareOut < 0.0f) ? -snareOut : snareOut;
    s_instance->hihatEnergy += (hihatOut < 0.0f) ? -hihatOut : hihatOut;
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
  // Kick filter: 60-130 Hz bandpass, center = 90 Hz, Q = 1.5 for moderate bandwidth
  calcBiquadBPF(90.0f, 1.5f, (float)_sampleRate, kickB0, kickB1, kickB2, kickA1, kickA2);

  // Snare filter: 300-750 Hz bandpass, center = 500 Hz, Q = 1.5
  calcBiquadBPF(500.0f, 1.5f, (float)_sampleRate, snareB0, snareB1, snareB2, snareA1, snareA2);

  // Hihat filter: 5-7 kHz bandpass, center = 6 kHz, Q = 1.5
  calcBiquadBPF(6000.0f, 1.5f, (float)_sampleRate, hihatB0, hihatB1, hihatB2, hihatA1, hihatA2);

  // Reset filter state
  kickZ1 = kickZ2 = 0.0f;
  snareZ1 = snareZ2 = 0.0f;
  hihatZ1 = hihatZ2 = 0.0f;

  // Reset energy accumulators and baselines
  kickEnergy = snareEnergy = hihatEnergy = 0.0f;
  kickBaseline = snareBaseline = hihatBaseline = 0.0f;
}

inline float AdaptiveMic::processBiquad(float input, float& z1, float& z2, float b0, float b1, float b2, float a1, float a2) {
  // Direct Form II implementation (most efficient for embedded)
  // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
  float out = b0 * input + z1;
  z1 = b1 * input - a1 * out + z2;
  z2 = b2 * input - a2 * out;
  return out;
}

void AdaptiveMic::detectFrequencySpecific(uint32_t nowMs, float dt, uint32_t sampleCount) {
  // Reset impulses and transient each frame (single-frame pulse behavior)
  kickImpulse = false;
  snareImpulse = false;
  hihatImpulse = false;
  transient = 0.0f;
  kickStrength = 0.0f;
  snareStrength = 0.0f;
  hihatStrength = 0.0f;

  // Track maximum percussion strength to link with generic transient
  float maxPercussionStrength = 0.0f;

  // Read energy values atomically (ISR may be updating them)
  float localKickEnergy, localSnareEnergy, localHihatEnergy;
  time_.noInterrupts();
  localKickEnergy = kickEnergy;
  localSnareEnergy = snareEnergy;
  localHihatEnergy = hihatEnergy;
  time_.interrupts();

  // CRITICAL FIX #1: Normalize energy by sample count to handle variable frame rates
  // This prevents false triggers when update() is called at inconsistent intervals
  if (sampleCount > 0) {
    localKickEnergy /= (float)sampleCount;
    localSnareEnergy /= (float)sampleCount;
    localHihatEnergy /= (float)sampleCount;
  } else {
    // No samples processed, skip detection this frame
    time_.noInterrupts();
    kickEnergy = 0.0f;
    snareEnergy = 0.0f;
    hihatEnergy = 0.0f;
    time_.interrupts();
    return;
  }

  // CRITICAL FIX #3: Time-based exponential moving average for baselines
  // This makes baseline tracking frame-rate independent
  constexpr float BASELINE_TAU = 0.15f;  // 150ms time constant
  float baselineAlpha = 1.0f - expf(-dt / maxValue(BASELINE_TAU, 0.001f));
  kickBaseline += baselineAlpha * (localKickEnergy - kickBaseline);
  snareBaseline += baselineAlpha * (localSnareEnergy - snareBaseline);
  hihatBaseline += baselineAlpha * (localHihatEnergy - hihatBaseline);

  // Check which bands have energy above threshold
  bool kickDetected = false;
  bool snareDetected = false;
  bool hihatDetected = false;

  // Minimum percussion floor threshold (absolute minimum to prevent noise triggers)
  constexpr float PERCUSSION_FLOOR = 0.0001f;  // Very low floor - rely on relative thresholds

  // IMPROVED THRESHOLD LOGIC (#6): Clearer, simpler threshold calculation
  // Use maximum of absolute floor OR relative threshold
  // Detect kick (bass transient)
  if ((int32_t)(nowMs - lastKickMs) > (int32_t)MicConstants::TRANSIENT_COOLDOWN_MS) {
    float kickThresh = maxValue(PERCUSSION_FLOOR, kickBaseline * kickThreshold);
    if (localKickEnergy > kickThresh) {
      kickDetected = true;
      kickStrength = localKickEnergy / maxValue(kickThresh, 0.0001f);
    }
  }

  // Detect snare (mid transient)
  if ((int32_t)(nowMs - lastSnareMs) > (int32_t)MicConstants::TRANSIENT_COOLDOWN_MS) {
    float snareThresh = maxValue(PERCUSSION_FLOOR, snareBaseline * snareThreshold);
    if (localSnareEnergy > snareThresh) {
      snareDetected = true;
      snareStrength = localSnareEnergy / maxValue(snareThresh, 0.0001f);
    }
  }

  // Detect hihat (high transient)
  if ((int32_t)(nowMs - lastHihatMs) > (int32_t)MicConstants::TRANSIENT_COOLDOWN_MS) {
    float hihatThresh = maxValue(PERCUSSION_FLOOR, hihatBaseline * hihatThreshold);
    if (localHihatEnergy > hihatThresh) {
      hihatDetected = true;
      hihatStrength = localHihatEnergy / maxValue(hihatThresh, 0.0001f);
    }
  }

  // IMPROVED PRIORITIZATION (#5): Allow simultaneous kick+snare when snare is dominant
  // This prevents missing real snare hits that occur with kick drums (e.g., backbeats)
  if (kickDetected) {
    // Kick detected - always trigger kick
    kickImpulse = true;
    maxPercussionStrength = maxValue(maxPercussionStrength, kickStrength);
    lastKickMs = nowMs;

    // Allow snare if it's MUCH stronger than kick (likely a real snare hit)
    if (snareDetected && snareStrength > kickStrength * SNARE_DOMINANCE_THRESHOLD) {
      snareImpulse = true;
      maxPercussionStrength = maxValue(maxPercussionStrength, snareStrength);
      lastSnareMs = nowMs;
    }

    // Allow simultaneous hihat (common in music)
    if (hihatDetected) {
      hihatImpulse = true;
      maxPercussionStrength = maxValue(maxPercussionStrength, hihatStrength);
      lastHihatMs = nowMs;
    }
  } else if (snareDetected) {
    // Snare detected (and no kick) - trigger snare
    snareImpulse = true;
    maxPercussionStrength = maxValue(maxPercussionStrength, snareStrength);
    lastSnareMs = nowMs;

    // Allow simultaneous hihat
    if (hihatDetected) {
      hihatImpulse = true;
      maxPercussionStrength = maxValue(maxPercussionStrength, hihatStrength);
      lastHihatMs = nowMs;
    }
  } else if (hihatDetected) {
    // Only hihat detected
    hihatImpulse = true;
    maxPercussionStrength = maxValue(maxPercussionStrength, hihatStrength);
    lastHihatMs = nowMs;
  }

  // Link frequency-specific detection to generic transient
  // This ensures percussion hits trigger fire bursts and appear in UI chart
  if (maxPercussionStrength > 0.0f) {
    // Set transient to maximum percussion strength detected this frame
    transient = maxPercussionStrength;
    // Update cooldown timer to prevent generic detector from immediately overriding
    lastTransientMs = nowMs;
  }

  // Reset energy accumulators atomically for next frame (ISR is accumulating)
  time_.noInterrupts();
  kickEnergy = 0.0f;
  snareEnergy = 0.0f;
  hihatEnergy = 0.0f;
  time_.interrupts();
}
