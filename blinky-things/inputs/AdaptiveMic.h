#pragma once
#include <stdint.h>
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// AdaptiveMic - Clean audio input with window/range normalization
// - Provides raw, unsmoothed audio level (generators can smooth if needed)
// - Auto-ranging: Tracks peak/valley, maps to 0-1 output (no clipping)
// - Hardware gain adapts to environment over minutes
// - Provides raw PCM samples for EnsembleDetector via getSamplesForExternal()
// NOTE: Transient detection has been moved to EnsembleDetector

namespace MicConstants {
    constexpr float MIN_DT_SECONDS = 0.0001f;         // Minimum dt clamp (0.1ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;         // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;     // PDM alive check timeout

    // Timing constants (not user-configurable)
    constexpr uint32_t HW_CALIB_PERIOD_MS = 30000;    // Hardware gain calibration period (30s)
    constexpr float HW_TRACKING_TAU = 30.0f;          // Hardware gain tracking time constant (30s)
}

/**
 * AdaptiveMic - Audio input with window/range normalization
 *
 * Provides normalized audio level (0-1) with automatic gain control.
 * Raw PCM samples available via getSamplesForExternal() for spectral analysis.
 *
 * Architecture:
 * - Raw samples → Normalize (0-1) → Window/Range mapping → Output
 * - Hardware-primary: HW gain optimizes ADC input, window/range maps to 0-1 output
 *
 * Window/Range Normalization:
 * - Peak tracker follows actual signal levels (exponential moving average)
 * - Fast attack, slow release for natural loudness following
 * - Output = (signal - valleyLevel) / (peak - valleyLevel) → always 0-1
 *
 * NOTE: Transient detection moved to EnsembleDetector (audio/EnsembleDetector.h)
 */
class AdaptiveMic {
public:
  // ---- Tunables ----
  // Window/Range auto-normalization - Peak/valley tracking
  float peakTau        = 2.0f;      // Peak adaptation speed (attack time, seconds)
  float releaseTau     = 5.0f;      // Peak release speed (release time, seconds)

  // Hardware gain target (adapts to raw ADC input for best signal quality)
  // Note: hwGainMin/Max are hardware limits (0-80 for nRF52840 PDM), not configurable
  // Dead zone: ±0.01 around target (no adjustment if within range)
  float    hwTarget = 0.35f;   // Target raw input level for optimal ADC quality

  // Fast AGC for low-level sources (accelerates calibration when signal is persistently low)
  bool     fastAgcEnabled = true;       // Enable fast AGC when signal is low and gain is high
  float    fastAgcThreshold = 0.15f;    // Raw level threshold to trigger fast AGC
  uint16_t fastAgcPeriodMs = 5000;      // Calibration period in fast mode (5s vs 30s)
  float    fastAgcTrackingTau = 5.0f;   // Tracking tau in fast mode (5s vs 30s)

  // Loud AGC mode for high-SPL environments (symmetric to fast AGC for low-SPL)
  // Automatically triggered when hardware gain bottoms out
  uint8_t hwGainMinHeadroom = 10;       // Min hardware gain (headroom floor)
  float   hwLoudThreshold = 0.60f;      // Trigger loud mode when rawLevel > this at low gain
  float   valleyFastTrackRatio = 2.0f;  // Faster valley tracking in loud mode (vs 4.0 normal)

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, normalized via adaptive peak/valley tracking)
  int    currentHardwareGain = Platform::Microphone::DEFAULT_GAIN;    // PDM hardware gain


  // Debug/health
  uint32_t lastIsrMs = 0;
  bool     pdmAlive  = false;
  inline bool isMicDead(uint32_t nowMs, uint32_t timeoutMs = MicConstants::MIC_DEAD_TIMEOUT_MS) const {
    // Use signed arithmetic to handle millis() wraparound at 49.7 days
    return (int32_t)(nowMs - lastIsrMs) > (int32_t)timeoutMs;
  }

  // Public getters
  inline float getLevel() const { return level; }
  inline float getPeakLevel() const { return peakLevel; }      // Current tracked peak (raw 0-1 range)
  inline float getValleyLevel() const { return valleyLevel; }  // Current tracked valley (raw 0-1 range)
  inline float getRawLevel() const { return rawInstantLevel; } // Instantaneous raw ADC level (for debugging)
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }
  inline bool isPdmAlive() const { return pdmAlive; }
  inline bool isHwGainLocked() const { return hwGainLocked_; }     // Check if hardware gain is locked for testing
  inline bool isInFastAgcMode() const { return inFastAgcMode_; }   // Check if fast AGC is active
  inline bool isInLoudAgcMode() const { return inLoudAgcMode_; }   // Check if loud AGC is active

public:
  /**
   * Construct with HAL dependencies for testability
   */
  AdaptiveMic(IPdmMic& pdm, ISystemTime& time);

  bool begin(uint32_t sampleRate = Platform::Microphone::DEFAULT_SAMPLE_RATE,
             int gainInit = Platform::Microphone::DEFAULT_GAIN);
  void end();

  void update(float dt);

  // Hardware gain lock/unlock for testing (bypasses AGC)
  void lockHwGain(int gain);    // Lock hardware gain at specific value (disables AGC)
  void unlockHwGain();          // Unlock hardware gain (re-enables AGC)

  /**
   * Get samples from ISR ring buffer for external FFT consumers
   * Used by EnsembleDetector to feed SharedSpectralAnalysis
   * @param buffer Destination buffer for samples (must be at least maxCount)
   * @param maxCount Maximum number of samples to retrieve
   * @return Number of samples actually retrieved
   */
  int getSamplesForExternal(int16_t* buffer, int maxCount);

  // ISR hook
  static void onPDMdata();

private:
  // HAL references
  IPdmMic& pdm_;
  ISystemTime& time_;

  // ISR accumulators - written by ISR, read/reset by main thread
  // Thread safety: consumeISR() disables interrupts during read to prevent
  // torn reads of multi-byte values. The volatile keyword ensures the
  // compiler doesn't optimize away reads/writes.
  //
  // Wraparound behavior: s_numSamples and s_sumAbs can technically overflow
  // after ~75 hours at 16kHz (2^32 samples). In practice, consumeISR() is
  // called every frame (~16ms), resetting accumulators before overflow.
  // Even if overflow occurred, the ratio-based calculations (average level)
  // would still produce valid results due to integer wraparound semantics.
  static AdaptiveMic* s_instance;
  volatile static uint32_t s_isrCount;
  volatile static uint64_t s_sumAbs;        // Sum of absolute sample values (uint64 prevents overflow)
  volatile static uint32_t s_numSamples;    // Count of samples processed (reset each frame)
  volatile static uint16_t s_maxAbs;

  // FFT sample ring buffer (ISR writes, main thread reads)
  // Size must be >= FFT_SIZE (256) to ensure we capture a full frame
  static constexpr int FFT_RING_SIZE = 512;  // Power of 2 for efficient modulo
  volatile static int16_t s_fftRing[FFT_RING_SIZE];
  volatile static uint32_t s_fftWriteIdx;    // ISR increments this
  static uint32_t s_extFftReadIdx;           // External FFT consumers (EnsembleDetector)

  // Window/Range tracking
  float peakLevel = 0.0f;        // Tracked peak for range window
  float valleyLevel = 0.0f;      // Tracked valley for range window (typically noise gate)
  float rawTrackedLevel = 0.0f;  // Raw ADC level tracked for hardware AGC (slow, 30s tau)
  float rawInstantLevel = 0.0f;  // Instantaneous raw level (for debugging)

  // Timing
  uint32_t lastHwCalibMs = 0;

  uint32_t _sampleRate = 16000;

  // Hardware gain lock state (for testing/bypass)
  bool hwGainLocked_ = false;   // When true, AGC is disabled and gain is fixed

  // Fast AGC state
  bool inFastAgcMode_ = false;  // Currently in fast AGC mode

  // Loud AGC state
  bool inLoudAgcMode_ = false;  // Currently in loud AGC mode (symmetric to inFastAgcMode_)

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
