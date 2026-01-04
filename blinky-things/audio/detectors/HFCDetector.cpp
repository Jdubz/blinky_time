#include "HFCDetector.h"
#include <math.h>

HFCDetector::HFCDetector()
    : currentHfc_(0.0f)
    , prevHfc_(0.0f)
    , averageHfc_(0.0f)
    , minBin_(32)    // 2 kHz @ 16kHz/256-point FFT
    , maxBin_(128)   // 8 kHz (Nyquist)
    , attackMultiplier_(1.2f)
{
}

void HFCDetector::resetImpl() {
    currentHfc_ = 0.0f;
    prevHfc_ = 0.0f;
    averageHfc_ = 0.0f;
}

void HFCDetector::setAnalysisRange(int minBin, int maxBin) {
    minBin_ = (minBin < 0) ? 0 : minBin;
    maxBin_ = (maxBin > SpectralConstants::NUM_BINS) ? SpectralConstants::NUM_BINS : maxBin;
    if (minBin_ >= maxBin_) {
        minBin_ = 32;
        maxBin_ = 128;
    }
}

DetectionResult HFCDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* magnitudes = frame.magnitudes;
    int numBins = frame.numBins;

    // Save previous HFC
    prevHfc_ = currentHfc_;

    // Compute current HFC
    currentHfc_ = computeHFC(magnitudes, numBins);

    // Update running average (EMA)
    const float alpha = 0.05f;
    averageHfc_ += alpha * (currentHfc_ - averageHfc_);

    // Store for debugging
    lastRawValue_ = currentHfc_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentHfc_);

    // Detection: HFC exceeds threshold AND rising
    // NOTE: Cooldown now applied at ensemble level (EnsembleFusion), not per-detector
    bool isLoudEnough = currentHfc_ > effectiveThreshold;
    bool isRising = currentHfc_ > prevHfc_ * attackMultiplier_;

    DetectionResult result;

    if (isLoudEnough && isRising) {
        // Strength
        float ratio = currentHfc_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentHfc_, localMedian);

        result = DetectionResult::hit(strength, confidence);
        // NOTE: markTransient() removed - cooldown now at ensemble level
    } else {
        result = DetectionResult::none();
    }

    return result;
}

float HFCDetector::computeHFC(const float* magnitudes, int numBins) const {
    // Weighted high-frequency content
    // HFC = sum(magnitude[i] * i^2) normalized
    // Quadratic weighting emphasizes higher frequencies

    float hfc = 0.0f;
    float weightSum = 0.0f;

    int actualMax = (maxBin_ > numBins) ? numBins : maxBin_;

    for (int i = minBin_; i < actualMax; i++) {
        // Quadratic weight: higher bins contribute more
        float weight = (float)(i * i);
        hfc += magnitudes[i] * weight;
        weightSum += weight;
    }

    // Normalize
    if (weightSum > 0.0f) {
        hfc /= weightSum;
    }

    return hfc;
}

float HFCDetector::computeConfidence(float hfc, float median) const {
    // HFC confidence based on:
    // 1. How far above threshold
    // 2. How rapidly rising (attack strength)

    float ratio = hfc / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 4.0f);

    // Attack strength: how much did it rise from previous?
    float attackRatio = (prevHfc_ > 0.001f) ? hfc / prevHfc_ : 2.0f;
    float attackConfidence = clamp01((attackRatio - 1.0f) / 2.0f);

    // Combine
    float confidence = (ratioConfidence + attackConfidence) * 0.5f;

    // HFC is good for percussive but can have false positives
    return clamp01(confidence * 0.8f + 0.1f);
}
