#include "EnsembleFusion.h"
#include "../types/BlinkyAssert.h"

EnsembleFusion::EnsembleFusion() {
    resetToDefaults();
}

void EnsembleFusion::resetToDefaults() {
    // Initialize detector configs with calibrated defaults (Jan 2026)
    for (int i = 0; i < MAX_DETECTORS; i++) {
        configs_[i].weight = FusionDefaults::WEIGHTS[i];
        configs_[i].threshold = FusionDefaults::THRESHOLDS[i];
        configs_[i].enabled = FusionDefaults::ENABLED[i];
    }

    // Initialize agreement boosts
    // agreementBoosts_ has MAX_DETECTORS+1 elements (indices 0..MAX_DETECTORS)
    // for 0 through MAX_DETECTORS detectors firing, so <= is intentional
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
    BLINKY_ASSERT(idx >= 0 && idx < MAX_DETECTORS, "EnsembleFusion::getConfig bad DetectorType");
    if (idx < 0 || idx >= MAX_DETECTORS) {
        return configs_[0];
    }
    return configs_[idx];
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
    // agreementBoosts_ has MAX_DETECTORS + 1 elements, so <= is intentional
    for (int i = 0; i <= MAX_DETECTORS; i++) {
        agreementBoosts_[i] = boosts[i];
    }
}

EnsembleOutput EnsembleFusion::fuse(const DetectionResult* results, uint32_t timestampMs, float audioLevel) {
    EnsembleOutput output;

    // === NOISE GATE ===
    // Suppress all detections when audio level is below threshold (silence)
    // This prevents false positives from electrical noise in quiet environments
    if (audioLevel < minAudioLevel) {
        output.transientStrength = 0.0f;
        output.ensembleConfidence = 0.0f;
        output.detectorAgreement = 0;
        output.dominantDetector = 0;
        return output;
    }

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
            // Apply confidence threshold - ignore low-confidence detections
            if (result.confidence < minConfidence) {
                continue;  // Skip this detector, confidence too low
            }

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

    // Compute fused strength BEFORE cooldown
    float fusedStrength = combinedStrength * agreementBoost;

    // Clamp to valid range
    if (fusedStrength > 1.0f) {
        fusedStrength = 1.0f;
    }

    // UNIFIED ENSEMBLE COOLDOWN: Apply cooldown AFTER fusion
    // Cooldown prevents rapid-fire ensemble detections (not individual algorithms)
    // Use unsigned arithmetic (wraps correctly within 49-day window)
    // Uses adaptive cooldown based on detected tempo (shorter at faster tempos)
    uint32_t elapsedMs = timestampMs - lastTransientMs_;
    uint16_t effectiveCooldown = getEffectiveCooldownMs();
    bool cooldownElapsed = elapsedMs > (uint32_t)effectiveCooldown;

    if (fusedStrength > 0.01f && cooldownElapsed) {
        // Detection passes cooldown - output it
        output.transientStrength = fusedStrength;
        lastTransientMs_ = timestampMs;  // Update cooldown timer
    } else {
        // Either no detection or suppressed by cooldown
        output.transientStrength = 0.0f;
    }

    // Confidence and agreement always reported (even if suppressed by cooldown)
    // This allows debugging of what fusion WOULD have output without cooldown
    // FIX: Clamp ensembleConfidence to [0, 1] - agreementBoost can exceed 1.0 for strong consensus
    output.ensembleConfidence = (agreementBoost > 1.0f) ? 1.0f : agreementBoost;
    output.detectorAgreement = static_cast<uint8_t>(agreementCount);
    output.dominantDetector = static_cast<uint8_t>(maxStrengthIdx);

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

void EnsembleFusion::setTempoHint(float bpm) {
    tempoHintBpm_ = bpm;
}

uint16_t EnsembleFusion::getEffectiveCooldownMs() const {
    // If adaptive cooldown disabled or no tempo hint, use fixed cooldown
    if (!adaptiveCooldownEnabled_ || tempoHintBpm_ < 30.0f) {
        return cooldownMs;
    }

    // Calculate beat period in ms
    float beatPeriodMs = 60000.0f / tempoHintBpm_;

    // Adaptive cooldown: allow ~6 detections per beat
    // This enables detection of 16th notes at moderate tempos
    // while still preventing rapid-fire false positives
    uint16_t adaptiveCooldown = static_cast<uint16_t>(beatPeriodMs / 6.0f);

    // Clamp to safe range
    if (adaptiveCooldown < MIN_COOLDOWN_MS) {
        adaptiveCooldown = MIN_COOLDOWN_MS;
    }
    if (adaptiveCooldown > MAX_COOLDOWN_MS) {
        adaptiveCooldown = MAX_COOLDOWN_MS;
    }

    // Use the smaller of base cooldown and adaptive cooldown
    // (never make cooldown longer than configured base)
    return (adaptiveCooldown < cooldownMs) ? adaptiveCooldown : cooldownMs;
}
