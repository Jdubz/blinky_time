#include "ComplexDomainDetector.h"
#include <math.h>

ComplexDomainDetector::ComplexDomainDetector()
    : frameCount_(0)
    , minBin_(1)
    , maxBin_(64)
    , currentCD_(0.0f)
    , averageCD_(0.0f)
{
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevPhases_[i] = 0.0f;
        prevPrevPhases_[i] = 0.0f;
    }
}

void ComplexDomainDetector::resetImpl() {
    frameCount_ = 0;
    currentCD_ = 0.0f;
    averageCD_ = 0.0f;

    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevPhases_[i] = 0.0f;
        prevPrevPhases_[i] = 0.0f;
    }
}

void ComplexDomainDetector::setAnalysisRange(int minBin, int maxBin) {
    minBin_ = (minBin < 0) ? 0 : minBin;
    maxBin_ = (maxBin > SpectralConstants::NUM_BINS) ? SpectralConstants::NUM_BINS : maxBin;
    if (minBin_ >= maxBin_) {
        minBin_ = 1;
        maxBin_ = 64;
    }
}

DetectionResult ComplexDomainDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* magnitudes = frame.magnitudes;
    const float* phases = frame.phases;
    int numBins = frame.numBins;

    // Need at least 2 previous frames for phase prediction
    if (frameCount_ < 2) {
        // Save current phases and advance history
        for (int i = 0; i < numBins && i < SpectralConstants::NUM_BINS; i++) {
            prevPrevPhases_[i] = prevPhases_[i];
            prevPhases_[i] = phases[i];
        }
        frameCount_++;
        return DetectionResult::none();
    }

    // Compute complex domain onset function
    currentCD_ = computeComplexDomain(magnitudes, phases, numBins);

    // Update running average
    const float alpha = 0.05f;
    averageCD_ += alpha * (currentCD_ - averageCD_);

    // Store for debugging
    lastRawValue_ = currentCD_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentCD_);

    // Detection
    // NOTE: Cooldown now applied at ensemble level (EnsembleFusion), not per-detector
    bool isLoudEnough = currentCD_ > effectiveThreshold;

    DetectionResult result;

    if (isLoudEnough) {
        // Strength
        float ratio = currentCD_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentCD_, localMedian);

        result = DetectionResult::hit(strength, confidence);
        // NOTE: markTransient() removed - cooldown now at ensemble level
    } else {
        result = DetectionResult::none();
    }

    // Advance phase history
    for (int i = 0; i < numBins && i < SpectralConstants::NUM_BINS; i++) {
        prevPrevPhases_[i] = prevPhases_[i];
        prevPhases_[i] = phases[i];
    }

    return result;
}

float ComplexDomainDetector::computeComplexDomain(const float* magnitudes,
                                                   const float* phases,
                                                   int numBins) const {
    // Complex domain onset function:
    // CD = sum(magnitude[i] * |phase[i] - targetPhase[i]|) / numBins
    //
    // Phase prediction uses circular difference to avoid false positives
    // at ±pi wrap boundaries. Instead of linear 2*prev - prevPrev (which
    // breaks when phase wraps from +pi to -pi), we compute the wrapped
    // delta and extrapolate from the most recent phase.

    float cd = 0.0f;
    int binsAnalyzed = 0;

    int actualMax = (maxBin_ > numBins) ? numBins : maxBin_;

    for (int i = minBin_; i < actualMax; i++) {
        // Circular phase prediction: wrap the delta, then extrapolate
        float phaseDelta = wrapPhase(prevPhases_[i] - prevPrevPhases_[i]);
        float targetPhase = prevPhases_[i] + phaseDelta;

        // Compute phase deviation (wrapped to [-pi, pi])
        float phaseDev = wrapPhase(phases[i] - targetPhase);

        // Take absolute value of deviation
        if (phaseDev < 0.0f) phaseDev = -phaseDev;

        // Weight by magnitude (strong bins contribute more)
        cd += magnitudes[i] * phaseDev;
        binsAnalyzed++;
    }

    // Normalize
    if (binsAnalyzed > 0) {
        cd /= binsAnalyzed;
    }

    return cd;
}

float ComplexDomainDetector::wrapPhase(float phase) {
    // Safety: Check for NaN/infinity to prevent infinite loop
    if (!isfinite(phase)) return 0.0f;

    // Wrap phase to [-pi, pi] range using fmodf (safe, no infinite loop risk)
    phase = fmodf(phase, 2.0f * CD_PI);
    if (phase > CD_PI) phase -= 2.0f * CD_PI;
    if (phase < -CD_PI) phase += 2.0f * CD_PI;
    return phase;
}

float ComplexDomainDetector::computeConfidence(float cd, float median) const {
    // Complex domain is good for soft onsets
    float ratio = cd / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Generally moderate confidence - good complement to other detectors
    return clamp01(ratioConfidence * 0.75f + 0.15f);
}
