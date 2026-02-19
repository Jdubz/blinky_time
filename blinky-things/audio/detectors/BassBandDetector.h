#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"

/**
 * BassBandDetector - Low-frequency spectral flux for kick drums and bass
 *
 * Computes spectral flux only on low-frequency bins (62.5-375 Hz),
 * which captures kick drum fundamentals and bass drops while ignoring
 * hi-hats and cymbals.
 *
 * Algorithm:
 * 1. Receive magnitude spectrum from SharedSpectralAnalysis
 * 2. Extract bass band: bins 1-6 (62.5-375 Hz at 16kHz/256-point)
 * 3. Compute spectral flux on bass band only
 * 4. Detect when bassFlux > localMedian * threshold
 *
 * Ported from AdaptiveMic::detectBassBand() with adaptations for
 * shared FFT architecture.
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 3.0)
 * - minBin/maxBin: Frequency range (default 1-6 = 62.5-375 Hz)
 * - cooldownMs: Minimum time between detections (default 80ms)
 *
 * Memory: ~100 bytes
 * CPU: <0.1ms per frame
 */
class BassBandDetector : public BaseDetector {
public:
    BassBandDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::BASS_BAND; }
    const char* name() const override { return "bass"; }
    bool requiresSpectralData() const override { return true; }

    // Bass-specific parameters
    void setAnalysisRange(int minBin, int maxBin);
    int getMinBin() const { return minBin_; }
    int getMaxBin() const { return maxBin_; }

    // Debug access
    float getCurrentBassFlux() const { return currentBassFlux_; }
    float getAverageBassFlux() const { return averageBassFlux_; }
    float getTransientSharpness() const { return transientSharpness_; }

    // Noise rejection parameters
    void setMinAbsoluteFlux(float minFlux) { minAbsoluteFlux_ = minFlux; }
    float getMinAbsoluteFlux() const { return minAbsoluteFlux_; }
    void setSharpnessThreshold(float threshold) { sharpnessThreshold_ = threshold; }
    float getSharpnessThreshold() const { return sharpnessThreshold_; }

protected:
    void resetImpl() override;

private:
    // Previous bass magnitudes (small buffer for bass bins only)
    static constexpr int MAX_BASS_BINS = 12;  // More than enough for bass range
    float prevBassMagnitudes_[MAX_BASS_BINS];
    bool hasPrevFrame_;

    // Analysis range
    int minBin_;
    int maxBin_;

    // Running stats
    float currentBassFlux_;
    float averageBassFlux_;

    // Transient sharpness (how quickly energy changed)
    // High = sudden spike (real bass hit), Low = gradual change (HVAC/rumble)
    float transientSharpness_;
    float prevBassEnergy_;

    // Noise rejection parameters (tunable via serial: bass_minflux, bass_sharpness)
    float minAbsoluteFlux_ = 0.03f;       // Minimum flux to detect (floor for quiet noise)
    float sharpnessThreshold_ = 2.0f;     // Min sharpness ratio (lowered for EDM: room acoustics smear bass)

    // Compute bass flux
    float computeBassFlux(const float* magnitudes, int numBins) const;

    // Compute total bass energy (for sharpness calculation)
    float computeBassEnergy(const float* magnitudes, int numBins) const;

    // Compute confidence
    float computeConfidence(float flux, float median, float sharpness) const;
};
