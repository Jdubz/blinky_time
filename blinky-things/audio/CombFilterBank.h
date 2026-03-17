#pragma once

/**
 * CombFilterBank - Independent tempo validation using parallel comb filter resonators
 *
 * Theory: A bank of comb filters at different tempos (60-198 BPM) provides
 * independent tempo validation without depending on autocorrelation being correct.
 * Each filter accumulates energy when the input has periodicity at its tempo.
 * The filter with maximum energy indicates the most likely tempo.
 *
 * Key improvements over single comb filter:
 * - Independent tempo detection (doesn't follow autocorrelation)
 * - Proper Scheirer (1998) equation: y[n] = (1-α)·x[n] + α·y[n-L]
 * - Complex phase extraction (not peak detection)
 * - Tempo prior weighting to reduce half-time/double-time confusion
 *
 * Memory: ~5.3 KB total (resonatorDelay_[20][66] = 5,280 bytes + state)
 * CPU: ~2% (20 filters × simple math, phase every 4 frames)
 */
class CombFilterBank {
public:
    static constexpr int MAX_LAG = 66;  // 60 BPM at 66 Hz (66*60/66)
    static constexpr int MIN_LAG = 20;  // 198 BPM at 66 Hz (66*60/20)
    // 20 filters linearly interpolated from MIN_LAG to MAX_LAG (60-198 BPM).
    // Proven at F1=0.519. 47 bins tested (v61, every integer lag) but A/B showed
    // no benefit (+12 KB RAM wasted). Reverted to 20.
    static constexpr int NUM_FILTERS = 20;

    // === TUNING PARAMETERS ===
    float feedbackGain = 0.92f;       // Resonance strength (0.85-0.98)
    // NOTE: No tempo prior - comb bank uses raw resonator energy
    // This provides independent tempo validation (autocorr applies prior separately)

    // === METHODS ===

    /**
     * Initialize the filter bank (compute filter lags/BPMs)
     */
    void init(float frameRate = 66.0f);

    /**
     * Reset all state (delay line, resonators, energy)
     */
    void reset();

    /**
     * Process one sample of onset strength
     * Updates all 20 resonators and finds peak tempo
     */
    void process(float input);

    /**
     * Get detected tempo in BPM
     */
    float getPeakBPM() const { return peakBPM_; }

    /**
     * Get confidence in tempo estimate (0-1)
     * Based on peak-to-mean energy ratio
     */
    float getPeakConfidence() const { return peakConfidence_; }

    /**
     * Get phase at detected tempo (0-1)
     * Extracted via complex exponential (Fourier method)
     */
    float getPhaseAtPeak() const { return peakPhase_; }

    /**
     * Get peak filter index (for debugging)
     */
    int getPeakFilterIndex() const { return peakFilterIdx_; }

    /**
     * Get resonator energy for a specific filter (for debugging)
     */
    float getFilterEnergy(int idx) const {
        if (idx >= 0 && idx < NUM_FILTERS) return resonatorEnergy_[idx];
        return 0.0f;
    }

    /**
     * Get BPM for a specific filter (for debugging)
     */
    float getFilterBPM(int idx) const {
        if (idx >= 0 && idx < NUM_FILTERS) return filterBPMs_[idx];
        return 0.0f;
    }

private:
    // Per-filter output delay lines for IIR feedback
    // Each filter stores its own output history so y[n-L] feeds back correctly.
    // Memory: 20 filters × 66 samples × 4 bytes = 5,280 bytes
    float resonatorDelay_[NUM_FILTERS][MAX_LAG] = {{0}};
    int writeIdx_ = 0;

    // Per-filter resonator state
    float resonatorOutput_[NUM_FILTERS] = {0};
    float resonatorEnergy_[NUM_FILTERS] = {0};  // Smoothed energy

    // Lag and BPM for each filter (pre-computed in init())
    int filterLags_[NUM_FILTERS] = {0};
    float filterBPMs_[NUM_FILTERS] = {0};

    // Results
    float peakBPM_ = 120.0f;
    float peakConfidence_ = 0.0f;
    float peakPhase_ = 0.0f;
    int peakFilterIdx_ = NUM_FILTERS / 2;  // Middle filter (approx 120 BPM)

    // Resonator history at peak filter for phase extraction
    float resonatorHistory_[MAX_LAG] = {0};
    int historyIdx_ = 0;

    // Frame counter for phase extraction throttling
    int frameCount_ = 0;

    // Frame rate for BPM calculations
    float frameRate_ = 60.0f;

    // Initialization flag
    bool initialized_ = false;

    /**
     * Extract phase using complex exponential (Fourier method)
     * Called every 4 frames to save CPU
     */
    void extractPhase();
};
