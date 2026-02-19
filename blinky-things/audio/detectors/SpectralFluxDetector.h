#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"

/**
 * SpectralFluxDetector - SuperFlux on mel bands
 *
 * Computes half-wave rectified spectral flux on 26 mel bands with:
 * - Lag-2 comparison (current vs 2 frames ago, not 1)
 * - 3-band max-filter on reference frame for vibrato suppression
 *
 * Operating on mel bands instead of raw FFT bins provides:
 * - Perceptual frequency grouping (matches human hearing)
 * - Reduced dimensionality (26 bands vs 128 bins = less noise)
 * - Log-compressed dynamic range (from SharedSpectralAnalysis)
 *
 * Reference: Böck & Widmer, "Maximum Filter Vibrato Suppression for Onset Detection"
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 1.4)
 *
 * Memory: ~260 bytes (2 frames × 26 mel bands + state)
 * CPU: <0.1ms per frame (26 multiply-adds)
 */
class SpectralFluxDetector : public BaseDetector {
public:
    SpectralFluxDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::SPECTRAL_FLUX; }
    const char* name() const override { return "spectral"; }
    bool requiresSpectralData() const override { return true; }

    // Debug access
    float getCurrentFlux() const { return currentFlux_; }
    float getAverageFlux() const { return averageFlux_; }

protected:
    void resetImpl() override;

private:
    // Mel band history for lag-2 SuperFlux
    // lag1 = previous frame (t-1), lag2 = two frames ago (t-2)
    float melLag1_[SpectralConstants::NUM_MEL_BANDS];
    float melLag2_[SpectralConstants::NUM_MEL_BANDS];
    int frameCount_;

    // Running stats
    float currentFlux_;
    float averageFlux_;

    // Compute SuperFlux on mel bands with lag-2 max-filtered reference
    float computeMelSuperFlux(const float* melBands, int numBands) const;

    // Compute confidence based on flux magnitude
    float computeConfidence(float flux, float median) const;

    // Helper: max of 3 values
    static float max3(float a, float b, float c) {
        float m = a > b ? a : b;
        return m > c ? m : c;
    }
};
