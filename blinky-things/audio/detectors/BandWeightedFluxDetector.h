#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"
#include <math.h>

/**
 * BandWeightedFluxDetector - Log-compressed band-weighted spectral flux
 *
 * Designed for low-signal-level environments (speakers at distance) where
 * multiplicative thresholds (median * factor) fail because the median is
 * near zero. Uses log compression and additive thresholds instead.
 *
 * Algorithm:
 * 1. Log-compress FFT magnitudes: log(1 + gamma * mag[k])
 * 2. 3-bin max-filter on reference frame (SuperFlux vibrato suppression)
 * 3. Half-wave rectified flux per frequency band:
 *    - Bass bins 1-6 (62-375 Hz): weight 2.0 (kicks)
 *    - Mid bins 7-32 (437-2000 Hz): weight 1.5 (snares)
 *    - High bins 33-63 (2-4 kHz): weight 0.1 (suppress hi-hats)
 * 4. Additive threshold: mean + delta (works at low signal levels)
 * 5. Asymmetric threshold update: skip buffer update on detection frames
 * 6. Hi-hat rejection gate: suppress when ONLY high band has flux
 *
 * Memory: ~400 bytes
 * CPU: ~27us per frame at 64MHz
 */
class BandWeightedFluxDetector : public BaseDetector {
public:
    BandWeightedFluxDetector();

    // IDetector interface
    DetectionResult detect(const AudioFrame& frame, float dt) override;
    DetectorType type() const override { return DetectorType::BAND_FLUX; }
    const char* name() const override { return "bandflux"; }
    bool requiresSpectralData() const override { return true; }

    // Tunable parameters
    void setGamma(float g) { gamma_ = g; }
    float getGamma() const { return gamma_; }

    void setBassWeight(float w) { bassWeight_ = w; }
    float getBassWeight() const { return bassWeight_; }

    void setMidWeight(float w) { midWeight_ = w; }
    float getMidWeight() const { return midWeight_; }

    void setHighWeight(float w) { highWeight_ = w; }
    float getHighWeight() const { return highWeight_; }

    void setMaxBin(int bin) { maxBin_ = bin; }
    int getMaxBin() const { return maxBin_; }

    void setMinOnsetDelta(float d) { minOnsetDelta_ = d; }
    float getMinOnsetDelta() const { return minOnsetDelta_; }

    // Debug access
    float getBassFlux() const { return bassFlux_; }
    float getMidFlux() const { return midFlux_; }
    float getHighFlux() const { return highFlux_; }
    float getCombinedFlux() const { return combinedFlux_; }
    float getAverageFlux() const { return averageFlux_; }

protected:
    void resetImpl() override;

private:
    // Band boundary constants (FFT bin indices at 16kHz/256-point = 62.5 Hz/bin)
    static constexpr int BASS_MIN = 1;   // 62.5 Hz
    static constexpr int BASS_MAX = 7;   // 375 Hz (exclusive: bins 1-6)
    static constexpr int MID_MIN = 7;    // 437 Hz
    static constexpr int MID_MAX = 33;   // 2000 Hz (exclusive: bins 7-32)
    static constexpr int HIGH_MIN = 33;  // 2062 Hz
    // HIGH_MAX = maxBin_

    // Max bins we store (64 bins = up to 4kHz, sufficient for onset detection)
    static constexpr int MAX_STORED_BINS = 64;

    // Previous frame log-compressed magnitudes
    float prevLogMag_[MAX_STORED_BINS];
    float prevCombinedFlux_;  // Previous frame's combined flux (for onset delta check)
    bool hasPrevFrame_;

    // Per-band flux values (for debug/streaming)
    float bassFlux_;
    float midFlux_;
    float highFlux_;
    float combinedFlux_;

    // Running mean for additive threshold
    float averageFlux_;
    int frameCount_;

    // Tunable parameters
    float gamma_;           // Log compression strength (default 20.0)
    float bassWeight_;      // Bass band weight (default 2.0)
    float midWeight_;       // Mid band weight (default 1.5)
    float highWeight_;      // High band weight (default 0.1)
    float minOnsetDelta_;   // Min flux jump from prev frame to confirm onset (default 0.3)
    int maxBin_;            // Max FFT bin to analyze (default 64)

    // Fast log(1+x) approximation for small x
    static float fastLog1p(float x) {
        // For small x, log(1+x) ~ x - x^2/2 (good for x < 0.5)
        // For larger x, fall back to logf
        if (x < 0.5f) {
            return x * (1.0f - x * 0.5f);
        }
        return logf(1.0f + x);
    }

    // Compute per-band flux from current and max-filtered reference
    void computeBandFlux(const float* logMag, const float* maxRef, int numBins);

    // Compute confidence based on flux ratio
    float computeConfidence(float flux, float mean) const;
};
