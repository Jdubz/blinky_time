#include "SpectralFluxDetector.h"
#include <math.h>

SpectralFluxDetector::SpectralFluxDetector()
    : hasPrevFrame_(false)
    , minBin_(1)    // Skip DC
    , maxBin_(64)   // Up to 4kHz (captures most transient energy)
    , currentFlux_(0.0f)
    , averageFlux_(0.0f)
{
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevMagnitudes_[i] = 0.0f;
    }
}

void SpectralFluxDetector::resetImpl() {
    hasPrevFrame_ = false;
    currentFlux_ = 0.0f;
    averageFlux_ = 0.0f;

    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevMagnitudes_[i] = 0.0f;
    }
}

void SpectralFluxDetector::setAnalysisRange(int minBin, int maxBin) {
    minBin_ = (minBin < 0) ? 0 : minBin;
    maxBin_ = (maxBin > SpectralConstants::NUM_BINS) ? SpectralConstants::NUM_BINS : maxBin;
    if (minBin_ >= maxBin_) {
        minBin_ = 1;
        maxBin_ = 64;
    }
}

DetectionResult SpectralFluxDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* magnitudes = frame.magnitudes;
    int numBins = frame.numBins;
    uint32_t nowMs = frame.timestampMs;

    // Need at least one previous frame for flux
    if (!hasPrevFrame_) {
        // Save current frame for next iteration
        for (int i = 0; i < numBins && i < SpectralConstants::NUM_BINS; i++) {
            prevMagnitudes_[i] = magnitudes[i];
        }
        hasPrevFrame_ = true;
        return DetectionResult::none();
    }

    // Compute spectral flux
    currentFlux_ = computeFlux(magnitudes, numBins);

    // Update running average (EMA, ~0.5s time constant at 60fps)
    const float alpha = 0.03f;
    averageFlux_ += alpha * (currentFlux_ - averageFlux_);

    // Store for debugging
    lastRawValue_ = currentFlux_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentFlux_);

    // Detection: flux exceeds adaptive threshold
    // NOTE: Cooldown now applied at ensemble level (EnsembleFusion), not per-detector
    bool isLoudEnough = currentFlux_ > effectiveThreshold;

    DetectionResult result;

    if (isLoudEnough) {
        // Strength: 0 at threshold, 1 at 2x threshold
        float ratio = currentFlux_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentFlux_, localMedian);

        result = DetectionResult::hit(strength, confidence);
        // NOTE: markTransient() removed - cooldown now at ensemble level
    } else {
        result = DetectionResult::none();
    }

    // Save current magnitudes for next frame
    for (int i = 0; i < numBins && i < SpectralConstants::NUM_BINS; i++) {
        prevMagnitudes_[i] = magnitudes[i];
    }

    return result;
}

float SpectralFluxDetector::computeFlux(const float* magnitudes, int numBins) {
    // SuperFlux algorithm: Half-wave rectified flux with max-filter
    // The max-filter on previous frame suppresses vibrato/pitch wobble

    float flux = 0.0f;
    int actualMax = (maxBin_ > numBins) ? numBins : maxBin_;

    for (int i = minBin_; i < actualMax; i++) {
        // Apply 3-bin max-filter to previous frame magnitudes
        // This smooths pitch variations while preserving onset edges
        float left = (i > 0) ? prevMagnitudes_[i - 1] : prevMagnitudes_[i];
        float center = prevMagnitudes_[i];
        float right = (i < numBins - 1) ? prevMagnitudes_[i + 1] : prevMagnitudes_[i];

        float maxPrev = max3(left, center, right);

        // Half-wave rectified difference
        float diff = magnitudes[i] - maxPrev;
        if (diff > 0.0f) {
            flux += diff;
        }
    }

    // Normalize by number of bins analyzed
    int binsAnalyzed = actualMax - minBin_;
    if (binsAnalyzed > 0) {
        flux /= binsAnalyzed;
    }

    return flux;
}

float SpectralFluxDetector::computeConfidence(float flux, float median) const {
    // Confidence based on:
    // 1. How far above threshold we are
    // 2. How stable the average flux is

    // Ratio-based confidence
    float ratio = flux / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Stability: if average is low and flux is high, more confident
    float stabilityConfidence = 0.7f;  // Default moderate confidence
    if (averageFlux_ > 0.001f) {
        float avgRatio = flux / averageFlux_;
        stabilityConfidence = clamp01(avgRatio / 4.0f);
    }

    // Combine
    float confidence = (ratioConfidence + stabilityConfidence) * 0.5f;

    // Spectral flux is generally reliable - base confidence is higher
    return clamp01(confidence * 0.85f + 0.15f);
}
