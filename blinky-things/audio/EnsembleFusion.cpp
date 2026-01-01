#include "EnsembleFusion.h"

EnsembleFusion::EnsembleFusion() {
    resetToDefaults();
}

void EnsembleFusion::resetToDefaults() {
    // Initialize detector configs with calibrated defaults
    for (int i = 0; i < MAX_DETECTORS; i++) {
        configs_[i].weight = FusionDefaults::WEIGHTS[i];
        configs_[i].threshold = FusionDefaults::THRESHOLDS[i];
        configs_[i].enabled = true;
    }

    // Initialize agreement boosts
    for (int i = 0; i <= MAX_DETECTORS; i++) {
        agreementBoosts_[i] = FusionDefaults::AGREEMENT_BOOSTS[i];
    }
}

void EnsembleFusion::configureDetector(DetectorType type, const DetectorConfig& config) {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < MAX_DETECTORS) {
        configs_[idx] = config;
    }
}

const DetectorConfig& EnsembleFusion::getConfig(DetectorType type) const {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < MAX_DETECTORS) {
        return configs_[idx];
    }
    // Return first config as fallback (shouldn't happen)
    return configs_[0];
}

void EnsembleFusion::setWeight(DetectorType type, float weight) {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < MAX_DETECTORS) {
        configs_[idx].weight = (weight < 0.0f) ? 0.0f : weight;
    }
}

void EnsembleFusion::setEnabled(DetectorType type, bool enabled) {
    int idx = static_cast<int>(type);
    if (idx >= 0 && idx < MAX_DETECTORS) {
        configs_[idx].enabled = enabled;
    }
}

void EnsembleFusion::setAllWeights(const float* weights) {
    for (int i = 0; i < MAX_DETECTORS; i++) {
        configs_[i].weight = (weights[i] < 0.0f) ? 0.0f : weights[i];
    }
}

void EnsembleFusion::setAgreementBoosts(const float* boosts) {
    for (int i = 0; i <= MAX_DETECTORS; i++) {
        agreementBoosts_[i] = boosts[i];
    }
}

EnsembleOutput EnsembleFusion::fuse(const DetectionResult* results) const {
    EnsembleOutput output;

    // Count how many enabled detectors fired and compute weighted strength
    int agreementCount = 0;
    float weightedStrengthSum = 0.0f;
    float activeWeightSum = 0.0f;
    float maxStrength = 0.0f;
    int maxStrengthIdx = 0;

    for (int i = 0; i < MAX_DETECTORS; i++) {
        if (!configs_[i].enabled) {
            continue;  // Skip disabled detectors
        }

        const DetectionResult& result = results[i];

        if (result.detected) {
            agreementCount++;

            // Weighted contribution
            float weight = configs_[i].weight;
            weightedStrengthSum += result.strength * weight;
            activeWeightSum += weight;

            // Track dominant detector
            if (result.strength > maxStrength) {
                maxStrength = result.strength;
                maxStrengthIdx = i;
            }
        }
    }

    // Compute combined strength (normalized weighted average of firing detectors)
    float combinedStrength = 0.0f;
    if (activeWeightSum > 0.0f) {
        combinedStrength = weightedStrengthSum / activeWeightSum;
    }

    // Apply agreement-based confidence scaling
    int boostIdx = (agreementCount > MAX_DETECTORS) ? MAX_DETECTORS : agreementCount;
    float agreementBoost = agreementBoosts_[boostIdx];

    // Final output
    output.transientStrength = combinedStrength * agreementBoost;
    output.ensembleConfidence = agreementBoost;
    output.detectorAgreement = static_cast<uint8_t>(agreementCount);
    output.dominantDetector = static_cast<uint8_t>(maxStrengthIdx);

    // Clamp strength to valid range
    if (output.transientStrength > 1.0f) {
        output.transientStrength = 1.0f;
    }

    return output;
}

float EnsembleFusion::getTotalWeight() const {
    float sum = 0.0f;
    for (int i = 0; i < MAX_DETECTORS; i++) {
        if (configs_[i].enabled) {
            sum += configs_[i].weight;
        }
    }
    return sum;
}

float EnsembleFusion::getAgreementBoost(int detectorCount) const {
    if (detectorCount < 0) return 0.0f;
    if (detectorCount > MAX_DETECTORS) detectorCount = MAX_DETECTORS;
    return agreementBoosts_[detectorCount];
}

uint8_t EnsembleFusion::findDominantDetector(const DetectionResult* results) const {
    float maxContribution = 0.0f;
    uint8_t dominant = 0;

    for (int i = 0; i < MAX_DETECTORS; i++) {
        if (!configs_[i].enabled) continue;

        const DetectionResult& result = results[i];
        if (result.detected) {
            float contribution = result.strength * configs_[i].weight;
            if (contribution > maxContribution) {
                maxContribution = contribution;
                dominant = static_cast<uint8_t>(i);
            }
        }
    }

    return dominant;
}
