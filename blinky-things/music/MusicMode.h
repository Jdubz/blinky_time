#pragma once

#include <stdint.h>
#include "../hal/interfaces/ISystemTime.h"

/**
 * MusicMode - Beat detection and tempo tracking for LED effects
 *
 * Provides three essential pieces of information:
 * 1. Beat events (quarterNote, halfNote, wholeNote)
 * 2. Beat phase (0.0-1.0 within current beat cycle)
 * 3. Tempo (BPM - beats per minute)
 *
 * Uses:
 * - Autocorrelation for BPM estimation (simple histogram approach)
 * - Phase-Locked Loop (PLL) for beat tracking
 * - Confidence-based activation/deactivation
 *
 * Designed for minimal resource usage:
 * - RAM: ~512 bytes
 * - CPU: ~3% @ 60fps
 */
class MusicMode {
public:
    // ===== PUBLIC STATE (Read-only for generators) =====

    // Mode state
    bool active = false;           // TRUE when music pattern detected
    float bpm = 120.0f;            // Beats per minute (60-200 typical)
    float phase = 0.0f;            // 0.0-1.0 within current beat
    uint32_t beatNumber = 0;       // Increments on each beat

    // Beat events (TRUE for one frame when beat occurs, then auto-cleared)
    bool beatHappened = false;     // Any beat
    bool quarterNote = false;      // Every beat (1/4 note)
    bool halfNote = false;         // Every 2 beats (1/2 note)
    bool wholeNote = false;        // Every 4 beats (whole note)

    // ===== TUNABLE PARAMETERS =====

    // Activation/deactivation
    float activationThreshold = 0.6f;   // Confidence needed to activate (0-1)
    uint8_t minBeatsToActivate = 4;     // Stable beats required
    uint8_t maxMissedBeats = 8;         // Missed beats before deactivation

    // BPM range (prevents false detections)
    float bpmMin = 60.0f;               // Minimum tempo
    float bpmMax = 200.0f;              // Maximum tempo

    // PLL tuning (advanced)
    float pllKp = 0.1f;                 // Proportional gain (responsiveness)
    float pllKi = 0.01f;                // Integral gain (stability)

    // Phase snap tuning (adaptive PLL)
    float phaseSnapThreshold = 0.3f;    // Phase error threshold for snap (vs gradual correction)
    float phaseSnapConfidence = 0.4f;   // Confidence below this enables phase snap
    float stablePhaseThreshold = 0.2f;  // Phase error below this counts as "stable"

    // Confidence tuning
    float confidenceIncrement = 0.1f;   // Confidence gained per stable beat
    float confidenceDecrement = 0.1f;   // Confidence lost per unstable beat
    float missedBeatPenalty = 0.05f;    // Confidence lost per missed beat

    // Tempo estimation tuning (comb filter resonator bank)
    float tempoFilterDecay = 0.95f;     // Comb filter energy decay per frame (0.9-0.99)
    float combFeedback = 0.8f;          // Resonance sharpness (0.5-0.95)
    float combConfidenceThreshold = 0.5f; // Comb filters only update BPM below this confidence
    float histogramBlend = 0.2f;        // Blend factor for histogram tempo estimates (0.1-0.5)

    // BPM locking hysteresis (resists large BPM changes when confident)
    float bpmLockThreshold = 0.7f;      // Confidence above which BPM changes are rate-limited
    float bpmLockMaxChange = 5.0f;      // Max BPM change per second when locked
    float bpmUnlockThreshold = 0.4f;    // Confidence below which to fully unlock

    // ===== CONSTRUCTOR =====

    explicit MusicMode(ISystemTime& time);

    // ===== UPDATE METHODS =====

    /**
     * Update music mode state (call every frame)
     * - Updates beat phase
     * - Checks for missed beats
     * - Handles activation/deactivation
     */
    void update(float dt);

    /**
     * Notify music mode of onset detection from AdaptiveMic
     * - Updates tempo estimation
     * - Corrects beat phase (PLL)
     * - Updates confidence
     */
    void onOnsetDetected(uint32_t timestampMs, bool isLowBand);

    /**
     * Reset to initial state
     */
    void reset();

    // ===== GETTERS =====

    inline float getPhase() const { return phase; }
    inline float getBPM() const { return bpm; }
    inline bool isActive() const { return active; }
    inline float getConfidence() const { return confidence_; }
    inline uint32_t getBeatNumber() const { return beatNumber; }

    // Debug getters for tuning
    inline uint8_t getStableBeats() const { return stableBeats_; }
    inline uint8_t getMissedBeats() const { return missedBeats_; }
    inline float getPeakTempoEnergy() const { return peakTempoEnergy_; }
    inline float getErrorIntegral() const { return errorIntegral_; }

private:
    ISystemTime& time_;

    // ===== TEMPO ESTIMATION (Comb Filter Resonator Bank) =====
    // Replaces histogram-based approach with continuous resonating comb filters
    // Each filter resonates at a specific tempo, peak energy indicates dominant BPM

    static constexpr uint8_t MAX_INTERVALS = 63;  // One less than onset count (N onsets = N-1 intervals)
    uint16_t onsetIntervals_[MAX_INTERVALS];      // Inter-onset intervals in ms (300-1000ms for 60-200 BPM)
    uint8_t intervalIndex_ = 0;                   // Current write position in circular buffer
    uint8_t intervalCount_ = 0;                   // Number of valid intervals (0-MAX_INTERVALS)
    uint32_t lastOnsetTime_ = 0;                  // Timestamp of last onset (for interval calculation)

    // Comb filter resonator bank for continuous tempo tracking
    static constexpr int NUM_TEMPO_FILTERS = 24;  // 60-200 BPM in ~6 BPM steps
    static constexpr int COMB_DELAY_SIZE = 128;   // ~2 seconds of onset history at 60fps

    float tempoEnergy_[NUM_TEMPO_FILTERS] = {0};  // Accumulated energy per tempo hypothesis
    float peakTempoEnergy_ = 0.0f;                // Peak energy for debugging (updated in updateTempoFilters)
    float combDelayLine_[COMB_DELAY_SIZE] = {0};  // Onset strength history for comb feedback
    uint8_t combDelayIdx_ = 0;                    // Current write position in delay line
    float lastOnsetStrength_ = 0.0f;              // Onset strength from last onOnsetDetected

    /**
     * Convert filter index to BPM (60-200 BPM range)
     */
    inline float filterIndexToBPM(int idx) const {
        return 60.0f + idx * (140.0f / (NUM_TEMPO_FILTERS - 1));
    }

    /**
     * Convert BPM to period in frames (at ~60fps)
     */
    inline float bpmToFramePeriod(float targetBpm) const {
        return 60.0f / targetBpm * 60.0f;  // frames per beat
    }

    /**
     * Update comb filter resonators with current onset strength
     * Called every frame for continuous tempo tracking
     *
     * NOTE: Only updates BPM when confidence < 0.5 (acquisition phase)
     * When confidence is high, PLL in onOnsetDetected() is primary tracker
     * When BPM is locked, changes are rate-limited to bpmLockMaxChange/sec
     */
    void updateTempoFilters(float onsetStrength, float dt);

    /**
     * Estimate tempo from onset history using simple histogram
     * Called every 8 onsets to reduce CPU usage (backup method)
     */
    void estimateTempo();

    // ===== PHASE TRACKING (Phase-Locked Loop) =====

    float beatPeriodMs_ = 500.0f;   // Period of one beat in milliseconds
    float errorIntegral_ = 0.0f;    // PLL integral term (for Ki)

    /**
     * Update beat phase based on elapsed time
     * Triggers beat events when phase wraps
     */
    void updatePhase(float dt);

    /**
     * Adjust beat period based on phase error (PLL correction)
     */
    void adjustPeriod(float error);

    // ===== CONFIDENCE TRACKING =====

    float confidence_ = 0.0f;       // 0.0-1.0 pattern confidence
    uint8_t stableBeats_ = 0;       // Count of consecutive stable beats
    uint8_t missedBeats_ = 0;       // Count of consecutive missed beats
    uint32_t lastMissedBeatCheck_ = 0;  // Last time we checked for missed beats

    // BPM locking state
    bool bpmLocked_ = false;        // Current lock state (hysteresis)

    /**
     * Update confidence based on phase error
     */
    void updateConfidence(float phaseError);

    /**
     * Check if music mode should activate
     */
    bool shouldActivate() const;

    /**
     * Check if music mode should deactivate
     */
    bool shouldDeactivate() const;

    // ===== UTILITIES =====

    inline float clampFloat(float value, float min, float max) const {
        return value < min ? min : (value > max ? max : value);
    }

    inline float absFloat(float value) const {
        return value < 0.0f ? -value : value;
    }

    inline uint8_t minInt(uint8_t a, uint8_t b) const {
        return a < b ? a : b;
    }
};
