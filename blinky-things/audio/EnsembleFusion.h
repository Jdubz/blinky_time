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
 * Agreement boost values (calibrated Jan 2026):
 *   0 detectors fired: 0.0  (no detection)
 *   1 detector fired:  0.3  (single-detector, strongly suppress)
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
     * Disabled detectors are skipped entirely (no CPU usage)
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
     * Fuse detection results from all detectors with unified cooldown
     * @param results Array of MAX_DETECTORS DetectionResult values
     *                Index corresponds to DetectorType enum value
     * @param timestampMs Current timestamp in milliseconds (for cooldown tracking)
     * @param audioLevel Current normalized audio level (0.0-1.0) for noise gate
     * @return Combined EnsembleOutput with transientStrength, confidence, agreement
     */
    EnsembleOutput fuse(const DetectionResult* results, uint32_t timestampMs, float audioLevel = 1.0f);

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

    /**
     * Set unified ensemble cooldown period
     * @param ms Cooldown period in milliseconds (default 250ms)
     */
    void setCooldownMs(uint16_t ms) { cooldownMs_ = ms; }
    uint16_t getCooldownMs() const { return cooldownMs_; }

    /**
     * Set minimum confidence threshold for detections
     * Detectors with confidence below this are ignored in fusion
     * @param threshold Minimum confidence (0.0-1.0, default 0.55)
     */
    void setMinConfidence(float threshold) { minConfidence_ = threshold; }
    float getMinConfidence() const { return minConfidence_; }

    /**
     * Set minimum audio level for noise gate
     * Detections are suppressed when audio level is below this threshold
     * @param level Minimum level (0.0-1.0, default 0.02 = 2%)
     */
    void setMinAudioLevel(float level) { minAudioLevel_ = level; }
    float getMinAudioLevel() const { return minAudioLevel_; }

    // === PUBLIC TUNING PARAMETERS (for SettingsRegistry) ===
    // These are exposed for real-time serial tuning via unified set/get interface

    // Unified ensemble cooldown (applied after fusion, not per-detector)
    uint16_t cooldownMs = 250;  // Calibrated Feb 2026: 250ms reduces rapid-fire false positives

    // Minimum confidence threshold (detectors below this are ignored)
    float minConfidence = 0.55f;  // Calibrated Feb 2026: 0.55 filters weak detections

    // Minimum audio level for noise gate (suppress detections in silence)
    float minAudioLevel = 0.025f;  // Calibrated Feb 2026: 2.5% level (noise gate)

private:
    // Per-detector configuration
    DetectorConfig configs_[MAX_DETECTORS];

    // Agreement-based confidence scaling
    // Index = number of detectors that fired (0-6)
    float agreementBoosts_[MAX_DETECTORS + 1];

    // Internal state
    uint32_t lastTransientMs_ = 0;

    // Legacy private aliases (for backward compatibility with setter methods)
    uint16_t& cooldownMs_ = cooldownMs;
    float& minConfidence_ = minConfidence;
    float& minAudioLevel_ = minAudioLevel;
};

// --- Default calibrated values (January 2026) ---
// Optimized via calibration suite: only HFC + Drummer enabled (2-detector config).
// Disabled detectors retain original weights for re-enablement.
// Run calibration suite (npm run tuner -- sweep-weights) to re-optimize.

namespace FusionDefaults {
    // Detector weights (Feb 2026)
    // Only enabled detectors are called; disabled ones use zero CPU.
    constexpr float WEIGHTS[] = {
        0.40f,  // DRUMMER - amplitude transients (calibrated Feb 2026)
        0.20f,  // SPECTRAL_FLUX - mel-band SuperFlux (disabled, needs tuning)
        0.60f,  // HFC - percussive attack detection (calibrated Feb 2026)
        0.18f,  // BASS_BAND - disabled, environmental noise issues
        0.13f,  // COMPLEX_DOMAIN - disabled, needs tuning after phase fix
        0.12f   // NOVELTY - cosine distance spectral novelty (disabled, needs tuning)
    };

    // Detector enabled flags (Feb 2026)
    // Only enabled detectors run; disabled ones are skipped entirely.
    constexpr bool ENABLED[] = {
        true,   // DRUMMER - time-domain amplitude detection
        false,  // SPECTRAL_FLUX - disabled: fires on pad chord changes at all thresholds
        true,   // HFC - high-frequency percussive attacks
        false,  // BASS_BAND - disabled: susceptible to room rumble/HVAC
        false,  // COMPLEX_DOMAIN - disabled: adds FPs on sparse patterns (tested Feb 2026)
        false   // NOVELTY - disabled: net negative avg F1, hurts sparse/full-mix (tested Feb 2026)
    };

    // Agreement boost values
    // [0] = 0 detectors, [1] = 1 detector, ..., [6] = 6 detectors
    constexpr float AGREEMENT_BOOSTS[] = {
        0.0f,   // 0: No detection
        0.2f,   // 1: Single detector - strongly suppress (calibrated Feb 2026)
        0.85f,  // 2: Two detectors - some agreement
        1.0f,   // 3: Three detectors - good consensus
        1.1f,   // 4: Four detectors - strong consensus
        1.15f,  // 5: Five detectors - very strong
        1.2f    // 6: All detectors - maximum boost
    };

    // Default detector thresholds (Feb 2026 calibration)
    constexpr float THRESHOLDS[] = {
        3.5f,   // DRUMMER: amplitude ratio vs average (calibrated Feb 2026)
        1.4f,   // SPECTRAL_FLUX: flux vs local median
        4.0f,   // HFC: high-freq content vs average (calibrated Feb 2026)
        3.0f,   // BASS_BAND: bass flux vs average
        2.0f,   // COMPLEX_DOMAIN: phase deviation threshold
        2.5f    // NOVELTY: cosine distance vs local median
    };

    // Compile-time validation: ensure arrays match detector count
    static_assert(sizeof(WEIGHTS) / sizeof(WEIGHTS[0]) == static_cast<int>(DetectorType::COUNT),
                  "WEIGHTS array size must match DetectorType::COUNT");
    static_assert(sizeof(ENABLED) / sizeof(ENABLED[0]) == static_cast<int>(DetectorType::COUNT),
                  "ENABLED array size must match DetectorType::COUNT");
    static_assert(sizeof(THRESHOLDS) / sizeof(THRESHOLDS[0]) == static_cast<int>(DetectorType::COUNT),
                  "THRESHOLDS array size must match DetectorType::COUNT");
    static_assert(sizeof(AGREEMENT_BOOSTS) / sizeof(AGREEMENT_BOOSTS[0]) == static_cast<int>(DetectorType::COUNT) + 1,
                  "AGREEMENT_BOOSTS array size must be DetectorType::COUNT + 1");
}
