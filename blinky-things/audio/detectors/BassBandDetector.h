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

    void setCooldownMs(uint16_t ms) { cooldownMs_ = ms; }
    uint16_t getCooldownMs() const { return cooldownMs_; }

    // Debug access
    float getCurrentBassFlux() const { return currentBassFlux_; }
    float getAverageBassFlux() const { return averageBassFlux_; }

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

    // Parameters
    uint16_t cooldownMs_;

    // Compute bass flux
    float computeBassFlux(const float* magnitudes, int numBins);

    // Compute confidence
    float computeConfidence(float flux, float median) const;
};
