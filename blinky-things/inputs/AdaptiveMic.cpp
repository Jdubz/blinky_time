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

// FFT sample ring buffer
volatile int16_t AdaptiveMic::s_fftRing[AdaptiveMic::FFT_RING_SIZE] = {0};
volatile uint32_t AdaptiveMic::s_fftWriteIdx = 0;
uint32_t AdaptiveMic::s_fftReadIdx = 0;

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
  lastFluxValue_ = 0.0f;  // Initialize spectral flux value for RhythmAnalyzer

  // Initialize spectral flux detector
  spectralFlux_.begin();
  s_fftWriteIdx = 0;
  s_fftReadIdx = 0;

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

    // Clear spectral flux value if not in flux mode (prevent stale data)
    if (detectionMode != 3 && detectionMode != 4) {
      lastFluxValue_ = 0.0f;
    }
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

  // Copy samples to FFT ring buffer for spectral flux detection
  // This is a lock-free single-producer (ISR) / single-consumer (main) pattern
  uint32_t writeIdx = s_fftWriteIdx;
  for (int i = 0; i < samples; ++i) {
    s_fftRing[writeIdx & (FFT_RING_SIZE - 1)] = buffer[i];
    writeIdx++;
  }
  s_fftWriteIdx = writeIdx;  // Atomic write of final index

  s_instance->lastIsrMs = s_instance->time_.millis();
}

/**
 * TRANSIENT DETECTION DISPATCHER
 *
 * Routes to the appropriate detection algorithm based on detectionMode:
 * - 0 (DRUMMER): Original amplitude-based "Drummer's Algorithm"
 * - 1 (BASS_BAND): Biquad lowpass filter focusing on kick frequencies
 * - 2 (HFC): High Frequency Content for percussive transients
 * - 3 (SPECTRAL_FLUX): FFT-based spectral difference (placeholder)
 */
void AdaptiveMic::detectTransients(uint32_t nowMs, float dt) {
  // Note: transient is reset at the start of update(), not here
  // This ensures it resets even when no audio samples are available (n==0)

  float rawLevel = rawInstantLevel;

  // Dispatch to appropriate algorithm
  switch (static_cast<DetectionMode>(detectionMode)) {
    case DetectionMode::BASS_BAND:
      detectBassBand(nowMs, dt, rawLevel);
      break;
    case DetectionMode::HFC:
      detectHFC(nowMs, dt, rawLevel);
      break;
    case DetectionMode::SPECTRAL_FLUX:
      detectSpectralFlux(nowMs, dt, rawLevel);
      break;
    case DetectionMode::HYBRID:
      detectHybrid(nowMs, dt, rawLevel);
      break;
    case DetectionMode::DRUMMER:
    default:
      detectDrummer(nowMs, dt, rawLevel);
      break;
  }

  // Keep previousLevel updated for compatibility (all algorithms use this)
  previousLevel = rawLevel;
}

/**
 * DRUMMER'S ALGORITHM (Mode 0) - Original amplitude-based detection
 *
 * Detects MUSICAL hits (kicks, snares, bass drops) by looking for:
 * 1. LOUD: Significantly louder than recent average (3x default)
 * 2. SUDDEN: Rapidly rising compared to ~50ms ago (ring buffer lookback)
 * 3. INFREQUENT: Cooldown prevents double-triggers
 */
void AdaptiveMic::detectDrummer(uint32_t nowMs, float dt, float rawLevel) {
  // Track recent average with exponential moving average
  float alpha = 1.0f - expf(-dt / averageTau);
  recentAverage += alpha * (rawLevel - recentAverage);

  // Get baseline level from ~50-70ms ago (the oldest entry in ring buffer)
  float baselineLevel = attackBuffer[attackBufferIdx];

  // Detect transient: LOUD + SUDDEN + not in cooldown
  bool isLoudEnough = rawLevel > recentAverage * transientThreshold;
  bool isAttacking = rawLevel > baselineLevel * attackMultiplier;
  bool cooldownElapsed = (int32_t)(nowMs - lastTransientMs) > cooldownMs;

  if (isLoudEnough && isAttacking && cooldownElapsed) {
    // Calculate strength: 0.0 at threshold, 1.0 at 2x threshold
    float ratio = rawLevel / maxValue(recentAverage, 0.001f);
    transient = clamp01((ratio - transientThreshold) / transientThreshold);
    lastTransientMs = nowMs;
  }

  // Update ring buffer with current level (overwrites oldest entry)
  attackBuffer[attackBufferIdx] = rawLevel;
  attackBufferIdx = (attackBufferIdx + 1) % ATTACK_BUFFER_SIZE;
}

/**
 * BASS BAND FILTER (Mode 1) - Focus on kick drum frequencies
 *
 * Uses a biquad lowpass filter to isolate bass frequencies (60-200Hz),
 * then applies the same LOUD + SUDDEN + COOLDOWN logic to the filtered signal.
 * This should improve kick detection while reducing hi-hat false positives.
 */
void AdaptiveMic::detectBassBand(uint32_t nowMs, float dt, float rawLevel) {
  // FIXED: Use SpectralFlux with bass frequency range instead of envelope filtering
  // Bass range: bins 1-6 covers 62.5-375 Hz (kick drum + bass fundamentals)
  // At 16kHz sample rate: bin 1 = 62.5 Hz, bin 6 = 375 Hz
  spectralFlux_.setAnalysisRange(1, 6);  // Focus on bass frequencies

  // Read available samples from ISR ring buffer (same as spectral flux mode)
  uint32_t writeIdx = s_fftWriteIdx;  // Snapshot of write position
  uint32_t available = writeIdx - s_fftReadIdx;  // Handles wrap correctly

  // Limit to ring buffer size to prevent reading stale data
  if (available > FFT_RING_SIZE) {
    s_fftReadIdx = writeIdx - FFT_RING_SIZE;
    available = FFT_RING_SIZE;
  }

  // Feed samples to spectral flux processor
  while (available > 0) {
    int16_t batch[64];
    int batchSize = 0;

    while (batchSize < 64 && available > 0) {
      batch[batchSize++] = s_fftRing[s_fftReadIdx & (FFT_RING_SIZE - 1)];
      s_fftReadIdx++;
      available--;
    }

    spectralFlux_.addSamples(batch, batchSize);
  }

  // Process FFT frame if ready
  if (spectralFlux_.isFrameReady()) {
    float bassFlux = spectralFlux_.process();

    // SAFETY: Skip if flux is invalid
    if (!isfinite(bassFlux)) {
      bassFlux = 0.0f;
    }

    // Store as bassFilteredLevel for compatibility with debugging/telemetry
    bassFilteredLevel = bassFlux;

    // Track recent average of bass flux (for threshold comparison)
    float alpha = 1.0f - expf(-dt / averageTau);
    bassRecentAverage += alpha * (bassFlux - bassRecentAverage);

    // SAFETY: Reset if average becomes corrupted
    if (!isfinite(bassRecentAverage)) {
      bassRecentAverage = 0.0f;
    }

    // Detect transient using LOUD + COOLDOWN logic
    // Note: Bass flux is already a change measure, so we don't need "SUDDEN" check
    bool isLoudEnough = bassFlux > bassRecentAverage * bassThresh;
    bool cooldownElapsed = (int32_t)(nowMs - lastTransientMs) > cooldownMs;

    if (isLoudEnough && cooldownElapsed) {
      // Calculate strength: 0.0 at threshold, 1.0 at 2x threshold
      float ratio = bassFlux / maxValue(bassRecentAverage, 0.001f);
      transient = clamp01((ratio - bassThresh) / bassThresh);
      lastTransientMs = nowMs;
    }
  }

  // Also update main recentAverage for compatibility (using raw level)
  float alpha = 1.0f - expf(-dt / averageTau);
  recentAverage += alpha * (rawLevel - recentAverage);
}

/**
 * HIGH FREQUENCY CONTENT (Mode 2) - Emphasizes percussive transients
 *
 * HFC weights high frequencies heavily, which emphasizes transients
 * (drums have bright attacks with lots of high frequency content).
 * We approximate HFC using the derivative of the signal (sample differences).
 */
void AdaptiveMic::detectHFC(uint32_t nowMs, float dt, float rawLevel) {
  // Approximate HFC using signal derivative (emphasizes changes)
  // HFC = |current - previous|^2 weighted by hfcWeight
  float diff = rawLevel - previousLevel;
  float hfc = diff * diff * hfcWeight;

  // Track recent average of HFC
  float alpha = 1.0f - expf(-dt / averageTau);
  hfcRecentAverage += alpha * (hfc - hfcRecentAverage);

  // Detect transient in HFC signal
  bool isLoudEnough = hfc > hfcRecentAverage * hfcThresh;
  bool isAttacking = hfc > lastHfcValue * attackMultiplier;
  bool cooldownElapsed = (int32_t)(nowMs - lastTransientMs) > cooldownMs;

  if (isLoudEnough && isAttacking && cooldownElapsed) {
    float ratio = hfc / maxValue(hfcRecentAverage, 0.0001f);
    transient = clamp01((ratio - hfcThresh) / hfcThresh);
    lastTransientMs = nowMs;
  }

  lastHfcValue = hfc;

  // Also update main recentAverage for compatibility
  recentAverage += alpha * (rawLevel - recentAverage);
}

/**
 * SPECTRAL FLUX (Mode 3) - FFT-based detection
 *
 * Computes spectral flux by comparing magnitude spectra between frames.
 * Spectral flux spikes during transients (drums, bass drops) because
 * the frequency content changes rapidly.
 *
 * Algorithm:
 * 1. Read samples from ISR ring buffer
 * 2. Pass to SpectralFlux for FFT processing
 * 3. Detect spikes in flux signal (LOUD + SUDDEN + COOLDOWN)
 */
void AdaptiveMic::detectSpectralFlux(uint32_t nowMs, float dt, float rawLevel) {
  // Update analysis range based on fluxBins parameter
  // fluxBins controls how many frequency bins to analyze (focus on bass-mid)
  spectralFlux_.setAnalysisRange(1, fluxBins);  // Skip DC (bin 0)

  // Read available samples from ISR ring buffer
  // Use signed comparison to handle uint32_t wrap-around correctly (~74 hours at 16kHz)
  uint32_t writeIdx = s_fftWriteIdx;  // Snapshot of write position
  uint32_t available = writeIdx - s_fftReadIdx;  // Handles wrap correctly

  // Limit to ring buffer size to prevent reading stale data if we fell behind
  if (available > FFT_RING_SIZE) {
    // We fell behind - skip old samples to catch up
    s_fftReadIdx = writeIdx - FFT_RING_SIZE;
    available = FFT_RING_SIZE;
  }

  // Feed samples to spectral flux processor
  while (available > 0) {
    // Read in batches for efficiency
    int16_t batch[64];
    int batchSize = 0;

    while (batchSize < 64 && available > 0) {
      batch[batchSize++] = s_fftRing[s_fftReadIdx & (FFT_RING_SIZE - 1)];
      s_fftReadIdx++;
      available--;
    }

    spectralFlux_.addSamples(batch, batchSize);
  }

  // Process FFT frame if ready
  if (spectralFlux_.isFrameReady()) {
    float flux = spectralFlux_.process();

    // SAFETY: Skip if flux is invalid
    if (!isfinite(flux)) {
      flux = 0.0f;
    }

    // Store for external access (e.g., RhythmAnalyzer)
    lastFluxValue_ = flux;

    // Update running average (for threshold comparison)
    float alpha = 1.0f - expf(-dt / averageTau);
    fluxRecentAverage_ += alpha * (flux - fluxRecentAverage_);

    // SAFETY: Reset if average becomes corrupted
    if (!isfinite(fluxRecentAverage_)) {
      fluxRecentAverage_ = 0.0f;
    }

    // Detect transient using same LOUD + SUDDEN + COOLDOWN logic
    bool isLoudEnough = flux > fluxRecentAverage_ * fluxThresh;
    bool cooldownElapsed = (int32_t)(nowMs - lastTransientMs) > cooldownMs;

    if (isLoudEnough && cooldownElapsed) {
      // Calculate strength: 0.0 at threshold, 1.0 at 2x threshold
      float ratio = flux / maxValue(fluxRecentAverage_, 0.001f);
      transient = clamp01((ratio - fluxThresh) / fluxThresh);
      lastTransientMs = nowMs;
    }
  }

  // Also update main recentAverage for compatibility (using raw level)
  float alpha = 1.0f - expf(-dt / averageTau);
  recentAverage += alpha * (rawLevel - recentAverage);
}

/**
 * HYBRID DETECTION (Mode 4) - Combines drummer + spectral flux
 *
 * Runs both algorithms and combines their outputs for confidence scoring:
 * - Both detect: High confidence (1.0 strength)
 * - Flux only: Medium-high confidence (0.7 strength)
 * - Drummer only: Medium confidence (0.5 strength)
 * - Neither: No detection
 *
 * This leverages:
 * - Spectral flux's high recall (catches most beats)
 * - Drummer's precision on certain patterns (good hat rejection)
 */
void AdaptiveMic::detectHybrid(uint32_t nowMs, float dt, float rawLevel) {
  // Update tracking averages (needed by both algorithms)
  float alpha = 1.0f - expf(-dt / averageTau);
  recentAverage += alpha * (rawLevel - recentAverage);

  // Process spectral flux (feed samples to FFT)
  spectralFlux_.setAnalysisRange(1, fluxBins);
  uint32_t writeIdx = s_fftWriteIdx;
  uint32_t available = writeIdx - s_fftReadIdx;

  if (available > FFT_RING_SIZE) {
    s_fftReadIdx = writeIdx - FFT_RING_SIZE;
    available = FFT_RING_SIZE;
  }

  while (available > 0) {
    int16_t batch[64];
    int batchSize = 0;
    while (batchSize < 64 && available > 0) {
      batch[batchSize++] = s_fftRing[s_fftReadIdx & (FFT_RING_SIZE - 1)];
      s_fftReadIdx++;
      available--;
    }
    spectralFlux_.addSamples(batch, batchSize);
  }

  // Evaluate both algorithms (without triggering detection yet)
  float drummerStrength = evalDrummerStrength(rawLevel);
  float fluxStrength = evalSpectralFluxStrength(dt);

  // Combine detections with confidence weighting
  bool cooldownElapsed = (int32_t)(nowMs - lastTransientMs) > cooldownMs;

  if (cooldownElapsed) {
    float confidence = 0.0f;

    if (drummerStrength > 0.0f && fluxStrength > 0.0f) {
      // Both algorithms agree - high confidence
      // Use max strength, boosted by agreement
      confidence = maxValue(drummerStrength, fluxStrength);
      confidence = minValue(1.0f, confidence * hybridBothBoost);
    } else if (fluxStrength > 0.0f) {
      // Spectral flux only - medium-high confidence
      confidence = fluxStrength * hybridFluxWeight;
    } else if (drummerStrength > 0.0f) {
      // Drummer only - medium confidence
      confidence = drummerStrength * hybridDrumWeight;
    }

    if (confidence > 0.0f) {
      transient = clamp01(confidence);
      lastTransientMs = nowMs;
    }
  }

  // Update attack buffer for drummer algorithm
  attackBuffer[attackBufferIdx] = rawLevel;
  attackBufferIdx = (attackBufferIdx + 1) % ATTACK_BUFFER_SIZE;
}

/**
 * Evaluate drummer algorithm strength without side effects
 * Returns 0.0 if no detection, 0.0-1.0 for detection strength
 */
float AdaptiveMic::evalDrummerStrength(float rawLevel) {
  float baselineLevel = attackBuffer[attackBufferIdx];

  bool isLoudEnough = rawLevel > recentAverage * transientThreshold;
  bool isAttacking = rawLevel > baselineLevel * attackMultiplier;

  if (isLoudEnough && isAttacking) {
    float ratio = rawLevel / maxValue(recentAverage, 0.001f);
    return clamp01((ratio - transientThreshold) / transientThreshold);
  }
  return 0.0f;
}

/**
 * Evaluate spectral flux strength and process FFT frame
 * NOTE: This consumes the FFT frame and updates fluxRecentAverage_
 * Returns 0.0 if no detection or frame not ready, 0.0-1.0 for detection strength
 */
float AdaptiveMic::evalSpectralFluxStrength(float dt) {
  if (!spectralFlux_.isFrameReady()) {
    return 0.0f;
  }

  float flux = spectralFlux_.process();

  // SAFETY: Skip if flux is invalid
  if (!isfinite(flux)) {
    return 0.0f;
  }

  // Update running average
  float alpha = 1.0f - expf(-dt / averageTau);
  fluxRecentAverage_ += alpha * (flux - fluxRecentAverage_);

  if (!isfinite(fluxRecentAverage_)) {
    fluxRecentAverage_ = 0.0f;
    return 0.0f;
  }

  // Check if loud enough
  if (flux > fluxRecentAverage_ * fluxThresh) {
    float ratio = flux / maxValue(fluxRecentAverage_, 0.001f);
    return clamp01((ratio - fluxThresh) / fluxThresh);
  }
  return 0.0f;
}
