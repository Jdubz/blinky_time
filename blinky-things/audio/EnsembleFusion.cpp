#include "EnsembleFusion.h"

EnsembleFusion::EnsembleFusion() {
    resetToDefaults();
}

void EnsembleFusion::resetToDefaults() {
    config_.weight = FusionDefaults::BAND_FLUX_WEIGHT;
    config_.threshold = FusionDefaults::BAND_FLUX_THRESHOLD;
    config_.enabled = true;
}

void EnsembleFusion::configureDetector(const DetectorConfig& config) {
    config_ = config;
}

const DetectorConfig& EnsembleFusion::getConfig() const {
    return config_;
}

void EnsembleFusion::setWeight(float weight) {
    config_.weight = (weight < 0.0f) ? 0.0f : weight;
}

void EnsembleFusion::setEnabled(bool enabled) {
    config_.enabled = enabled;
}

EnsembleOutput EnsembleFusion::fuseSolo(const DetectionResult& result, int detectorIdx,
                                        uint32_t timestampMs, float audioLevel) {
    EnsembleOutput output;

    // Noise gate: suppress detections in silence
    if (audioLevel < minAudioLevel) {
        return output;
    }

    // Pass through detection if above confidence threshold
    float strength = 0.0f;
    if (result.detected && result.confidence >= minConfidence) {
        strength = result.strength;
        if (strength > 1.0f) strength = 1.0f;
    }

    // Tempo-adaptive cooldown (guard against timestamp wraparound)
    if (timestampMs < lastTransientMs_) {
        lastTransientMs_ = timestampMs;
        return output;
    }
    uint32_t elapsedMs = timestampMs - lastTransientMs_;
    uint16_t effectiveCooldown = getEffectiveCooldownMs();

    if (strength > 0.01f && elapsedMs > (uint32_t)effectiveCooldown) {
        output.transientStrength = strength;
        output.ensembleConfidence = 1.0f;
        output.detectorAgreement = 1;
        output.dominantDetector = static_cast<uint8_t>(detectorIdx);
        lastTransientMs_ = timestampMs;
    }

    return output;
}

void EnsembleFusion::setTempoHint(float bpm) {
    tempoHintBpm_ = bpm;
}

uint16_t EnsembleFusion::getEffectiveCooldownMs() const {
    if (!adaptiveCooldownEnabled_ || tempoHintBpm_ < 30.0f) {
        return cooldownMs;
    }

    float beatPeriodMs = 60000.0f / tempoHintBpm_;
    uint16_t adaptiveCooldown = static_cast<uint16_t>(beatPeriodMs / 6.0f);

    if (adaptiveCooldown < MIN_COOLDOWN_MS) adaptiveCooldown = MIN_COOLDOWN_MS;
    if (adaptiveCooldown > MAX_COOLDOWN_MS) adaptiveCooldown = MAX_COOLDOWN_MS;

    return (adaptiveCooldown < cooldownMs) ? adaptiveCooldown : cooldownMs;
}
