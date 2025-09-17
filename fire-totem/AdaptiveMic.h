#pragma once
#include <Arduino.h>
#include <PDM.h>

// Refactored AdaptiveMic
// - Slow hardware gain adaptation over minutes
// - Fast software AGC for musical dynamics over ~10s
// - Envelope with attack/release
// - Continuous normalization window (no hard resets on gain change)

class AdaptiveMic {
public:
  // ---- Tunables (safe defaults) ----
  // Envelope
  float attackSeconds = 0.08f;   // fast attack
  float releaseSeconds = 0.30f;  // slower release

  // Normalization window behavior
  float normFloorDecay = 0.9995f; // how slowly minEnv drifts up (closer to 1.0 = slower)
  float normCeilDecay  = 0.9995f; // how slowly maxEnv drifts down
  float normInset      = 0.02f;   // keep some headroom inside min/max
  float noiseGate      = 0.06f;   // gate after normalization (pre-software-gain)

  // Software AGC (seconds scale)
  bool  agEnabled      = true;
  float agTarget       = 0.35f;   // desired normalized level (pre-gamma)
  float agStrength     = 0.9f;    // integral gain (tune for ~5–10s)
  float agMin          = 0.10f;   // lower bound for software gain
  float agMax          = 8.0f;    // upper bound for software gain

  // Hardware gain (minutes scale)
  uint32_t hwCalibPeriodMs = 60000; // 1 min per step
  float    envTargetRaw    = 1000.0f; // long-term raw target (matches your prior code)
  float    envLowRatio     = 0.50f; // below 50% => too quiet
  float    envHighRatio    = 1.50f; // above 150% => too loud
  int      hwGainMin       = 0;
  int      hwGainMax       = 64;
  int      hwGainStep      = 1;     // creep 1 step per period

  // Limit dwell heuristics to coordinate HW <-> SW
  float limitDwellTriggerSec = 8.0f; // if sw gain pinned at limit for this long, allow HW step
  float limitDwellRelaxSec   = 3.0f; // need this long away from limit to reset dwell

  // Public state you may want to log
  float  levelPreGate  = 0.0f;  // normalized 0..1 before gate/gamma & before SW gain
  float  levelPostAGC  = 0.0f;  // normalized 0..1 after software gain and gate (feed FireEffect)
  float  envAR         = 0.0f;  // attack/release envelope (raw units)
  float  envMean       = 0.0f;  // very slow EMA of envAR (raw units)
  float  globalGain    = 1.0f;  // software AGC multiplier
  int    currentHwGain = 32;    // PDM hardware gain

public:
  bool begin(uint32_t sampleRate = 16000, int gainInit = 32);
  void end();

  // Call each frame with dt (seconds)
  void update(float dt);

  // The main thing you’ll use for the fire effect (0..1)
  inline float getLevel() const { return levelPostAGC; }

  // If you need a raw-ish measure (already enveloped), here it is:
  inline float getEnv() const { return envAR; }

  // ISR hook
  static void onPDMdata();

private:
  // Aggregators filled in ISR, consumed in update()
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

  // Software gain limit dwell timers
  float dwellAtMin = 0.0f;
  float dwellAtMax = 0.0f;

  // Cached coefficients (recomputed if attack/release change)
  float aAtk = 0.0f, aRel = 0.0f;
  uint32_t _sampleRate = 16000;

private:
  void computeCoeffs(float dt);
  void consumeISR(float& avgAbs, uint16_t& maxAbs, uint32_t& n);
  void updateEnvelope(float avgAbs, float dt);
  void updateNormWindow(float dt);
  void autoGainTick(float dt);
  void hardwareCalibrate(uint32_t nowMs, float dt);
  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
