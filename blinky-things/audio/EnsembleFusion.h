#pragma once

#include "DetectionResult.h"
#include <stdint.h>

/**
 * EnsembleFusion - A+B Hybrid Fusion Strategy
 *
 * Combines detection results from multiple detectors using:
 * - Option A: Fixed calibrated weights (determined offline via calibration suite)
 * - Option B: Agreement-based confidence scaling (runtime adjustment)
 *
 * The fusion output is:
 *   finalStrength = weightedAverageStrength * agreementBoost
 *
 * Agreement boost values:
 *   0 detectors fired: 0.0  (no detection)
 *   1 detector fired:  0.6  (single-detector, suppress as possible false positive)
 *   2 detectors fired: 0.85 (some agreement)
 *   3 detectors fired: 1.0  (good consensus)
 *   4+ detectors:      1.1-1.2 (strong consensus boost)
 *
 * Why this works:
 * - Noise rarely triggers multiple independent algorithms simultaneously
 * - Real transients (drums, bass drops) ARE detected by multiple algorithms
 * - Fixed weights provide predictable baseline behavior
 * - Agreement boost adds natural false positive suppression
 *
 * Memory: ~100 bytes (detector configs + state)
 * CPU: <0.1ms per fusion (simple weighted sum)
 */

class EnsembleFusion {
public:
    // Maximum supported detectors
    static constexpr int MAX_DETECTORS = static_cast<int>(DetectorType::COUNT);

    /**
     * Default constructor - initializes with calibrated defaults
     */
    EnsembleFusion();

    /**
     * Configure a detector's weight and enable state
     * @param type Detector type
     * @param config Configuration with weight, threshold, enabled flag
     */
    void configureDetector(DetectorType type, const DetectorConfig& config);

    /**
     * Get a detector's current configuration
     */
    const DetectorConfig& getConfig(DetectorType type) const;

    /**
     * Set a detector's weight (0.0-1.0)
     * Weights don't need to sum to 1.0 - they're normalized during fusion
     */
    void setWeight(DetectorType type, float weight);

    /**
     * Enable or disable a detector
     * Disabled detectors still run but their results are ignored in fusion
     */
    void setEnabled(DetectorType type, bool enabled);

    /**
     * Set all weights at once (for calibration)
     * @param weights Array of MAX_DETECTORS weights
     */
    void setAllWeights(const float* weights);

    /**
     * Set agreement boost values
     * @param boosts Array of 7 values for 0-6 detectors firing
     */
    void setAgreementBoosts(const float* boosts);

    /**
     * Fuse detection results from all detectors
     * @param results Array of MAX_DETECTORS DetectionResult values
     *                Index corresponds to DetectorType enum value
     * @return Combined EnsembleOutput with transientStrength, confidence, agreement
     */
    EnsembleOutput fuse(const DetectionResult* results) const;

    /**
     * Get the weight sum for normalization (debug/tuning)
     */
    float getTotalWeight() const;

    /**
     * Get the current agreement boost for a given detector count
     */
    float getAgreementBoost(int detectorCount) const;

    /**
     * Reset to calibrated defaults
     */
    void resetToDefaults();

private:
    // Per-detector configuration
    DetectorConfig configs_[MAX_DETECTORS];

    // Agreement-based confidence scaling
    // Index = number of detectors that fired (0-6)
    float agreementBoosts_[MAX_DETECTORS + 1];

    // Find which detector contributed most to the detection
    uint8_t findDominantDetector(const DetectionResult* results) const;
};

// --- Default calibrated values ---
// These are initial values; run calibration suite to optimize for your patterns

namespace FusionDefaults {
    // Detector weights (determined by relative F1 contribution)
    // Drummer: Strong on amplitude spikes, precise timing
    // SpectralFlux: High recall, robust to noise
    // HFC: Excellent on percussive attacks
    // BassBand: Focused on kicks/bass
    // ComplexDomain: Catches soft onsets
    // MelFlux: Perceptually accurate
    constexpr float WEIGHTS[] = {
        0.22f,  // DRUMMER
        0.20f,  // SPECTRAL_FLUX
        0.15f,  // HFC
        0.18f,  // BASS_BAND
        0.13f,  // COMPLEX_DOMAIN
        0.12f   // MEL_FLUX
    };

    // Agreement boost values
    // [0] = 0 detectors, [1] = 1 detector, ..., [6] = 6 detectors
    constexpr float AGREEMENT_BOOSTS[] = {
        0.0f,   // 0: No detection
        0.6f,   // 1: Single detector - suppress as possible false positive
        0.85f,  // 2: Two detectors - some agreement
        1.0f,   // 3: Three detectors - good consensus
        1.1f,   // 4: Four detectors - strong consensus
        1.15f,  // 5: Five detectors - very strong
        1.2f    // 6: All detectors - maximum boost
    };

    // Default detector thresholds
    constexpr float THRESHOLDS[] = {
        2.5f,   // DRUMMER: amplitude ratio vs average
        1.4f,   // SPECTRAL_FLUX: flux vs local median
        3.0f,   // HFC: high-freq content vs average
        3.0f,   // BASS_BAND: bass flux vs average
        2.0f,   // COMPLEX_DOMAIN: phase deviation threshold
        2.5f    // MEL_FLUX: mel flux vs local median
    };
}
