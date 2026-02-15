#include "SpectralFluxDetector.h"
#include <math.h>

SpectralFluxDetector::SpectralFluxDetector()
    : frameCount_(0)
    , currentFlux_(0.0f)
    , averageFlux_(0.0f)
{
    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        melLag1_[i] = 0.0f;
        melLag2_[i] = 0.0f;
    }
}

void SpectralFluxDetector::resetImpl() {
    frameCount_ = 0;
    currentFlux_ = 0.0f;
    averageFlux_ = 0.0f;

    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        melLag1_[i] = 0.0f;
        melLag2_[i] = 0.0f;
    }
}

DetectionResult SpectralFluxDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* melBands = frame.melBands;
    int numBands = frame.numMelBands;

    // Need at least 2 previous frames for lag-2 comparison
    if (frameCount_ < 2) {
        // Shift history: lag2 = lag1, lag1 = current
        for (int i = 0; i < numBands && i < SpectralConstants::NUM_MEL_BANDS; i++) {
            melLag2_[i] = melLag1_[i];
            melLag1_[i] = melBands[i];
        }
        frameCount_++;
        return DetectionResult::none();
    }

    // Compute SuperFlux on mel bands (current vs lag-2 with max filter)
    currentFlux_ = computeMelSuperFlux(melBands, numBands);

    // Update running average (EMA, ~0.5s time constant at ~30 FFT fps)
    const float alpha = 0.05f;
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
    bool isLoudEnough = currentFlux_ > effectiveThreshold;

    DetectionResult result;

    if (isLoudEnough) {
        // Strength: 0 at threshold, 1 at 2x threshold
        float ratio = currentFlux_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentFlux_, localMedian);

        result = DetectionResult::hit(strength, confidence);
    } else {
        result = DetectionResult::none();
    }

    // Shift mel band history: lag2 = lag1, lag1 = current
    for (int i = 0; i < numBands && i < SpectralConstants::NUM_MEL_BANDS; i++) {
        melLag2_[i] = melLag1_[i];
        melLag1_[i] = melBands[i];
    }

    return result;
}

float SpectralFluxDetector::computeMelSuperFlux(const float* melBands, int numBands) const {
    // SuperFlux on mel bands with lag-2 reference and 3-band max filter
    //
    // Comparing against 2 frames ago (instead of 1) gives onsets more time
    // to develop, reducing premature triggering on slow attacks.
    // The 3-band max filter on the reference frame suppresses vibrato/pitch wobble.

    float flux = 0.0f;
    int actualBands = (numBands > SpectralConstants::NUM_MEL_BANDS)
                     ? SpectralConstants::NUM_MEL_BANDS : numBands;

    for (int i = 0; i < actualBands; i++) {
        // Apply 3-band max-filter to lag-2 reference (vibrato suppression)
        float left  = (i > 0) ? melLag2_[i - 1] : melLag2_[i];
        float center = melLag2_[i];
        float right = (i < actualBands - 1) ? melLag2_[i + 1] : melLag2_[i];

        float maxRef = max3(left, center, right);

        // Half-wave rectified difference (only positive = energy increase)
        float diff = melBands[i] - maxRef;
        if (diff > 0.0f) {
            flux += diff;
        }
    }

    // Normalize by number of bands
    if (actualBands > 0) {
        flux /= actualBands;
    }

    return flux;
}

float SpectralFluxDetector::computeConfidence(float flux, float median) const {
    // Ratio-based confidence
    float ratio = flux / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Stability: if average is low and flux is high, more confident
    float stabilityConfidence = 0.7f;
    if (averageFlux_ > 0.001f) {
        float avgRatio = flux / averageFlux_;
        stabilityConfidence = clamp01(avgRatio / 4.0f);
    }

    // Combine
    float confidence = (ratioConfidence + stabilityConfidence) * 0.5f;

    // Mel-band SuperFlux is generally reliable
    return clamp01(confidence * 0.85f + 0.15f);
}
