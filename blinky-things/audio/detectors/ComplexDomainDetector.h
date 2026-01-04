#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"

/**
 * ComplexDomainDetector - Phase deviation onset detection
 *
 * Detects soft onsets (piano, guitar picks, pitched instruments) that don't
 * have strong amplitude spikes but DO have phase discontinuities.
 *
 * The complex domain approach uses both magnitude AND phase:
 * - Steady-state signals have predictable phase evolution
 * - Onsets cause phase deviation from prediction
 * - Combined with magnitude gives robust soft onset detection
 *
 * Algorithm:
 * 1. Receive magnitude AND phase spectrum from SharedSpectralAnalysis
 * 2. Compute target phase (unwrapped prediction from previous frames):
 *    targetPhase[i] = 2 * prevPhase[i] - prevPrevPhase[i]
 * 3. Compute phase deviation:
 *    phaseDev[i] = |phase[i] - targetPhase[i]| (wrapped to [-pi, pi])
 * 4. Compute complex domain onset function:
 *    CD = sum(magnitude[i] * phaseDev[i]) / numBins
 * 5. Detect when CD > localMedian * threshold
 *
 * Reference: Bello et al., "A Tutorial on Onset Detection in Music Signals"
 *
 * Parameters:
 * - threshold: Detection threshold as ratio (default 2.0)
 * - minBin/maxBin: Frequency range (default 1-64)
 * - cooldownMs: Minimum time between detections (default 80ms)
 *
 * Memory: ~600 bytes (two phase history buffers)
 * CPU: <0.2ms per frame
 */
class ComplexDomainDetector : public BaseDetector {
public:
    ComplexDomainDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::COMPLEX_DOMAIN; }
    const char* name() const override { return "complex"; }
    bool requiresSpectralData() const override { return true; }

    // Complex domain parameters
    void setAnalysisRange(int minBin, int maxBin);
    int getMinBin() const { return minBin_; }
    int getMaxBin() const { return maxBin_; }

    void setCooldownMs(uint16_t ms) { cooldownMs_ = ms; }
    uint16_t getCooldownMs() const { return cooldownMs_; }

    // Debug access
    float getCurrentCD() const { return currentCD_; }
    float getAverageCD() const { return averageCD_; }

protected:
    void resetImpl() override;

private:
    // Phase history (two frames needed for prediction)
    float prevPhases_[SpectralConstants::NUM_BINS];
    float prevPrevPhases_[SpectralConstants::NUM_BINS];
    int frameCount_;  // Track how many frames we've seen

    // Analysis range
    int minBin_;
    int maxBin_;

    // Running stats
    float currentCD_;
    float averageCD_;

    // Parameters
    uint16_t cooldownMs_;

    // Compute complex domain onset function
    float computeComplexDomain(const float* magnitudes, const float* phases, int numBins);

    // Wrap phase difference to [-pi, pi]
    static float wrapPhase(float phase);

    // Compute confidence
    float computeConfidence(float cd, float median) const;

    // Pi constant (using CD_ prefix to avoid Arduino PI macro conflict)
    static constexpr float CD_PI = 3.14159265358979f;
};
