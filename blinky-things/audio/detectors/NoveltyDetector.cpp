#include "NoveltyDetector.h"
#include <math.h>

NoveltyDetector::NoveltyDetector()
    : hasPrevFrame_(false)
    , currentNovelty_(0.0f)
    , averageNovelty_(0.0f)
{
    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = 0.0f;
    }
}

void NoveltyDetector::resetImpl() {
    hasPrevFrame_ = false;
    currentNovelty_ = 0.0f;
    averageNovelty_ = 0.0f;

    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = 0.0f;
    }
}

DetectionResult NoveltyDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* melBands = frame.melBands;
    int numBands = frame.numMelBands;

    // Need at least one previous frame for comparison
    if (!hasPrevFrame_) {
        for (int i = 0; i < numBands && i < SpectralConstants::NUM_MEL_BANDS; i++) {
            prevMelBands_[i] = melBands[i];
        }
        hasPrevFrame_ = true;
        return DetectionResult::none();
    }

    // Compute cosine distance (spectral shape change)
    currentNovelty_ = computeCosineDistance(melBands, prevMelBands_, numBands);

    // Update running average
    const float alpha = 0.05f;
    averageNovelty_ += alpha * (currentNovelty_ - averageNovelty_);

    // Store for debugging
    lastRawValue_ = currentNovelty_;

    // Compute local median for adaptive threshold
    float localMedian = computeLocalMedian();
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);
    currentThreshold_ = effectiveThreshold;

    // Update threshold buffer
    updateThresholdBuffer(currentNovelty_);

    // Detection: novelty exceeds adaptive threshold
    bool isNovel = currentNovelty_ > effectiveThreshold;

    DetectionResult result;

    if (isNovel) {
        // Strength: 0 at threshold, 1 at 2x threshold
        float ratio = currentNovelty_ / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Confidence
        float confidence = computeConfidence(currentNovelty_, localMedian);

        result = DetectionResult::hit(strength, confidence);
    } else {
        result = DetectionResult::none();
    }

    // Save current mel bands for next frame
    for (int i = 0; i < numBands && i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = melBands[i];
    }

    return result;
}

float NoveltyDetector::computeCosineDistance(const float* current, const float* previous, int numBands) const {
    // Cosine distance = 1 - cosine_similarity
    // cosine_similarity = dot(a, b) / (|a| * |b|)
    //
    // Result range: 0.0 (identical spectra) to 1.0 (orthogonal spectra)
    // Chord changes, instrument entries, and timbral shifts produce
    // values of 0.1-0.5. Steady-state is typically <0.02.

    float dotProduct = 0.0f;
    float normA = 0.0f;
    float normB = 0.0f;

    int actualBands = (numBands > SpectralConstants::NUM_MEL_BANDS)
                     ? SpectralConstants::NUM_MEL_BANDS : numBands;

    for (int i = 0; i < actualBands; i++) {
        dotProduct += current[i] * previous[i];
        normA += current[i] * current[i];
        normB += previous[i] * previous[i];
    }

    // Guard against zero-magnitude vectors (silence)
    float denominator = sqrtf(normA) * sqrtf(normB);
    if (denominator < 1e-10f) {
        return 0.0f;  // Both silent or near-silent: no novelty
    }

    float similarity = dotProduct / denominator;

    // Clamp similarity to [0, 1] (can exceed due to floating point)
    if (similarity > 1.0f) similarity = 1.0f;
    if (similarity < 0.0f) similarity = 0.0f;

    // Distance = 1 - similarity
    return 1.0f - similarity;
}

float NoveltyDetector::computeConfidence(float novelty, float median) const {
    // Cosine distance is independent of amplitude, making it reliable
    // for detecting spectral shape changes
    float ratio = novelty / maxf(median, 0.001f);
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Cosine distance is robust to volume changes
    return clamp01(ratioConfidence * 0.8f + 0.15f);
}
