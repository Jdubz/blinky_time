#pragma once

#include "AudioControl.h"
#include "EnsembleDetector.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

/**
 * AudioController - Unified audio analysis and control signal synthesis
 *
 * Combines microphone input processing and rhythm analysis into a single
 * interface that outputs a simple 4-parameter AudioControl struct.
 *
 * Rhythm Tracking Approach (pattern-based, not event-based):
 *   1. Buffer 6 seconds of onset strength (spectral flux)
 *   2. Run autocorrelation to find periodicity and tempo
 *   3. Derive phase from autocorrelation pattern
 *   4. Predict beats ahead of time
 *
 * Key Design Decision:
 *   Transient detection drives VISUAL PULSE output only.
 *   Beat tracking is derived from buffered pattern analysis.
 *   This prevents unreliable transients from disrupting beat sync.
 *
 * Architecture:
 *   PDM Microphone
 *        |
 *   AdaptiveMic (level normalization, gain control)
 *        |
 *   EnsembleDetector (6 detectors + fusion)
 *        |
 *   OSS Buffer (6s) --> Autocorrelation --> Tempo + Phase
 *        |                                      |
 *   Ensemble Transient --> Pulse (visual only)  |
 *        |                                      |
 *   AudioControl { energy, pulse, phase, rhythmStrength }
 *        |
 *   Generators
 *
 * FRAME RATE ASSUMPTION:
 *   The OSS buffer and autocorrelation calculations assume ~60 Hz frame rate.
 *   - OSS_BUFFER_SIZE = 360 samples = 6 seconds @ 60 Hz
 *   - BPM-to-lag conversion: lag = 60 / bpm * 60 (assumes 60 Hz)
 *   If frame rate drops significantly (e.g., due to serial flooding or
 *   heavy LED updates), BPM estimates will drift. Phase tracking uses
 *   actual dt values, so phase remains accurate even with variable frame rate.
 *
 * Memory: ~6.5 KB (AdaptiveMic + 360-sample OSS buffer)
 * CPU: ~5-6% @ 64 MHz with FFT enabled
 */
class AudioController {
public:
    // === CONSTRUCTION ===
    AudioController(IPdmMic& pdm, ISystemTime& time);
    ~AudioController();

    // === LIFECYCLE ===
    bool begin(uint32_t sampleRate = 16000);
    void end();

    // === UPDATE (call every frame) ===
    /**
     * Update all audio analysis and return synthesized control signal.
     * Call this once per frame with delta time in seconds.
     */
    const AudioControl& update(float dt);

    // === OUTPUT ===
    /**
     * Get current control signal (read-only).
     * Valid after update() is called.
     */
    const AudioControl& getControl() const { return control_; }

    // === CONFIGURATION ===

    // Ensemble detector configuration
    void setDetectorEnabled(DetectorType type, bool enabled);
    void setDetectorWeight(DetectorType type, float weight);
    void setDetectorThreshold(DetectorType type, float threshold);

    // BPM range constraints
    void setBpmRange(float minBpm, float maxBpm);
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }

    // Get current BPM estimate (for debugging/display)
    float getCurrentBpm() const { return bpm_; }

    // Hardware gain control (for testing)
    void lockHwGain(int gain);
    void unlockHwGain();
    bool isHwGainLocked() const;
    int getHwGain() const;

    // === TUNING PARAMETERS ===
    // Exposed for SerialConsole tuning

    // Rhythm activation threshold (periodicity strength needed to activate)
    float activationThreshold = 0.4f;

    // Beat alignment pulse modulation (visual effect only)
    float pulseBoostOnBeat = 1.3f;      // Boost factor for on-beat transients
    float pulseSuppressOffBeat = 0.6f;  // Suppress factor for off-beat transients

    // Energy boost during rhythm lock
    float energyBoostOnBeat = 0.3f;     // Energy boost near predicted beats

    // Phase tracking smoothing
    float phaseAdaptRate = 0.15f;       // How quickly phase adapts to autocorrelation (0-1)

    // BPM range for autocorrelation tempo detection
    float bpmMin = 60.0f;               // Minimum BPM to detect (affects autocorr lag range)
    float bpmMax = 200.0f;              // Maximum BPM to detect (affects autocorr lag range)

    // === ADVANCED ACCESS (for debugging/tuning only) ===

    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }

    EnsembleDetector& getEnsemble() { return ensemble_; }
    const EnsembleDetector& getEnsemble() const { return ensemble_; }

    // Get last ensemble output for debugging
    const EnsembleOutput& getLastEnsembleOutput() const { return ensemble_.getLastOutput(); }

    // Debug getters
    float getPeriodicityStrength() const { return periodicityStrength_; }

private:
    // === HAL REFERENCES ===
    ISystemTime& time_;

    // === MICROPHONE ===
    AdaptiveMic mic_;

    // === ENSEMBLE DETECTOR ===
    EnsembleDetector ensemble_;
    EnsembleOutput lastEnsembleOutput_;

    // === RHYTHM TRACKING STATE ===

    // Onset Strength Signal buffer (6 seconds at 60 Hz frame rate)
    static constexpr int OSS_BUFFER_SIZE = 360;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // Current tempo estimate
    float bpm_ = 120.0f;
    float beatPeriodMs_ = 500.0f;
    float periodicityStrength_ = 0.0f;

    // Phase tracking (derived from autocorrelation, not from transients)
    float phase_ = 0.0f;                // Current beat phase (0-1)
    float targetPhase_ = 0.0f;          // Phase derived from autocorrelation

    // Autocorrelation timing
    uint32_t lastAutocorrMs_ = 0;
    static constexpr uint32_t AUTOCORR_PERIOD_MS = 500;  // Run every 500ms

    // Silence detection
    uint32_t lastSignificantAudioMs_ = 0;

    // Level tracking for non-FFT modes
    float prevLevel_ = 0.0f;

    // === SYNTHESIZED OUTPUT ===
    AudioControl control_;

    // === INTERNAL METHODS ===

    // Rhythm tracking
    void addOssSample(float onsetStrength);
    void runAutocorrelation(uint32_t nowMs);
    void updatePhase(float dt, uint32_t nowMs);

    // Output synthesis
    void synthesizeEnergy();
    void synthesizePulse();
    void synthesizePhase();
    void synthesizeRhythmStrength();

    // Utilities
    inline float clampf(float val, float minVal, float maxVal) const {
        return val < minVal ? minVal : (val > maxVal ? maxVal : val);
    }

    inline float absf(float val) const {
        return val < 0.0f ? -val : val;
    }
};
