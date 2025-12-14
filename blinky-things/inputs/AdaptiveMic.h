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
  float  globalGain    = 3.0f;  // software AGC multiplier (start higher)
  int    currentHardwareGain = 32;    // PDM hardware gain

  // --- Enhanced Musical Analysis ---
  // Frequency-aware transient detection (tuned for punchy response)
  float transient          = 0.0f;
  float transientDecay     = 8.0f;    // faster decay for snappy response
  float fastAvg            = 0.0f;    // short-term avg (~10ms)
  float slowAvg            = 0.0f;    // medium-term avg (~150ms)
  float fastAlpha          = 0.35f;   // faster tracking for quick attacks
  float slowAlpha          = 0.025f;  // medium-term baseline
  float transientFactor    = 1.5f;    // Threshold: level must be 1.5x above baseline
  float loudFloor          = 0.05f;   // lower floor for sensitivity
  uint32_t transientCooldownMs = 120; // ~8 hits/sec max for fast beats
  uint32_t lastTransientMs = 0;

  // Frequency band analysis (simplified 4-band)
  float bassLevel    = 0.0f;    // 20-250 Hz
  float midLevel     = 0.0f;    // 250-2000 Hz
  float highLevel    = 0.0f;    // 2000-8000 Hz
  float spectralCentroid = 0.0f; // weighted frequency center

  // Musical environment adaptation
  float bassWeight   = 1.0f;    // weighting for bass-heavy music
  float percWeight   = 1.0f;    // weighting for percussive content
  float ambientNoise = 0.0f;    // background noise floor

  // Dynamic range compression
  float compRatio    = 4.0f;    // compression ratio for loud environments
  float compThresh   = 0.7f;    // compression threshold
  float compAttack   = 0.003f;  // compressor attack time
  float compRelease  = 0.1f;    // compressor release time
  float compGain     = 1.0f;    // makeup gain after compression

  // Environment classification
  enum AudioEnvironment {
    ENV_UNKNOWN = 0,
    ENV_QUIET,      // library, bedroom
    ENV_AMBIENT,    // office, cafe background
    ENV_MODERATE,   // normal conversation
    ENV_LOUD,       // party, restaurant
    ENV_CONCERT,    // live music, club
    ENV_EXTREME     // very loud club, outdoor festival
  };
  AudioEnvironment currentEnv = ENV_UNKNOWN;
  uint32_t envConfidence = 0;   // confidence counter for environment detection

  float getTransient() const { return transient; }

  // Time-domain frequency approximation getters (not real FFT)
  float getApproxBassLevel() const { return bassLevel; }
  float getApproxMidLevel() const { return midLevel; }
  float getApproxHighLevel() const { return highLevel; }
  float getSpectralCentroid() const { return spectralCentroid; }
  AudioEnvironment getNoiseClassification() const { return currentEnv; }
  float getAmbientNoise() const { return ambientNoise; }

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
  int   getHwGain()      const { return currentHardwareGain; }
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

  // Frequency analysis buffers
  static const int FREQ_BUFFER_SIZE = 128;  // Small FFT for Arduino
  float freqBuffer[FREQ_BUFFER_SIZE];
  int freqBufferIndex = 0;
  bool freqBufferReady = false;

  // Compressor state
  float compEnvelope = 0.0f;

  // Environment detection history
  float envHistory[8] = {0};  // Rolling history for environment classification
  int envHistoryIndex = 0;

  // Musical pattern detection
  float beatHistory[16] = {0};  // Beat detection history
  int beatHistoryIndex = 0;
  uint32_t lastBeatMs = 0;
  float estimatedBPM = 0.0f;

private:
  void computeCoeffs(float dt);
  void consumeISR(float& avgAbs, uint16_t& maxAbs, uint32_t& n);
  void updateEnvelope(float avgAbs, float dt);
  void updateNormWindow(float ref, float dt);
  void autoGainTick(float dt);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  // Enhanced musical analysis methods
  void analyzeFrequencySpectrum(float avgAbs);
  void updateEnvironmentClassification(float dt);
  void detectMusicalPatterns(float level, uint32_t nowMs);
  void applyDynamicRangeCompression(float& level);
  void adaptToEnvironment();

  // Simple FFT-like analysis for frequency bands
  void computeSpectralBands();
  float computeSpectralCentroid();

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
