#include "BassBandDetector.h"
#include <math.h>

BassBandDetector::BassBandDetector()
    : hasPrevFrame_(false)
    , minBin_(1)    // 62.5 Hz (skip DC)
    , maxBin_(6)    // 375 Hz
    , currentBassFlux_(0.0f)
    , averageBassFlux_(0.0f)
{
    for (int i = 0; i < MAX_BASS_BINS; i++) {
        prevBassMagnitudes_[i] = 0.0f;
    }
}

void BassBandDetector::resetImpl() {
    hasPrevFrame_ = false;
    currentBassFlux_ = 0.0f;
    averageBassFlux_ = 0.0f;

    for (int i = 0; i < MAX_BASS_BINS; i++) {
        prevBassMagnitudes_[i] = 0.0f;
    }
}

void BassBandDetector::setAnalysisRange(int minBin, int maxBin) {
    minBin_ = (minBin < 0) ? 0 : minBin;
    maxBin_ = (maxBin > MAX_BASS_BINS) ? MAX_BASS_BINS : maxBin;
    if (maxBin_ > SpectralConstants::NUM_BINS) {
        maxBin_ = SpectralConstants::NUM_BINS;
    }
    if (minBin_ >= maxBin_) {
        minBin_ = 1;
        maxBin_ = 6;
    }
}

DetectionResult BassBandDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* magnitudes = frame.magnitudes;
    int numBins = frame.numBins;
    uint32_t nowMs = frame.timestampMs;

    // Need at least one previous frame for flux
    if (!hasPrevFrame_) {
        // Save current bass magnitudes
        int binsToSave = (maxBin_ < numBins) ? maxBin_ : numBins;
        for (int i = minBin_; i < binsToSave && (i - minBin_) < MAX_BASS_BINS; i++) {
            prevBassMagnitudes_[i - minBin_] = magnitudes[i];
        }
        hasPrevFrame_ = true;
        return DetectionResult::none();
    }

    // Compute bass flux
    currentBassFlux_ = computeBassFlux(magnitudes, numBins);

    // Update running average
    const float alpha = 0.05f;
    averageBassFlux_ += alpha * (currentBassFlux_ - averageBassFlux_);

    // Store for debugging
    lastRawValue_ = currentBassFlux_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentBassFlux_);

    // Detection: bass flux exceeds threshold
    // NOTE: Cooldown now applied at ensemble level (EnsembleFusion), not per-detector
    // Note: flux is already a change measure, so no "sudden" check needed
    bool isLoudEnough = currentBassFlux_ > effectiveThreshold;

    DetectionResult result;

    if (isLoudEnough) {
        // Strength
        float ratio = currentBassFlux_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentBassFlux_, localMedian);

        result = DetectionResult::hit(strength, confidence);
        // NOTE: markTransient() removed - cooldown now at ensemble level
    } else {
        result = DetectionResult::none();
    }

    // Save current bass magnitudes for next frame
    int binsToSave = (maxBin_ < numBins) ? maxBin_ : numBins;
    for (int i = minBin_; i < binsToSave && (i - minBin_) < MAX_BASS_BINS; i++) {
        prevBassMagnitudes_[i - minBin_] = magnitudes[i];
    }

    return result;
}

float BassBandDetector::computeBassFlux(const float* magnitudes, int numBins) {
    // Half-wave rectified spectral flux on bass bins only
    float flux = 0.0f;
    int binsAnalyzed = 0;

    int actualMax = (maxBin_ < numBins) ? maxBin_ : numBins;

    for (int i = minBin_; i < actualMax; i++) {
        int prevIdx = i - minBin_;
        if (prevIdx >= MAX_BASS_BINS) break;

        float diff = magnitudes[i] - prevBassMagnitudes_[prevIdx];
        if (diff > 0.0f) {
            flux += diff;
        }
        binsAnalyzed++;
    }

    // Normalize
    if (binsAnalyzed > 0) {
        flux /= binsAnalyzed;
    }

    return flux;
}

float BassBandDetector::computeConfidence(float flux, float median) const {
    // Bass confidence based on how clearly the flux stands out
    float ratio = flux / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Bass is usually reliable when it fires
    return clamp01(ratioConfidence * 0.85f + 0.15f);
}
