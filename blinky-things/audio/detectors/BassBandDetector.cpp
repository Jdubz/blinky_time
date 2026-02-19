#include "BassBandDetector.h"
#include <math.h>

BassBandDetector::BassBandDetector()
    : hasPrevFrame_(false)
    , minBin_(1)    // 62.5 Hz (skip DC)
    , maxBin_(6)    // 375 Hz
    , currentBassFlux_(0.0f)
    , averageBassFlux_(0.0f)
    , transientSharpness_(0.0f)
    , prevBassEnergy_(0.0f)
{
    for (int i = 0; i < MAX_BASS_BINS; i++) {
        prevBassMagnitudes_[i] = 0.0f;
    }
}

void BassBandDetector::resetImpl() {
    hasPrevFrame_ = false;
    currentBassFlux_ = 0.0f;
    averageBassFlux_ = 0.0f;
    transientSharpness_ = 0.0f;
    prevBassEnergy_ = 0.0f;

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

    // Compute bass flux and energy
    currentBassFlux_ = computeBassFlux(magnitudes, numBins);
    float currentBassEnergy = computeBassEnergy(magnitudes, numBins);

    // Compute transient sharpness: how quickly did energy change?
    // High sharpness = sudden spike (real bass hit)
    // Low sharpness = gradual change (HVAC/room noise)
    if (prevBassEnergy_ > 0.001f) {
        float energyRatio = currentBassEnergy / prevBassEnergy_;
        transientSharpness_ = (energyRatio > 1.0f) ? energyRatio : (1.0f / energyRatio);
    } else {
        transientSharpness_ = (currentBassEnergy > 0.01f) ? 10.0f : 1.0f;
    }
    prevBassEnergy_ = currentBassEnergy;

    // Update running average
    const float alpha = 0.05f;
    averageBassFlux_ += alpha * (currentBassFlux_ - averageBassFlux_);

    // Store for debugging
    lastRawValue_ = currentBassFlux_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, minAbsoluteFlux_);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentBassFlux_);

    // Detection requires:
    // 1. Flux exceeds threshold
    // 2. Absolute flux above minimum floor (rejects quiet noise)
    // 3. Transient is sharp enough (rejects gradual HVAC changes)
    bool isLoudEnough = currentBassFlux_ > effectiveThreshold;
    bool aboveAbsoluteFloor = currentBassFlux_ > minAbsoluteFlux_;
    bool isSharpEnough = transientSharpness_ > sharpnessThreshold_;

    DetectionResult result;

    if (isLoudEnough && aboveAbsoluteFloor && isSharpEnough) {
        // Strength
        float ratio = currentBassFlux_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence (includes sharpness factor)
        float confidence = computeConfidence(currentBassFlux_, localMedian, transientSharpness_);

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

float BassBandDetector::computeBassFlux(const float* magnitudes, int numBins) const {
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

float BassBandDetector::computeBassEnergy(const float* magnitudes, int numBins) const {
    // Total energy in bass bins
    float energy = 0.0f;
    int actualMax = (maxBin_ < numBins) ? maxBin_ : numBins;

    for (int i = minBin_; i < actualMax; i++) {
        energy += magnitudes[i] * magnitudes[i];
    }

    return energy;
}

float BassBandDetector::computeConfidence(float flux, float median, float sharpness) const {
    // Bass confidence based on how clearly the flux stands out
    float ratio = flux / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Sharpness contributes to confidence (sharp transients are more reliable)
    float sharpnessConfidence = clamp01((sharpness - 1.0f) / 4.0f);

    // Combined confidence
    float baseConfidence = (ratioConfidence * 0.6f + sharpnessConfidence * 0.4f);

    // Bass is usually reliable when it fires with good sharpness
    return clamp01(baseConfidence * 0.85f + 0.15f);
}
