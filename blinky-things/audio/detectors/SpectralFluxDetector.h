#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"

/**
 * SpectralFluxDetector - SuperFlux spectral onset detection
 *
 * Implements the SuperFlux algorithm with max-filter vibrato suppression.
 * Computes half-wave rectified spectral flux between consecutive FFT frames.
 *
 * Algorithm:
 * 1. Receive magnitude spectrum from SharedSpectralAnalysis
 * 2. Apply 3-bin max-filter to previous frame magnitudes
 *    maxPrev[i] = max(prev[i-1], prev[i], prev[i+1])
 * 3. Compute half-wave rectified flux:
 *    flux = sum(max(current[i] - maxPrev[i], 0)) / numBins
 * 4. Detect when flux > localMedian * threshold
 *
 * Reference: Böck & Widmer, "Maximum Filter Vibrato Suppression for Onset Detection"
 *
 * Ported from SpectralFlux.h with adaptations for shared FFT architecture.
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 1.4)
 * - minBin/maxBin: Frequency range to analyze (default 1-64 = 62.5-4kHz)
 * - cooldownMs: Minimum time between detections (default 80ms)
 *
 * Memory: ~600 bytes (previous magnitude buffer + state)
 * CPU: <0.2ms per frame (uses shared FFT, just computes flux)
 */
class SpectralFluxDetector : public BaseDetector {
public:
    SpectralFluxDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::SPECTRAL_FLUX; }
    const char* name() const override { return "spectral"; }
    bool requiresSpectralData() const override { return true; }

    // Spectral flux parameters
    void setAnalysisRange(int minBin, int maxBin);
    int getMinBin() const { return minBin_; }
    int getMaxBin() const { return maxBin_; }

    // Debug access
    float getCurrentFlux() const { return currentFlux_; }
    float getAverageFlux() const { return averageFlux_; }

protected:
    void resetImpl() override;

private:
    // Previous frame magnitudes (local copy for flux computation)
    float prevMagnitudes_[SpectralConstants::NUM_BINS];
    bool hasPrevFrame_;

    // Analysis range
    int minBin_;
    int maxBin_;

    // Running stats
    float currentFlux_;
    float averageFlux_;

    // Compute SuperFlux with max-filter vibrato suppression
    float computeFlux(const float* magnitudes, int numBins) const;

    // Compute confidence based on flux stability and magnitude
    float computeConfidence(float flux, float median) const;

    // Helper: max of 3 values
    static float max3(float a, float b, float c) {
        float m = a > b ? a : b;
        return m > c ? m : c;
    }
};
