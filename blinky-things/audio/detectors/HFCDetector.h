#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"

/**
 * HFCDetector - High Frequency Content onset detection
 *
 * Computes weighted high-frequency content from FFT magnitude spectrum.
 * Uses quadratic weighting to emphasize higher frequencies, which is
 * effective for detecting percussive transients (cymbals, hi-hats, snares).
 *
 * Algorithm:
 * 1. Receive magnitude spectrum from SharedSpectralAnalysis
 * 2. Compute weighted HFC:
 *    HFC = sum(magnitude[i] * i^2) for i in [minBin, maxBin]
 *    (Quadratic weighting: higher bins contribute more)
 * 3. Normalize by bin count
 * 4. Track previous HFC for attack detection
 * 5. Detect when HFC > localMedian * threshold AND rapidly rising
 *
 * This is a new FFT-based implementation (not ported from existing code).
 * The old detectHFC() in AdaptiveMic was time-domain ZCR-based, which
 * doesn't capture frequency information as accurately.
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 3.0)
 * - minBin/maxBin: Frequency range (default 32-128 = 2-8kHz for HF content)
 * - attackMultiplier: Required rise from previous (default 1.2)
 * - cooldownMs: Minimum time between detections (default 80ms)
 *
 * Memory: ~50 bytes
 * CPU: <0.1ms per frame (simple weighted sum)
 */
class HFCDetector : public BaseDetector {
public:
    HFCDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::HFC; }
    const char* name() const override { return "hfc"; }
    bool requiresSpectralData() const override { return true; }

    // HFC-specific parameters
    void setAnalysisRange(int minBin, int maxBin);
    int getMinBin() const { return minBin_; }
    int getMaxBin() const { return maxBin_; }

    void setAttackMultiplier(float mult) { attackMultiplier_ = mult; }
    float getAttackMultiplier() const { return attackMultiplier_; }

    // Debug access
    float getCurrentHFC() const { return currentHfc_; }
    float getPreviousHFC() const { return prevHfc_; }
    float getAverageHFC() const { return averageHfc_; }

protected:
    void resetImpl() override;

private:
    // HFC state
    float currentHfc_;
    float prevHfc_;
    float averageHfc_;

    // Analysis range (focus on high frequencies)
    int minBin_;
    int maxBin_;

    // Parameters
    float attackMultiplier_;

    // Compute weighted HFC from magnitude spectrum
    float computeHFC(const float* magnitudes, int numBins);

    // Compute confidence
    float computeConfidence(float hfc, float median) const;
};
