#pragma once

#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// Utility functions for integer/float clamp to avoid pulling in math.h
static inline float maxValue(float a, float b) { return (a > b) ? a : b; }
static inline int constrainValue(int val, int lo, int hi) { return val < lo ? lo : (val > hi ? hi : val); }
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

namespace MicConstants {
    // Exponential tracking limits (prevent division by zero)
    constexpr float MIN_TAU_RANGE = 0.005f;     // Minimum tau for range tracking (5ms)

    // Window/range normalization — minimum range to prevent divide-by-zero
    // and control noise amplification at very low signal levels
    constexpr float MIN_NORMALIZATION_RANGE = 0.01f;

    // Valley tracking
    constexpr float VALLEY_FLOOR = 0.001f;                    // Minimum valley (0.1% of full scale)
    constexpr float VALLEY_RELEASE_MULTIPLIER = 4.0f;         // How much slower valley rises vs peak
    constexpr float INSTANT_ADAPT_THRESHOLD = 1.3f;           // Jump peak if signal exceeds by 30%

    // Timing
    constexpr float MIN_DT_SECONDS = 0.0005f;                 // Minimum dt clamp (0.5ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;                 // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;             // PDM alive check timeout
}

/**
 * AdaptiveMic — Microphone input with window/range normalization
 *
 * Hardware gain is fixed at the platform's optimal level (nRF52840: 40, ESP32-S3: 30).
 * No AGC — the window/range normalization handles all dynamic range adaptation.
 * This creates identical signal processing on both platforms and eliminates
 * competing adaptation systems that cause visible visual disruption.
 *
 * Window/Range Normalization:
 *   peak/valley tracking maps raw 0-32768 ADC range to normalized 0-1 output
 *   with asymmetric attack/release for natural dynamics.
 */
class AdaptiveMic {
public:
  // ---- Tunables (window/range normalization) ----
  float peakTau        = 2.0f;      // Peak adaptation speed (attack time, seconds)
  float releaseTau     = 5.0f;      // Peak release speed (release time, seconds)

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, normalized via adaptive peak/valley tracking)
  int    currentHardwareGain = Platform::Microphone::DEFAULT_GAIN;    // Fixed PDM hardware gain

  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs = MicConstants::MIC_DEAD_TIMEOUT_MS) const {
    return (int32_t)(nowMs - lastIsrMs) > (int32_t)timeoutMs;
  }

  // Public getters
  inline float getLevel() const { return level; }
  inline float getPeakLevel() const { return peakLevel; }
  inline float getValleyLevel() const { return valleyLevel; }
  inline float getRawLevel() const { return rawInstantLevel; }
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }
  inline bool isPdmAlive() const { return pdmAlive; }

public:
  AdaptiveMic(IPdmMic& pdm, ISystemTime& time);

  bool begin(uint32_t sampleRate = Platform::Microphone::DEFAULT_SAMPLE_RATE,
             int gainInit = Platform::Microphone::DEFAULT_GAIN);
  void end();

  void update(float dt);

  /**
   * Get samples from ISR ring buffer for external FFT consumers
   * Used by AudioController to feed SharedSpectralAnalysis
   */
  int getSamplesForExternal(int16_t* buffer, int maxCount);

  // ISR hook
  static void onPDMdata();

private:
  IPdmMic& pdm_;
  ISystemTime& time_;

  // ISR accumulators
  static AdaptiveMic* s_instance;
  volatile static uint32_t s_isrCount;
  volatile static uint64_t s_sumAbs;
  volatile static uint32_t s_numSamples;
  volatile static uint16_t s_maxAbs;

  // FFT sample ring buffer
  static constexpr int FFT_RING_SIZE = 512;
  volatile static int16_t s_fftRing[FFT_RING_SIZE];
  volatile static uint32_t s_fftWriteIdx;
  static uint32_t s_extFftReadIdx;

  // Window/Range tracking
  float peakLevel = 0.0f;
  float valleyLevel = 0.0f;
  float rawInstantLevel = 0.0f;

  uint32_t _sampleRate = 16000;

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n);
};
