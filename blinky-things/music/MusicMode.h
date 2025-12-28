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

    // Confidence tracking (named constants for magic numbers)
    float confidenceIncrement = 0.1f;   // Confidence gain per good beat
    float confidenceDecrement = 0.1f;   // Confidence loss per bad/missed beat
    float phaseErrorTolerance = 0.2f;   // Max phase error to count as "good beat" (0-0.5)
    float missedBeatTolerance = 1.5f;   // Beat period multiplier for missed beat detection

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

    /**
     * Provide external BPM guidance (e.g., from RhythmAnalyzer)
     * - Only affects BPM if external estimate is confident and within range
     * - Smoothly blends external BPM with current PLL estimate
     * - Helps prevent PLL drift during quiet sections
     *
     * @param externalBPM BPM estimate from external source
     * @param confidence Confidence in estimate (0.0-1.0)
     */
    void applyExternalBPMGuidance(float externalBPM, float confidence);

    // ===== GETTERS =====

    inline float getPhase() const { return phase; }
    inline float getBPM() const { return bpm; }
    inline bool isActive() const { return active; }
    inline float getConfidence() const { return confidence_; }
    inline uint32_t getBeatNumber() const { return beatNumber; }

private:
    ISystemTime& time_;

    // ===== TEMPO ESTIMATION (Autocorrelation) =====

    static constexpr uint8_t MAX_INTERVALS = 63;  // One less than onset count (N onsets = N-1 intervals)
    uint16_t onsetIntervals_[MAX_INTERVALS];      // Inter-onset intervals in ms (300-1000ms for 60-200 BPM)
    uint8_t intervalIndex_ = 0;                   // Current write position in circular buffer
    uint8_t intervalCount_ = 0;                   // Number of valid intervals (0-MAX_INTERVALS)
    uint32_t lastOnsetTime_ = 0;                  // Timestamp of last onset (for interval calculation)

    /**
     * Estimate tempo from onset history using simple histogram
     * Called every 8 onsets to reduce CPU usage
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
