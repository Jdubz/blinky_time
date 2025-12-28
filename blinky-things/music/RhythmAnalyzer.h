#pragma once
#include <stdint.h>

/**
 * RhythmAnalyzer - Industry-standard beat tracking via Onset Strength Signal (OSS) buffering
 *
 * Buffers spectral flux output and performs autocorrelation for periodicity detection.
 * This is the proven approach used by librosa, aubio, and BTrack.
 *
 * Key concept: Instead of relying on discrete transient events, we:
 * 1. Buffer continuous onset strength signal (spectral flux)
 * 2. Find periodicity via autocorrelation
 * 3. Predict beat likelihood based on detected pattern
 * 4. Provide tempo estimate independent of discrete events
 *
 * Resources:
 * - RAM: 1 KB (256 frames × 4 bytes)
 * - CPU: ~2% @ 64 MHz (autocorrelation every 1 sec)
 *
 * References:
 * - librosa.onset.onset_strength_multi
 * - Meier et al. (2024) "Real-Time Beat Tracking with Zero Latency"
 * - Alonso et al. (2017) "OBTAIN: Real-Time Beat Tracking"
 */
class RhythmAnalyzer {
public:
    // ===== CONFIGURATION =====

    static constexpr int BUFFER_SIZE = 256;  // 256 frames @ 60 Hz = ~4.3 seconds

    // Tempo range for autocorrelation (matches MusicMode)
    float minBPM = 60.0f;
    float maxBPM = 200.0f;

    // Autocorrelation update rate (reduce CPU by analyzing less frequently)
    uint32_t autocorrUpdateIntervalMs = 1000;  // 1 second

    // Beat likelihood threshold for virtual beat synthesis
    float beatLikelihoodThreshold = 0.7f;

    // Minimum periodicity strength to trust detected tempo
    float minPeriodicityStrength = 0.5f;

    // ===== PUBLIC STATE =====

    // Detected periodicity from autocorrelation
    float detectedPeriodMs = 0.0f;      // Period in milliseconds (0 = no pattern)
    float periodicityStrength = 0.0f;   // Confidence in detected period (0-1)

    // Current beat likelihood (0-1, based on periodic pattern and phase)
    float beatLikelihood = 0.0f;

    // ===== PUBLIC METHODS =====

    RhythmAnalyzer();

    /**
     * Add new onset strength sample (spectral flux value)
     * Call every frame when spectral flux is computed
     *
     * @param onsetStrength Spectral flux value (typically 0.0-10.0 range)
     */
    void addSample(float onsetStrength);

    /**
     * Update autocorrelation and periodicity detection
     * Call periodically (every 1 sec) to reduce CPU load
     *
     * @param nowMs Current time in milliseconds
     * @param frameRate Frame rate in Hz (e.g., 60.0)
     * @return true if pattern detected, false otherwise
     */
    bool update(uint32_t nowMs, float frameRate);

    /**
     * Get beat likelihood at current time
     * Based on detected period and current position in buffer
     * Returns 0.0 if no pattern, 0.0-1.0 otherwise
     */
    float getBeatLikelihood() const;

    /**
     * Retroactively confirm if beat occurred N frames ago
     * Checks if onset strength spike occurred at expected time
     *
     * @param framesAgo How many frames back to check (1 = previous frame)
     * @param threshold Minimum ratio vs neighbors to confirm (e.g., 1.5 = 50% louder)
     * @return true if beat confirmed, false otherwise
     */
    bool confirmPastBeat(int framesAgo, float threshold = 1.5f);

    /**
     * Reset all state
     */
    void reset();

    // ===== GETTERS =====

    inline float getDetectedPeriodMs() const { return detectedPeriodMs; }
    inline float getPeriodicityStrength() const { return periodicityStrength; }
    inline float getDetectedBPM() const {
        if (detectedPeriodMs <= 0.0f) return 0.0f;
        return 60000.0f / detectedPeriodMs;
    }
    inline int getBufferFillLevel() const { return frameCount_; }
    inline bool isBufferFull() const { return frameCount_ >= BUFFER_SIZE; }
    inline float getCurrentPhase() const { return currentPhase_; }

private:
    // Circular buffer for Onset Strength Signal (OSS) history
    float ossHistory_[BUFFER_SIZE];
    int writeIdx_ = 0;
    int frameCount_ = 0;  // Total frames written (for initialization)

    // Autocorrelation state
    uint32_t lastAutocorrMs_ = 0;

    // Beat phase tracking (for likelihood calculation)
    float currentPhase_ = 0.0f;  // 0.0-1.0 within detected period
    uint32_t lastPhaseUpdateMs_ = 0;  // Last time phase was updated
    float frameRate_ = 60.0f;  // Cached frame rate for phase calculations

    /**
     * Autocorrelation on buffered OSS
     * Finds the lag (period) with strongest correlation
     *
     * @param signal Buffer to analyze
     * @param length Number of samples in buffer
     * @param minPeriod Minimum period in frames (from maxBPM)
     * @param maxPeriod Maximum period in frames (from minBPM)
     * @param outPeriod [out] Detected period in frames
     * @param outStrength [out] Correlation strength (0-1)
     */
    void autocorrelate(const float* signal, int length,
                      float minPeriod, float maxPeriod,
                      float& outPeriod, float& outStrength);

    /**
     * Get sample from circular buffer (with wraparound)
     * @param framesAgo How many frames back (0 = most recent)
     * @return Sample value
     */
    inline float getSample(int framesAgo) const {
        int idx = (writeIdx_ - 1 - framesAgo + BUFFER_SIZE) % BUFFER_SIZE;
        if (idx < 0) idx += BUFFER_SIZE;  // Handle negative wraparound
        return ossHistory_[idx];
    }

    /**
     * Update beat phase based on detected period and elapsed time
     * Used for beat likelihood calculation
     * @param dtMs Elapsed time since last update in milliseconds
     */
    void updatePhase(uint32_t dtMs);

    /**
     * Helper: Clamp float to range
     */
    inline float clamp(float value, float minVal, float maxVal) const {
        return value < minVal ? minVal : (value > maxVal ? maxVal : value);
    }
};
