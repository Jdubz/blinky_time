#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"

/**
 * MelFluxDetector - Mel-scaled spectral flux for perceptual accuracy
 *
 * Uses mel-scaled frequency bands to match human hearing perception.
 * A kick drum at 80Hz and a snare at 200Hz sound perceptually similar
 * in terms of "attack"; linear FFT bins treat them very differently.
 *
 * Algorithm:
 * 1. Receive mel bands from SharedSpectralAnalysis (26 log-compressed bands)
 * 2. Compute mel-scaled spectral flux:
 *    melFlux = sum(max(melBand[j] - prevMelBand[j], 0))
 * 3. Detect when melFlux > localMedian * threshold
 *
 * Benefits:
 * - Matches human perception of frequency
 * - Log compression (dB scale) reduces dynamic range issues
 * - Broadband transients naturally emphasized
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 2.5)
 * - cooldownMs: Minimum time between detections (default 80ms)
 *
 * Memory: ~150 bytes
 * CPU: <0.1ms per frame (mel bands pre-computed by SharedSpectralAnalysis)
 */
class MelFluxDetector : public BaseDetector {
public:
    MelFluxDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::MEL_FLUX; }
    const char* name() const override { return "mel"; }
    bool requiresSpectralData() const override { return true; }

    // Mel flux parameters
    void setCooldownMs(uint16_t ms) { cooldownMs_ = ms; }
    uint16_t getCooldownMs() const { return cooldownMs_; }

    // Debug access
    float getCurrentMelFlux() const { return currentMelFlux_; }
    float getAverageMelFlux() const { return averageMelFlux_; }

protected:
    void resetImpl() override;

private:
    // Previous mel bands (local copy for flux computation)
    float prevMelBands_[SpectralConstants::NUM_MEL_BANDS];
    bool hasPrevFrame_;

    // Running stats
    float currentMelFlux_;
    float averageMelFlux_;

    // Parameters
    uint16_t cooldownMs_;

    // Compute mel flux
    float computeMelFlux(const float* melBands, int numBands);

    // Compute confidence
    float computeConfidence(float flux, float median) const;
};
