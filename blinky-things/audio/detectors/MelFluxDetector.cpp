#include "MelFluxDetector.h"
#include <math.h>

MelFluxDetector::MelFluxDetector()
    : hasPrevFrame_(false)
    , currentMelFlux_(0.0f)
    , averageMelFlux_(0.0f)
{
    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = 0.0f;
    }
}

void MelFluxDetector::resetImpl() {
    hasPrevFrame_ = false;
    currentMelFlux_ = 0.0f;
    averageMelFlux_ = 0.0f;

    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = 0.0f;
    }
}

DetectionResult MelFluxDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* melBands = frame.melBands;
    int numBands = frame.numMelBands;
    uint32_t nowMs = frame.timestampMs;

    // Need at least one previous frame
    if (!hasPrevFrame_) {
        // Save current mel bands
        for (int i = 0; i < numBands && i < SpectralConstants::NUM_MEL_BANDS; i++) {
            prevMelBands_[i] = melBands[i];
        }
        hasPrevFrame_ = true;
        return DetectionResult::none();
    }

    // Compute mel flux
    currentMelFlux_ = computeMelFlux(melBands, numBands);

    // Update running average
    const float alpha = 0.05f;
    averageMelFlux_ += alpha * (currentMelFlux_ - averageMelFlux_);

    // Store for debugging
    lastRawValue_ = currentMelFlux_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentMelFlux_);

    // Detection
    // NOTE: Cooldown now applied at ensemble level (EnsembleFusion), not per-detector
    bool isLoudEnough = currentMelFlux_ > effectiveThreshold;

    DetectionResult result;

    if (isLoudEnough) {
        // Strength
        float ratio = currentMelFlux_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentMelFlux_, localMedian);

        result = DetectionResult::hit(strength, confidence);
        // NOTE: markTransient() removed - cooldown now at ensemble level
    } else {
        result = DetectionResult::none();
    }

    // Save current mel bands for next frame
    for (int i = 0; i < numBands && i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = melBands[i];
    }

    return result;
}

float MelFluxDetector::computeMelFlux(const float* melBands, int numBands) {
    // Half-wave rectified flux on mel bands
    // Since mel bands are already log-compressed, this captures
    // perceptually significant changes

    float flux = 0.0f;
    int bandsAnalyzed = 0;

    int actualBands = (numBands > SpectralConstants::NUM_MEL_BANDS)
                     ? SpectralConstants::NUM_MEL_BANDS : numBands;

    for (int i = 0; i < actualBands; i++) {
        float diff = melBands[i] - prevMelBands_[i];
        if (diff > 0.0f) {
            flux += diff;
        }
        bandsAnalyzed++;
    }

    // Normalize
    if (bandsAnalyzed > 0) {
        flux /= bandsAnalyzed;
    }

    return flux;
}

float MelFluxDetector::computeConfidence(float flux, float median) const {
    // Mel flux matches human perception, generally reliable
    float ratio = flux / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Perceptual approach is robust
    return clamp01(ratioConfidence * 0.8f + 0.2f);
}
