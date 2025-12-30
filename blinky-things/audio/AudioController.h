#pragma once

#include "AudioControl.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

/**
 * AudioController - Unified audio analysis and control signal synthesis
 *
 * Combines microphone input processing and rhythm analysis into a single
 * interface that outputs a simple 4-parameter AudioControl struct.
 *
 * Generators and the main sketch should ONLY interact with AudioControl.
 * All audio analysis internals (transient detection, beat tracking, tempo
 * estimation) are encapsulated here.
 *
 * Architecture:
 *   PDM Microphone
 *        |
 *   AdaptiveMic (level, transient, spectral flux)
 *        |
 *   RhythmTracker (BPM estimation, phase tracking)
 *        |
 *   AudioControl { energy, pulse, phase, rhythmStrength }
 *        |
 *   Generators
 *
 * Memory: ~6 KB (AdaptiveMic + rhythm tracking buffers)
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

    // Detection mode (affects transient sensitivity)
    void setDetectionMode(uint8_t mode);
    uint8_t getDetectionMode() const;

    // BPM range constraints
    void setBpmRange(float minBpm, float maxBpm);
    float getBpmMin() const { return bpmMin_; }
    float getBpmMax() const { return bpmMax_; }

    // Get current BPM estimate (for debugging/display)
    float getCurrentBpm() const { return bpm_; }

    // Hardware gain control (for testing)
    void lockHwGain(int gain);
    void unlockHwGain();
    bool isHwGainLocked() const;
    int getHwGain() const;

    // === TUNING PARAMETERS ===
    // Exposed for SerialConsole tuning

    // Rhythm tracking sensitivity
    float activationThreshold = 0.5f;   // Confidence needed to activate rhythm mode
    float pllKp = 0.15f;                // Phase-locked loop proportional gain
    float pllKi = 0.02f;                // Phase-locked loop integral gain

    // Beat alignment pulse modulation
    float pulseBoostOnBeat = 1.3f;      // Boost factor for on-beat transients
    float pulseSuppressOffBeat = 0.6f;  // Suppress factor for off-beat transients

    // Energy boost during rhythm lock
    float energyBoostOnBeat = 0.3f;     // Energy boost on detected beats

    // === ADVANCED ACCESS (for debugging/tuning only) ===
    // These expose internal state but should NOT be used by generators

    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }

    // Debug getters
    float getPhaseError() const { return lastPhaseError_; }
    float getConfidence() const { return confidence_; }
    float getPeriodicityStrength() const { return periodicityStrength_; }

private:
    // === HAL REFERENCES ===
    ISystemTime& time_;

    // === MICROPHONE ===
    AdaptiveMic mic_;

    // === RHYTHM TRACKING STATE ===

    // BPM constraints
    float bpmMin_ = 60.0f;
    float bpmMax_ = 200.0f;

    // Tempo estimation (autocorrelation on onset strength)
    static constexpr int OSS_BUFFER_SIZE = 256;  // ~4.3 seconds at 60 Hz
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};     // Onset Strength Signal history
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // Current tempo estimate
    float bpm_ = 120.0f;
    float beatPeriodMs_ = 500.0f;
    float periodicityStrength_ = 0.0f;

    // Phase tracking (simplified PLL)
    float phase_ = 0.0f;                // Current beat phase (0-1)
    float errorIntegral_ = 0.0f;        // PLL integral term
    float lastPhaseError_ = 0.0f;       // For debugging
    uint32_t lastOnsetMs_ = 0;          // Last onset timestamp

    // Confidence tracking
    float confidence_ = 0.0f;           // Overall rhythm confidence (0-1)
    float confidenceSmooth_ = 0.0f;     // Smoothed confidence for output

    // Autocorrelation throttling
    uint32_t lastAutocorrMs_ = 0;
    static constexpr uint32_t AUTOCORR_PERIOD_MS = 1000;  // Run autocorrelation every 1 second

    // === SYNTHESIZED OUTPUT ===
    AudioControl control_;

    // === INTERNAL METHODS ===

    // Rhythm tracking
    void addOssSample(float onsetStrength);
    void runAutocorrelation(uint32_t nowMs);
    void updatePhase(float dt, uint32_t nowMs);
    void onTransientDetected(uint32_t nowMs, float strength);

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
