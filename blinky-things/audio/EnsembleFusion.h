#pragma once

#include "DetectionResult.h"
#include <stdint.h>

/**
 * EnsembleFusion - BandFlux Solo detection with cooldown and noise gate
 *
 * Simplified from multi-detector weighted fusion (v64, Mar 2026).
 * With BandFlux as the only detector, this class provides:
 * - Noise gate (suppress detections in silence)
 * - Confidence threshold (filter low-confidence detections)
 * - Unified cooldown (tempo-adaptive, prevents rapid-fire)
 *
 * Memory: ~32 bytes (config + state)
 * CPU: <0.01ms per frame
 */

class EnsembleFusion {
public:
    EnsembleFusion();

    /**
     * Configure the BandFlux detector's weight, threshold, and enabled state
     */
    void configureDetector(DetectorType type, const DetectorConfig& config);
    const DetectorConfig& getConfig(DetectorType type) const;
    void setWeight(DetectorType type, float weight);
    void setEnabled(DetectorType type, bool enabled);

    /**
     * Fuse BandFlux detection result with noise gate and cooldown
     */
    EnsembleOutput fuseSolo(const DetectionResult& result, int detectorIdx,
                            uint32_t timestampMs, float audioLevel = 1.0f);

    void resetToDefaults();

    // Cooldown control
    void setCooldownMs(uint16_t ms) { cooldownMs = ms; }
    uint16_t getCooldownMs() const { return cooldownMs; }
    void setTempoHint(float bpm);
    float getTempoHint() const { return tempoHintBpm_; }
    void setAdaptiveCooldown(bool enabled) { adaptiveCooldownEnabled_ = enabled; }
    bool isAdaptiveCooldownEnabled() const { return adaptiveCooldownEnabled_; }
    uint16_t getEffectiveCooldownMs() const;

    // Confidence/noise gate
    void setMinConfidence(float threshold) { minConfidence = threshold; }
    float getMinConfidence() const { return minConfidence; }
    void setMinAudioLevel(float level) { minAudioLevel = level; }
    float getMinAudioLevel() const { return minAudioLevel; }

    // === PUBLIC TUNING PARAMETERS (for SettingsRegistry) ===
    uint16_t cooldownMs = 250;
    float minConfidence = 0.40f;
    float minAudioLevel = 0.025f;

private:
    DetectorConfig config_;  // BandFlux config

    // Cooldown state
    uint32_t lastTransientMs_ = 0;
    float tempoHintBpm_ = 0.0f;
    bool adaptiveCooldownEnabled_ = true;
    static constexpr uint16_t MIN_COOLDOWN_MS = 40;
    static constexpr uint16_t MAX_COOLDOWN_MS = 150;
};

namespace FusionDefaults {
    constexpr float BAND_FLUX_WEIGHT = 0.50f;
    constexpr float BAND_FLUX_THRESHOLD = 0.5f;
}
