#pragma once
#include <stdint.h>
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// AdaptiveMic
// - Output uses raw instantaneous mic average (snappy, no smoothing).
// - Envelope/envMean kept only for normalization + gain adaptation.
// - Hardware gain adapts slowly (minutes).
// - Software AGC adapts over ~10s.
// - Continuous normalization window.

// Time constants for envelope and gain adaptation
namespace MicConstants {
    constexpr float ENV_MEAN_TAU_SECONDS = 90.0f;     // ~90s EMA for long-term env tracking
    constexpr float MIN_DT_SECONDS = 0.0001f;         // Minimum dt clamp (0.1ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;         // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;     // PDM alive check timeout
}

/**
 * AdaptiveMic - Audio input with auto-gain control
 *
 * Uses HAL interfaces for hardware abstraction, enabling unit testing.
 * Default values from PlatformConstants.h.
 */
class AdaptiveMic {
public:
  // ---- Tunables ----
  // Envelope smoothing (still used for tracking only)
  float attackSeconds = 0.08f;
  float releaseSeconds = 0.30f;

  // Normalization window (slow adaptation preserves dynamics)
  float normFloorDecay = 0.99995f;  // Very slow floor tracking (~20s)
  float normCeilDecay  = 0.9999f;   // Slow ceiling tracking (~10s)
  float normInset      = 0.02f;
  float noiseGate      = 0.04f;     // Lower gate for sensitivity

  // Software AGC (phrase-level adaptation ~6-8 seconds behind)
  bool  agEnabled      = true;
  float agTarget       = 0.50f;     // Target level
  float agStrength     = 0.25f;     // Faster adaptation
  float agMin          = 1.0f;      // Never go below 1.0 gain
  float agMax          = 12.0f;     // Allow high gain for quiet sources

  // AGC time constants (professional audio standards)
  float agcTauSeconds  = 7.0f;      // Main AGC adaptation time (5-10s window)
  float agcAttackTau   = 2.0f;      // Attack time constant (faster response to increases)
  float agcReleaseTau  = 10.0f;     // Release time constant (slower response to decreases)

  // Hardware gain (minutes scale)
  uint32_t hwCalibPeriodMs = 180000;  // 3 minutes - environmental adaptation
  float    envTargetRaw    = 1000.0f;
  float    envLowRatio     = 0.50f;
  float    envHighRatio    = 1.50f;
  int      hwGainMin       = 0;
  int      hwGainMax       = 64;
  int      hwGainStep      = 1;

  // Dwell timers for coordination
  float limitDwellTriggerSec = 8.0f;
  float limitDwellRelaxSec   = 3.0f;

  // ---- Public state ----
  float  levelInstant  = 0.0f;  // raw instantaneous average
  float  levelPreGate  = 0.0f;  // normalized, pre-gate, pre-AGC
  float  levelPostAGC  = 0.0f;  // final snappy output
  float  envAR         = 0.0f;  // smoothed envelope (tracking only)
  float  envMean       = 0.0f;  // very slow EMA of envAR
  float  globalGain    = 3.0f;  // software AGC multiplier (start higher)
  int    currentHardwareGain = 32;    // PDM hardware gain

  // --- Enhanced Musical Analysis ---
  // Transient detection: single-frame impulse with strength
  // Returns 0.0f normally, non-zero (0.0-1.0) for ONE frame when transient detected
  float transient          = 0.0f;    // Impulse with strength (0.0 = none, >0.0 = detected)
  float slowAvg            = 0.0f;    // Medium-term baseline (~150ms)
  float slowAlpha          = 0.025f;  // Baseline tracking speed
  float transientFactor    = 1.5f;    // Threshold: level must be 1.5x above baseline
  float loudFloor          = 0.05f;   // Minimum threshold for sensitivity
  uint32_t transientCooldownMs = 60;  // 60ms = 16.7 hits/sec (supports 32nd notes at 125 BPM)
  uint32_t lastTransientMs = 0;

  float getTransient() const { return transient; }

  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs = MicConstants::MIC_DEAD_TIMEOUT_MS) const {
    return (nowMs - lastIsrMs) > timeoutMs;
  }

  // Debug helpers
  float getLevelInstant() const { return levelInstant; }
  float getLevelPreGate() const { return levelPreGate; }
  float getLevelPostAGC() const { return levelPostAGC; }
  float getEnvMean()     const { return envMean; }
  float getGlobalGain()  const { return globalGain; }
  int   getHwGain()      const { return currentHardwareGain; }
  uint32_t getIsrCount() const { return s_isrCount; }

public:
  /**
   * Construct with HAL dependencies for testability
   */
  AdaptiveMic(IPdmMic& pdm, ISystemTime& time);

  bool begin(uint32_t sampleRate = Platform::Microphone::DEFAULT_SAMPLE_RATE,
             int gainInit = Platform::Microphone::DEFAULT_GAIN);
  void end();

  void update(float dt);

  // The main thing FireEffect consumes
  inline float getLevel() const { return levelPostAGC; }

  // Envelope access for debugging
  inline float getEnv() const { return envAR; }

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

  // Normalization window
  float minEnv = 1e9f;
  float maxEnv = 0.0f;

  // AGC tracking
  float trackedLevel = 0.0f;  // RMS level tracked over AGC window

  // Timing
  uint32_t lastHwCalibMs = 0;

  // Dwell timers
  float dwellAtMin = 0.0f;
  float dwellAtMax = 0.0f;

  // Cached coeffs
  float aAtk = 0.0f, aRel = 0.0f;
  uint32_t _sampleRate = 16000;

private:
  void computeCoeffs(float dt);
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n);
  void updateEnvelope(float avgAbs, float dt);
  void updateNormWindow(float ref, float dt);
  void autoGainTick(float dt);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
