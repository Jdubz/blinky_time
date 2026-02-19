#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"
#include <math.h>

/**
 * NoveltyDetector - Cosine distance spectral novelty detection
 *
 * Measures how much the spectral SHAPE changes between frames,
 * independent of overall volume. Uses cosine similarity between
 * consecutive mel-band spectral vectors.
 *
 * This catches musically significant events that amplitude-based
 * detectors miss: chord changes, new instruments entering, key
 * modulations, timbral shifts. These events change spectral shape
 * dramatically even when loudness is constant.
 *
 * Algorithm:
 * 1. Receive mel bands from SharedSpectralAnalysis (26 bands)
 * 2. Compute cosine similarity between current and previous mel bands:
 *    sim = dot(prev, curr) / (|prev| * |curr|)
 * 3. Novelty = 1 - sim  (0 = identical, 1 = orthogonal)
 * 4. Detect when novelty > localMedian * threshold
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 2.5)
 *
 * Memory: ~150 bytes (26 mel bands + state)
 * CPU: <0.05ms per frame (dot product of 26 values)
 */
class NoveltyDetector : public BaseDetector {
public:
    NoveltyDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::NOVELTY; }
    const char* name() const override { return "novelty"; }
    bool requiresSpectralData() const override { return true; }

    // Debug access
    float getCurrentNovelty() const { return currentNovelty_; }
    float getAverageNovelty() const { return averageNovelty_; }

protected:
    void resetImpl() override;

private:
    // Previous mel bands for cosine distance computation
    float prevMelBands_[SpectralConstants::NUM_MEL_BANDS];
    bool hasPrevFrame_;

    // Running stats
    float currentNovelty_;
    float averageNovelty_;

    // Compute cosine distance between two spectral vectors
    float computeCosineDistance(const float* current, const float* previous, int numBands) const;

    // Compute confidence based on novelty magnitude
    float computeConfidence(float novelty, float median) const;
};
