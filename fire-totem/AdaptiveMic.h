#pragma once
#include <Arduino.h>
#include <PDM.h>

// AdaptiveMic
// - Output uses raw instantaneous mic average (snappy, no smoothing).
// - Envelope/envMean kept only for normalization + gain adaptation.
// - Hardware gain adapts slowly (minutes).
// - Software AGC adapts over ~10s.
// - Continuous normalization window.

class AdaptiveMic {
public:
  // ---- Tunables ----
  // Envelope smoothing (still used for tracking only)
  float attackSeconds = 0.08f;
  float releaseSeconds = 0.30f;

  // Normalization window
  float normFloorDecay = 0.9995f;
  float normCeilDecay  = 0.9995f;
  float normInset      = 0.02f;
  float noiseGate      = 0.06f;

  // Software AGC
  bool  agEnabled      = true;
  float agTarget       = 0.35f;
  float agStrength     = 0.9f;
  float agMin          = 0.10f;
  float agMax          = 8.0f;

  // Hardware gain (minutes scale)
  uint32_t hwCalibPeriodMs = 60000;
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
  float  globalGain    = 1.0f;  // software AGC multiplier
  int    currentHwGain = 32;    // PDM hardware gain

  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs=250) const {
    return (nowMs - lastIsrMs) > timeoutMs;
  }

  // Debug helpers
  float getLevelInstant() const { return levelInstant; }
  float getLevelPreGate() const { return levelPreGate; }
  float getLevelPostAGC() const { return levelPostAGC; }
  float getEnvMean()     const { return envMean; }
  float getGlobalGain()  const { return globalGain; }
  int   getHwGain()      const { return currentHwGain; }
  uint32_t getIsrCount() const { return s_isrCount; }

public:
  bool begin(uint32_t sampleRate = 16000, int gainInit = 32);
  void end();

  void update(float dt);

  // The main thing FireEffect consumes
  inline float getLevel() const { return levelPostAGC; }

  // Envelope access for debugging
  inline float getEnv() const { return envAR; }

  // ISR hook
  static void onPDMdata();

private:
  // ISR accumulators
  static AdaptiveMic* s_instance;
  volatile static uint32_t s_isrCount;
  volatile static uint64_t s_sumAbs;
  volatile static uint32_t s_numSamples;
  volatile static uint16_t s_maxAbs;

  // Normalization window
  float minEnv = 1e9f;
  float maxEnv = 0.0f;

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
  void consumeISR(float& avgAbs, uint16_t& maxAbs, uint32_t& n);
  void updateEnvelope(float avgAbs, float dt);
  void updateNormWindow(float ref, float dt);
  void autoGainTick(float dt);
  void hardwareCalibrate(uint32_t nowMs, float dt);
  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
