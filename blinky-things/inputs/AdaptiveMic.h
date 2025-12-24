#pragma once
#include <stdint.h>
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// AdaptiveMic - Clean audio input with AGC
// - Provides raw, unsmoothed audio level (generators can smooth if needed)
// - AGC adapts gain to maintain target level
// - Transient detection for beat/percussion events
// - Hardware gain adapts to environment over minutes

namespace MicConstants {
    constexpr float MIN_DT_SECONDS = 0.0001f;         // Minimum dt clamp (0.1ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;         // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;     // PDM alive check timeout
}

/**
 * AdaptiveMic - Audio input with auto-gain control
 *
 * Simplified architecture:
 * - Raw samples → Normalize (0-1) → Apply AGC gain → Noise gate → Output
 * - No envelope smoothing (visualizers handle their own smoothing)
 * - AGC time constants control gain adaptation speed
 * - Transient detection for percussion/beat events
 */
class AdaptiveMic {
public:
  // ---- Tunables ----
  float noiseGate      = 0.04f;     // Noise gate threshold (0-1)

  // Software AGC - Simplified peak-based design
  bool  agEnabled      = true;
  static constexpr float AG_PEAK_TARGET = 1.0f;  // Always target full dynamic range

  // AGC time constants - Attack/Release envelope follower
  float agcAttackTau   = 0.1f;      // Peak attack: how fast to catch peaks (100ms)
  float agcReleaseTau  = 2.0f;      // Peak release: how slow peaks decay (2s)
  float agcGainTau     = 5.0f;      // Gain adjustment speed (5s adaptation)

  // Hardware gain (PRIMARY - adapts to raw ADC input for best signal quality)
  uint32_t hwCalibPeriodMs = 30000;   // 30 seconds between calibration checks
  int      hwGainMin       = 0;
  int      hwGainMax       = 64;
  int      hwGainStep      = 1;
  float    hwTargetLow     = 0.15f;   // If raw input below this, increase HW gain
  float    hwTargetHigh    = 0.35f;   // If raw input above this, decrease HW gain
  float    hwTrackingTau   = 10.0f;   // Time constant for tracking raw input (10s)

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, post-AGC, post-gate)
  float  globalGain    = 3.0f;  // Current AGC gain multiplier
  int    currentHardwareGain = 32;    // PDM hardware gain

  // Transient detection: single-frame impulse with strength
  float transient          = 0.0f;    // Impulse strength (0.0 = none, typically 0.0-1.0, clamped but can exceed 1.0 for very strong hits)
  float slowAvg            = 0.0f;    // Medium-term baseline (~150ms)
  float slowAlpha          = 0.025f;  // Baseline tracking speed
  float transientFactor    = 1.5f;    // Threshold multiplier
  float loudFloor          = 0.05f;   // Minimum absolute threshold
  uint32_t transientCooldownMs = 60;  // Cooldown between detections (ms)
  uint32_t lastTransientMs = 0;

  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs = MicConstants::MIC_DEAD_TIMEOUT_MS) const {
    return (nowMs - lastIsrMs) > timeoutMs;
  }

  // Public getters
  inline float getLevel() const { return level; }
  inline float getTransient() const { return transient; }
  inline float getTrackedLevel() const { return trackedLevel; }  // RMS level AGC is tracking
  inline float getGlobalGain() const { return globalGain; }
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }

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

  // AGC tracking
  float trackedLevel = 0.0f;     // Peak level tracked for software AGC (post-gain)
  float rawTrackedLevel = 0.0f;  // Raw ADC level tracked for hardware AGC (pre-gain)

  // Timing
  uint32_t lastHwCalibMs = 0;

  uint32_t _sampleRate = 16000;

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n);
  void autoGainTick(float normalizedLevel, float dt);
  void detectTransient(float normalizedLevel, float dt, uint32_t nowMs);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
