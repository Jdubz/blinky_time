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
    void setCooldownMs(uint16_t ms) { cooldownMs = ms; }
    uint16_t getCooldownMs() const { return cooldownMs; }

    /**
     * Set tempo hint for adaptive cooldown (called by AudioController)
     * Adapts cooldown to detected tempo:
     *   - At 120 BPM: cooldown = baseCooldownMs (unchanged)
     *   - At 150 BPM: cooldown = ~67ms (faster tempo = shorter cooldown)
     *   - At 180 BPM: cooldown = ~55ms
     * Formula: effectiveCooldown = max(minCooldown, min(baseCooldown, beatPeriod / 6))
     * @param bpm Current detected tempo (0 = disabled, use fixed cooldown)
     */
    void setTempoHint(float bpm);
    float getTempoHint() const { return tempoHintBpm_; }

    /**
     * Enable/disable adaptive cooldown based on tempo
     * When enabled, cooldown adjusts to detected tempo
     * When disabled, uses fixed cooldownMs value
     */
    void setAdaptiveCooldown(bool enabled) { adaptiveCooldownEnabled_ = enabled; }
    bool isAdaptiveCooldownEnabled() const { return adaptiveCooldownEnabled_; }

    /**
     * Get the current effective cooldown (may differ from base if adaptive)
     */
    uint16_t getEffectiveCooldownMs() const;

    /**
     * Set minimum confidence threshold for detections
     * Detectors with confidence below this are ignored in fusion
     * @param threshold Minimum confidence (0.0-1.0, default 0.55)
     */
    void setMinConfidence(float threshold) { minConfidence = threshold; }
    float getMinConfidence() const { return minConfidence; }

    /**
     * Set minimum audio level for noise gate
     * Detections are suppressed when audio level is below this threshold
     * @param level Minimum level (0.0-1.0, default 0.02 = 2%)
     */
    void setMinAudioLevel(float level) { minAudioLevel = level; }
    float getMinAudioLevel() const { return minAudioLevel; }

    // === PUBLIC TUNING PARAMETERS (for SettingsRegistry) ===
    // These are exposed for real-time serial tuning via unified set/get interface

    // Unified ensemble cooldown (applied after fusion, not per-detector)
    uint16_t cooldownMs = 250;  // Calibrated Feb 2026: 250ms reduces rapid-fire false positives

    // Minimum confidence threshold (detectors below this are ignored)
    float minConfidence = 0.40f;  // Lowered for EDM: bass detections often have lower confidence

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

    // Adaptive cooldown state
    float tempoHintBpm_ = 0.0f;           // Current tempo hint (0 = unknown)
    bool adaptiveCooldownEnabled_ = true; // Enable tempo-adaptive cooldown
    static constexpr uint16_t MIN_COOLDOWN_MS = 40;   // Absolute minimum cooldown
    static constexpr uint16_t MAX_COOLDOWN_MS = 150;  // Maximum cooldown (slower tempos)

};

// --- Default calibrated values (January 2026) ---
// Optimized via calibration suite: only HFC + Drummer enabled (2-detector config).
// Disabled detectors retain original weights for re-enablement.
// Run calibration suite (npm run tuner -- sweep-weights) to re-optimize.

namespace FusionDefaults {
    // Detector weights (Feb 2026)
    // Only enabled detectors are called; disabled ones use zero CPU.
    // Rebalanced for EDM: BassBand promoted (sub-bass kicks), HFC demoted (hi-hats only)
    constexpr float WEIGHTS[] = {
        0.50f,  // DRUMMER - amplitude transients, best recall (tuned Feb 2026)
        0.20f,  // SPECTRAL_FLUX - disabled
        0.20f,  // HFC - disabled
        0.45f,  // BASS_BAND - disabled
        0.50f,  // COMPLEX_DOMAIN - phase onsets, best precision (tuned Feb 2026)
        0.12f   // NOVELTY - disabled
    };

    // Detector enabled flags (Feb 2026)
    // Only enabled detectors run; disabled ones are skipped entirely.
    constexpr bool ENABLED[] = {
        true,   // DRUMMER - time-domain amplitude, best recall on kicks/snares (F1=0.901)
        false,  // SPECTRAL_FLUX - disabled: fires on pad chord changes
        false,  // HFC - disabled: hi-hat detector, max F1=0.620, creates busy visuals
        false,  // BASS_BAND - disabled: too noisy (100+ detections/30s even at thresh 60)
        true,   // COMPLEX_DOMAIN - phase-based onset, best precision on kicks (F1=0.929)
        false   // NOVELTY - disabled: near-zero detections on real music
    };

    // Agreement boost values (updated Feb 2026 for 2-detector config: Drummer + Complex)
    // With only 2 detectors, single-detector hits are common and valid
    // [0] = 0 detectors, [1] = 1 detector, ..., [6] = 6 detectors
    constexpr float AGREEMENT_BOOSTS[] = {
        0.0f,   // 0: No detection
        0.7f,   // 1: Single detector - common with 2-detector config, allow most through
        1.0f,   // 2: Two detectors - full agreement (Drummer+Complex both fire)
        1.1f,   // 3: Three detectors
        1.1f,   // 4: Four detectors
        1.15f,  // 5: Five detectors
        1.2f    // 6: All detectors
    };

    // Default detector thresholds (Feb 2026 calibration)
    // Tune at runtime via: set detector_thresh <type> <value>
    constexpr float THRESHOLDS[] = {
        4.5f,   // DRUMMER: amplitude ratio vs average (tuned Feb 2026: F1=0.901 @ 4.5)
        1.4f,   // SPECTRAL_FLUX: flux vs local median
        4.0f,   // HFC: high-freq content vs average
        3.0f,   // BASS_BAND: bass flux vs average
        3.5f,   // COMPLEX_DOMAIN: phase deviation (tuned Feb 2026: F1=0.929 @ 3.5)
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
