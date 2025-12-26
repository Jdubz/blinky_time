#pragma once
#include <stdint.h>
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// AdaptiveMic - Clean audio input with window/range normalization and percussion detection
// - Provides raw, unsmoothed audio level (generators can smooth if needed)
// - Auto-ranging: Tracks peak/valley, maps to 0-1 output (no clipping)
// - Frequency-specific percussion detection (kick/snare/hihat)
// - Hardware gain adapts to environment over minutes

namespace MicConstants {
    constexpr float MIN_DT_SECONDS = 0.0001f;         // Minimum dt clamp (0.1ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;         // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;     // PDM alive check timeout

    // Timing constants (not user-configurable)
    constexpr uint32_t TRANSIENT_COOLDOWN_MS = 60;    // Percussion detection cooldown
    constexpr uint32_t HW_CALIB_PERIOD_MS = 30000;    // Hardware gain calibration period (30s)
    constexpr float HW_TRACKING_TAU = 30.0f;          // Hardware gain tracking time constant (30s)
}

/**
 * AdaptiveMic - Audio input with window/range normalization and percussion detection
 *
 * Architecture:
 * - Raw samples → Normalize (0-1) → Window/Range mapping → Noise gate → Output
 * - Frequency-specific percussion detection via biquad bandpass filters
 * - No envelope smoothing (visualizers handle their own smoothing)
 * - Hardware-primary: HW gain optimizes ADC input, window/range maps to 0-1 output
 * - Percussion detection: kick (60-130 Hz), snare (300-750 Hz), hihat (5-7 kHz)
 *
 * Window/Range Normalization (Follows Loudness):
 * - Peak tracker follows actual signal levels (exponential moving average)
 * - Fast attack (peakTau) when signal exceeds peak
 * - Slow release (releaseTau) when signal drops below peak
 * - Instant adaptation for loud transients (>1.3x current peak)
 * - Output = (signal - noiseGate) / (peak - noiseGate) → always 0-1
 * - As music gets louder: peak increases, range expands, still maps to 0-1
 * - As music gets quieter: peak decreases, range shrinks, still maps to 0-1
 * - NO clipping - full dynamic range preserved
 */
class AdaptiveMic {
public:
  // ---- Tunables ----
  float noiseGate      = 0.04f;     // Noise gate threshold (0-1)

  // Window/Range auto-normalization - Peak/valley tracking
  // Peak tracks actual signal levels (no target - follows loudness naturally)
  float peakTau        = 2.0f;      // Peak adaptation speed (attack time, seconds)
  float releaseTau     = 5.0f;      // Peak release speed (release time, seconds)

  // Hardware gain (PRIMARY - adapts to raw ADC input for best signal quality)
  int      hwGainMin       = 0;
  int      hwGainMax       = 64;
  int      hwGainStep      = 1;
  float    hwTargetLow     = 0.15f;   // If raw input below this, increase HW gain
  float    hwTargetHigh    = 0.35f;   // If raw input above this, decrease HW gain

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, post-range-mapping, post-gate)
  int    currentHardwareGain = Platform::Microphone::DEFAULT_GAIN;    // PDM hardware gain

  // Percussion transient: single-frame impulse with strength (max of kick/snare/hihat)
  float transient          = 0.0f;    // Impulse strength (0.0 = none, typically 0.0-1.0, can exceed 1.0 for very strong hits)
  uint32_t lastTransientMs = 0;

  // Frequency-specific percussion detection (Phase 3)
  bool  kickImpulse  = false;     // Single-frame kick detection
  bool  snareImpulse = false;     // Single-frame snare detection
  bool  hihatImpulse = false;     // Single-frame hihat detection
  float kickStrength = 0.0f;      // Kick strength (0.0-1.0+)
  float snareStrength = 0.0f;     // Snare strength (0.0-1.0+)
  float hihatStrength = 0.0f;     // Hihat strength (0.0-1.0+)

  // Frequency detection thresholds (tunable)
  // Lower values = more sensitive (1.0 = any energy above baseline triggers)
  // Kick is most important (lowest), hihat often continuous (highest)
  float kickThreshold  = 1.15f;   // Bass energy must exceed baseline * threshold (was 1.3f)
  float snareThreshold = 1.20f;   // Mid energy must exceed baseline * threshold (was 1.3f)
  float hihatThreshold = 1.25f;   // High energy must exceed baseline * threshold (was 1.3f)

  // Zero-crossing rate (for improved classification and reliability)
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
  inline float getPeakLevel() const { return peakLevel; }      // Current tracked peak
  inline float getValleyLevel() const { return valleyLevel; }  // Current tracked valley
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }

  // Frequency-specific getters
  inline bool getKickImpulse() const { return kickImpulse; }
  inline bool getSnareImpulse() const { return snareImpulse; }
  inline bool getHihatImpulse() const { return hihatImpulse; }
  inline float getKickStrength() const { return kickStrength; }
  inline float getSnareStrength() const { return snareStrength; }
  inline float getHihatStrength() const { return hihatStrength; }

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

  // Biquad filter state for frequency-specific detection
  // Note: Filter state is only accessed within ISR, no volatile needed
  // Kick filter: bandpass 60-130 Hz (center: 90 Hz)
  float kickZ1 = 0.0f, kickZ2 = 0.0f;
  float kickB0, kickB1, kickB2, kickA1, kickA2;

  // Snare filter: bandpass 300-750 Hz (center: 500 Hz)
  float snareZ1 = 0.0f, snareZ2 = 0.0f;
  float snareB0, snareB1, snareB2, snareA1, snareA2;

  // Hihat filter: bandpass 5-7 kHz (center: 6 kHz)
  float hihatZ1 = 0.0f, hihatZ2 = 0.0f;
  float hihatB0, hihatB1, hihatB2, hihatA1, hihatA2;

  // Energy tracking per frequency band (volatile: accessed from ISR)
  volatile float kickEnergy = 0.0f;
  volatile float snareEnergy = 0.0f;
  volatile float hihatEnergy = 0.0f;

  // Baselines per frequency band
  float kickBaseline = 0.0f;
  float snareBaseline = 0.0f;
  float hihatBaseline = 0.0f;

  // Timing for frequency-specific cooldowns
  uint32_t lastKickMs = 0;
  uint32_t lastSnareMs = 0;
  uint32_t lastHihatMs = 0;

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n, uint32_t& zeroCrossings);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  // Biquad filter helpers
  void initBiquadFilters();
  void calcBiquadBPF(float fc, float Q, float fs, float& b0, float& b1, float& b2, float& a1, float& a2);
  inline float processBiquad(float input, float& z1, float& z2, float b0, float b1, float b2, float a1, float a2);
  void detectFrequencySpecific(uint32_t nowMs, float dt, uint32_t sampleCount);

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
