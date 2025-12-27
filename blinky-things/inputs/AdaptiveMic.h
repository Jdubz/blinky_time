#pragma once
#include <stdint.h>
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// AdaptiveMic - Clean audio input with window/range normalization and onset detection
// - Provides raw, unsmoothed audio level (generators can smooth if needed)
// - Auto-ranging: Tracks peak/valley, maps to 0-1 output (no clipping)
// - Two-band onset detection: low (bass) and high (brightness/attack)
// - Hardware gain adapts to environment over minutes

namespace MicConstants {
    constexpr float MIN_DT_SECONDS = 0.0001f;         // Minimum dt clamp (0.1ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;         // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;     // PDM alive check timeout

    // Timing constants (not user-configurable)
    constexpr uint32_t ONSET_COOLDOWN_MS = 80;        // Onset detection cooldown (80ms = 12.5 hits/sec max)
    constexpr uint32_t HW_CALIB_PERIOD_MS = 30000;    // Hardware gain calibration period (30s)
    constexpr float HW_TRACKING_TAU = 30.0f;          // Hardware gain tracking time constant (30s)
}

/**
 * AdaptiveMic - Audio input with window/range normalization and onset detection
 *
 * Architecture:
 * - Raw samples → Normalize (0-1) → Window/Range mapping → Noise gate → Output
 * - Two-band onset detection via biquad bandpass filters:
 *   - Low band (50-200 Hz): Bass transients (kicks, bass hits)
 *   - High band (2-8 kHz): Brightness/attack transients (snare crack, hi-hats)
 * - Onset detection: Detects RISING energy, not just high energy
 * - Hardware-primary: HW gain optimizes ADC input, window/range maps to 0-1 output
 *
 * Onset Detection Algorithm:
 * - Tracks energy in each frequency band
 * - Detects when energy RISES sharply (>1.5x previous frame)
 * - AND exceeds baseline by threshold (2x baseline)
 * - Prevents false triggers on sustained audio
 *
 * Window/Range Normalization:
 * - Peak tracker follows actual signal levels (exponential moving average)
 * - Fast attack, slow release for natural loudness following
 * - Output = (signal - valleyLevel) / (peak - valleyLevel) → always 0-1
 */
class AdaptiveMic {
public:
  // ---- Tunables ----
  // Window/Range auto-normalization - Peak/valley tracking
  float peakTau        = 2.0f;      // Peak adaptation speed (attack time, seconds)
  float releaseTau     = 5.0f;      // Peak release speed (release time, seconds)

  // Hardware gain targets (adapts to raw ADC input for best signal quality)
  // Note: hwGainMin/Max are hardware limits (0-80 for nRF52840 PDM), not configurable
  float    hwTargetLow     = 0.15f;   // If raw input below this, increase HW gain
  float    hwTargetHigh    = 0.35f;   // If raw input above this, decrease HW gain

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, normalized via adaptive peak/valley tracking)
  int    currentHardwareGain = Platform::Microphone::DEFAULT_GAIN;    // PDM hardware gain

  // Combined transient output: single-frame impulse with strength (max of low/high)
  float transient          = 0.0f;    // Impulse strength (0.0 = none, typically 0.0-1.0, can exceed 1.0 for very strong hits)
  uint32_t lastTransientMs = 0;

  // Two-band onset detection
  bool  lowOnset   = false;       // Single-frame low-band onset (bass transient)
  bool  highOnset  = false;       // Single-frame high-band onset (brightness transient)
  float lowStrength  = 0.0f;      // Low-band onset strength (0.0-1.0)
  float highStrength = 0.0f;      // High-band onset strength (0.0-1.0)

  // Onset detection thresholds (tunable)
  // Higher values = less sensitive, fewer false positives
  float onsetThreshold = 2.5f;    // Energy must exceed baseline * threshold (default: 2.5x)
  float riseThreshold  = 1.5f;    // Energy must rise by this factor from previous frame (default: 1.5x)

  // Onset detection timing (tunable)
  uint16_t onsetCooldownMs = 80;  // Cooldown between detections (default: 80ms = 12.5 hits/sec max)
  float baselineTau = 0.5f;       // Baseline adaptation time constant (default: 0.5s)

  // Zero-crossing rate (for additional context)
  float zeroCrossingRate = 0.0f;  // Current ZCR (0.0-1.0, typically 0.0-0.5)

  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs = MicConstants::MIC_DEAD_TIMEOUT_MS) const {
    // Use signed arithmetic to handle millis() wraparound at 49.7 days
    return (int32_t)(nowMs - lastIsrMs) > (int32_t)timeoutMs;
  }

  // Public getters
  inline float getLevel() const { return level; }
  inline float getTransient() const { return transient; }
  inline float getPeakLevel() const { return peakLevel; }      // Current tracked peak (raw 0-1 range)
  inline float getValleyLevel() const { return valleyLevel; }  // Current tracked valley (raw 0-1 range)
  inline float getRawLevel() const { return rawTrackedLevel; } // Raw ADC level for HW gain tracking
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }
  inline bool isPdmAlive() const { return pdmAlive; }

  // Onset detection getters
  inline bool getLowOnset() const { return lowOnset; }
  inline bool getHighOnset() const { return highOnset; }
  inline float getLowStrength() const { return lowStrength; }
  inline float getHighStrength() const { return highStrength; }

  // Debug getters for testing/tuning (exposes internal detection state)
  inline float getLowBaseline() const { return lowBaseline; }
  inline float getHighBaseline() const { return highBaseline; }
  inline float getPrevLowEnergy() const { return prevLowEnergy; }
  inline float getPrevHighEnergy() const { return prevHighEnergy; }

  // Reset baselines (for test mode - start fresh detection)
  void resetBaselines();

public:
  /**
   * Construct with HAL dependencies for testability
   */
  AdaptiveMic(IPdmMic& pdm, ISystemTime& time);

  bool begin(uint32_t sampleRate = Platform::Microphone::DEFAULT_SAMPLE_RATE,
             int gainInit = Platform::Microphone::DEFAULT_GAIN);
  void end();

  void update(float dt);

  // ISR hook
  static void onPDMdata();

private:
  // HAL references
  IPdmMic& pdm_;
  ISystemTime& time_;

  // ISR accumulators
  static AdaptiveMic* s_instance;
  volatile static uint32_t s_isrCount;
  volatile static uint64_t s_sumAbs;
  volatile static uint32_t s_numSamples;
  volatile static uint16_t s_maxAbs;
  volatile static uint32_t s_zeroCrossings;  // Count of zero crossings
  volatile static int16_t s_lastSample;       // Previous sample for ZCR

  // Window/Range tracking
  float peakLevel = 0.0f;        // Tracked peak for range window
  float valleyLevel = 0.0f;      // Tracked valley for range window (typically noise gate)
  float rawTrackedLevel = 0.0f;  // Raw ADC level tracked for hardware AGC

  // Timing
  uint32_t lastHwCalibMs = 0;

  uint32_t _sampleRate = 16000;

  // Biquad filter state for two-band onset detection
  // Note: Filter state is only accessed within ISR, no volatile needed
  // Low band filter: bandpass 50-200 Hz (center: 100 Hz, Q=0.7)
  float lowZ1 = 0.0f, lowZ2 = 0.0f;
  float lowB0, lowB1, lowB2, lowA1, lowA2;

  // High band filter: bandpass 2-8 kHz (center: 4 kHz, Q=0.5)
  float highZ1 = 0.0f, highZ2 = 0.0f;
  float highB0, highB1, highB2, highA1, highA2;

  // Energy tracking per band (volatile: accessed from ISR)
  volatile float lowEnergy = 0.0f;
  volatile float highEnergy = 0.0f;

  // Previous frame energy for onset detection (rise detection)
  float prevLowEnergy = 0.0f;
  float prevHighEnergy = 0.0f;

  // Baselines per band (slow-adapting average)
  float lowBaseline = 0.0f;
  float highBaseline = 0.0f;

  // Timing for cooldowns
  uint32_t lastLowOnsetMs = 0;
  uint32_t lastHighOnsetMs = 0;

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n, uint32_t& zeroCrossings);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  // Biquad filter helpers
  void initBiquadFilters();
  void calcBiquadBPF(float fc, float Q, float fs, float& b0, float& b1, float& b2, float& a1, float& a2);
  inline float processBiquad(float input, float& z1, float& z2, float b0, float b1, float b2, float a1, float a2);
  void detectOnsets(uint32_t nowMs, float dt, uint32_t sampleCount);

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
