#pragma once
#include <stdint.h>
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"
#include "DetectionMode.h"
#include "BiquadFilter.h"
#include "SpectralFlux.h"

// AdaptiveMic - Clean audio input with window/range normalization and transient detection
// - Provides raw, unsmoothed audio level (generators can smooth if needed)
// - Auto-ranging: Tracks peak/valley, maps to 0-1 output (no clipping)
// - Simplified transient detection: amplitude spike detection (LOUD + SUDDEN + INFREQUENT)
// - Hardware gain adapts to environment over minutes
//
// NOTE: "Transient detection" is the simplified term for what was previously called "onset detection".
// MusicMode still uses onOnsetDetected() callback for historical reasons, but both refer to the same concept.

namespace MicConstants {
    constexpr float MIN_DT_SECONDS = 0.0001f;         // Minimum dt clamp (0.1ms)
    constexpr float MAX_DT_SECONDS = 0.1000f;         // Maximum dt clamp (100ms)
    constexpr uint32_t MIC_DEAD_TIMEOUT_MS = 250;     // PDM alive check timeout

    // Timing constants (not user-configurable)
    constexpr uint32_t ONSET_COOLDOWN_MS = 30;        // Onset detection cooldown (30ms = 33 hits/sec max, tuned 2024-12)
    constexpr uint32_t HW_CALIB_PERIOD_MS = 30000;    // Hardware gain calibration period (30s)
    constexpr float HW_TRACKING_TAU = 30.0f;          // Hardware gain tracking time constant (30s)
}

/**
 * AdaptiveMic - Audio input with window/range normalization and transient detection
 *
 * Architecture:
 * - Raw samples → Normalize (0-1) → Window/Range mapping → Noise gate → Output
 * - Simplified transient detection: "The Drummer's Algorithm"
 *   - LOUD: Signal significantly louder than recent average (2x default, tuned 2024-12)
 *   - SUDDEN: Rapidly rising (20% increase from previous frame)
 *   - INFREQUENT: Cooldown prevents double-triggers (30ms default)
 * - Hardware-primary: HW gain optimizes ADC input, window/range maps to 0-1 output
 *
 * Transient Detection Algorithm:
 * - Tracks recent average level with exponential moving average
 * - Detects amplitude spikes that are LOUD + SUDDEN + past cooldown
 * - Returns strength: 0.0 at threshold, 1.0 at 2x threshold
 * - Focuses on musical events (kicks, snares, bass drops), not waveform analysis
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

  // Hardware gain target (adapts to raw ADC input for best signal quality)
  // Note: hwGainMin/Max are hardware limits (0-80 for nRF52840 PDM), not configurable
  // Dead zone: ±0.01 around target (no adjustment if within range)
  float    hwTarget = 0.35f;   // Target raw input level for optimal ADC quality

  // ---- Public state ----
  float  level         = 0.0f;  // Final output level (0-1, normalized via adaptive peak/valley tracking)
  int    currentHardwareGain = Platform::Microphone::DEFAULT_GAIN;    // PDM hardware gain

  // SIMPLIFIED TRANSIENT DETECTION
  // Single-frame impulse when amplitude spike detected
  float transient          = 0.0f;    // Impulse strength (0.0 = none, 1.0 = strong hit)
  uint32_t lastTransientMs = 0;

  // Transient detection parameters (tunable) - shared by all algorithms (tuned 2024-12)
  float transientThreshold = 3.5f;    // Must be 3.5x louder than recent average
  float attackMultiplier   = 1.2f;    // Must be 20% louder than previous frame (rapid rise)
  float averageTau         = 0.8f;    // Recent average tracking time (seconds)
  uint16_t cooldownMs      = 30;      // Cooldown between hits (ms)

  // ---- DETECTION MODE ----
  // Switch between different onset detection algorithms
  uint8_t detectionMode = 0;  // 0=drummer, 1=bass, 2=hfc, 3=flux (use uint8_t for serial registration)

  // Bass Band Filter parameters (mode 1)
  float bassFreq   = 120.0f;   // Filter cutoff frequency (Hz)
  float bassQ      = 1.0f;     // Filter Q factor (0.5=Butterworth, higher=sharper)
  float bassThresh = 3.0f;     // Detection threshold for bass energy

  // High Frequency Content parameters (mode 2)
  float hfcWeight = 1.0f;      // HFC weighting factor
  float hfcThresh = 3.0f;      // Detection threshold for HFC

  // Spectral Flux parameters (mode 3) - tuned 2024-12
  float fluxThresh = 2.0f;     // Detection threshold for spectral flux
  uint8_t fluxBins = 64;       // Number of FFT bins to analyze (focus on bass-mid)

  // Hybrid parameters (mode 4) - tuned via param-tuner 2024-12 (F1: 0.705)
  float hybridFluxWeight = 0.3f;   // Weight when only flux detects (was 0.7)
  float hybridDrumWeight = 0.3f;   // Weight when only drummer detects (was 0.5)
  float hybridBothBoost = 1.2f;    // Multiplier when both agree (1.0-2.0)

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
  inline float getRawLevel() const { return rawInstantLevel; } // Instantaneous raw ADC level (for debugging)
  inline int getHwGain() const { return currentHardwareGain; }
  inline uint32_t getIsrCount() const { return s_isrCount; }
  inline bool isPdmAlive() const { return pdmAlive; }
  inline float getRecentAverage() const { return recentAverage; }  // Recent average level for debugging
  inline float getPreviousLevel() const { return previousLevel; }  // Previous frame level for debugging
  inline bool isHwGainLocked() const { return hwGainLocked_; }     // Check if hardware gain is locked for testing
  inline uint8_t getDetectionMode() const { return detectionMode; }  // Current detection algorithm
  inline float getBassLevel() const { return bassFilteredLevel; }    // Bass-filtered level (for debugging)
  inline float getLastFluxValue() const { return lastFluxValue_; }   // Last spectral flux value (for RhythmAnalyzer)

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

  // FFT sample ring buffer (ISR writes, main thread reads)
  // Size must be >= FFT_SIZE (256) to ensure we capture a full frame
  static constexpr int FFT_RING_SIZE = 512;  // Power of 2 for efficient modulo
  volatile static int16_t s_fftRing[FFT_RING_SIZE];
  volatile static uint32_t s_fftWriteIdx;    // ISR increments this
  static uint32_t s_fftReadIdx;              // Main thread increments this

  // Window/Range tracking
  float peakLevel = 0.0f;        // Tracked peak for range window
  float valleyLevel = 0.0f;      // Tracked valley for range window (typically noise gate)
  float rawTrackedLevel = 0.0f;  // Raw ADC level tracked for hardware AGC (slow, 30s tau)
  float rawInstantLevel = 0.0f;  // Instantaneous raw level (for debugging)

  // Timing
  uint32_t lastHwCalibMs = 0;

  uint32_t _sampleRate = 16000;

  // SIMPLIFIED AMPLITUDE SPIKE DETECTION
  // Ring buffer for attack detection (compare against level from ~50ms ago, not just previous frame)
  // This catches gradual attacks that rise over 50-100ms while still being "sudden" musically
  static constexpr int ATTACK_BUFFER_SIZE = 4;  // 4 frames @ 60Hz = ~67ms lookback
  float attackBuffer[ATTACK_BUFFER_SIZE] = {0};
  int attackBufferIdx = 0;
  bool attackBufferInitialized_ = false;  // Track if buffer has been pre-filled
  float recentAverage = 0.0f;   // Rolling average of audio level (~1 second window)
  float previousLevel = 0.0f;   // Last frame's level (kept for compatibility)

  // Hardware gain lock state (for testing/bypass)
  bool hwGainLocked_ = false;   // When true, AGC is disabled and gain is fixed

  // Bass band filter state
  BiquadFilter bassFilter;
  float bassFilteredLevel = 0.0f;     // Filtered bass level (for envelope tracking)
  float bassRecentAverage = 0.0f;     // Rolling average for bass detection
  bool bassFilterInitialized = false; // Track if filter coefficients are set

  // HFC state
  float hfcRecentAverage = 0.0f;      // Rolling average for HFC detection
  float lastHfcValue = 0.0f;          // Previous HFC value for attack detection

  // Spectral Flux state (FFT-based detection)
  SpectralFlux spectralFlux_;
  float fluxRecentAverage_ = 0.0f;    // Rolling average for flux detection
  float lastFluxValue_ = 0.0f;        // Last computed flux value (for RhythmAnalyzer)

private:
  void consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n, uint32_t& zeroCrossings);
  void hardwareCalibrate(uint32_t nowMs, float dt);

  // Detection method dispatcher (calls appropriate algorithm based on detectionMode)
  void detectTransients(uint32_t nowMs, float dt);

  // Individual detection algorithms
  void detectDrummer(uint32_t nowMs, float dt, float rawLevel);      // Mode 0: Original amplitude-based
  void detectBassBand(uint32_t nowMs, float dt, float rawLevel);     // Mode 1: Bass band filter
  void detectHFC(uint32_t nowMs, float dt, float rawLevel);          // Mode 2: High frequency content
  void detectSpectralFlux(uint32_t nowMs, float dt, float rawLevel); // Mode 3: FFT-based
  void detectHybrid(uint32_t nowMs, float dt, float rawLevel);       // Mode 4: Combined drummer + flux

  // Helper methods for hybrid detection (return detection strength without side effects)
  float evalDrummerStrength(float rawLevel);      // Returns drummer detection strength (0-1)
  float evalSpectralFluxStrength(float flux, bool frameReady);  // Returns spectral flux detection strength (0-1) from cached flux

  inline float clamp01(float x) const { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
};
