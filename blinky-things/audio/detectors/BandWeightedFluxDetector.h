#pragma once

#include "../IDetector.h"
#include "../SharedSpectralAnalysis.h"
#include "../BassSpectralAnalysis.h"
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
 * Memory: ~1200 bytes (768 for 3-frame history + ~400 for state/params)
 * CPU: ~35us per frame at 64MHz (includes history shift memcpy)
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

    void setBandDominanceGate(float r) { bandDominanceGate_ = r; }
    float getBandDominanceGate() const { return bandDominanceGate_; }

    void setDecayRatio(float r) { decayRatioThreshold_ = r; }
    float getDecayRatio() const { return decayRatioThreshold_; }

    void setDecayFrames(int f) { if (f >= 0 && f <= MAX_CONFIRM_FRAMES) confirmFrames_ = f; }
    int getDecayFrames() const { return confirmFrames_; }

    void setCrestGate(float c) { crestGate_ = c; }
    float getCrestGate() const { return crestGate_; }

    void setPerBandThresh(bool e) { perBandThreshEnabled_ = e; }
    bool getPerBandThresh() const { return perBandThreshEnabled_; }

    void setPerBandThreshMult(float m) { perBandThreshMult_ = m; }
    float getPerBandThreshMult() const { return perBandThreshMult_; }

    void setDiffFrames(int f) { if (f >= 1 && f <= MAX_HISTORY_FRAMES) diffFrames_ = f; }
    int getDiffFrames() const { return diffFrames_; }

    void setHiResBass(bool e);
    bool getHiResBass() const { return hiResBassEnabled_; }

    // Debug access
    float getBassFlux() const { return bassFlux_; }
    float getMidFlux() const { return midFlux_; }
    float getHighFlux() const { return highFlux_; }
    float getCombinedFlux() const { return combinedFlux_; }
    float getAverageFlux() const { return averageFlux_; }
    float getHiResBassFlux() const { return hiResBassFlux_; }

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

    // Multi-frame history for temporal max-filter (SuperFlux diff_frames)
    // Frame 0 = most recent previous, frame 1 = 2 frames ago, etc.
    static constexpr int MAX_HISTORY_FRAMES = 3;
    float historyLogMag_[MAX_HISTORY_FRAMES][MAX_STORED_BINS];
    int historyCount_;   // How many valid history frames we have (0..MAX_HISTORY_FRAMES)
    int diffFrames_;     // Which frame to compare against (1=prev, 2=two-ago, 3=three-ago)
    float prevCombinedFlux_;  // Previous frame's combined flux (for onset delta check)

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
    float bandDominanceGate_; // Min band-dominance ratio (max band / total) to confirm onset (default 0.0 = disabled)
    float decayRatioThreshold_; // Max flux ratio after N frames to confirm percussive onset (default 0.0 = disabled)
    float crestGate_;       // Max spectral crest factor to confirm onset (default 0.0 = disabled)
    int confirmFrames_;     // Frames to wait for decay check (default 3)
    int maxBin_;            // Max FFT bin to analyze (default 64)
    bool perBandThreshEnabled_; // Per-band independent detection (default false)
    float perBandThreshMult_; // Per-band threshold multiplier for bass+mid independent detection (default 1.5)

    // Post-onset decay confirmation state
    static constexpr int MAX_CONFIRM_FRAMES = 6;
    int confirmCountdown_;      // Frames remaining until confirmation check
    float candidateFlux_;       // Combined flux at candidate onset frame
    float minFluxDuringWindow_; // Minimum flux seen during confirmation window
    DetectionResult cachedResult_; // Cached result to return on confirmation

    // Per-band running means for independent thresholds
    float averageBassFlux_;
    float averageMidFlux_;

    // Hi-res bass (Goertzel 512-sample, 12 bins at 31.25 Hz/bin)
    static constexpr int MAX_BASS_BINS = BassConstants::NUM_BASS_BINS;
    float historyBassLogMag_[MAX_HISTORY_FRAMES][MAX_BASS_BINS];
    int bassHistoryCount_;
    float hiResBassFlux_;
    bool hiResBassEnabled_;  // Runtime toggle (default false)

    // Fast log(1+x) approximation for small x
    // ~8% error at boundary (x=0.5: returns 0.375, true value 0.405).
    // With gamma=20, crossover is at mag=0.025 (very quiet), so rarely matters.
    static float fastLog1p(float x) {
        if (x < 0.5f) {
            return x * (1.0f - x * 0.5f);
        }
        return logf(1.0f + x);
    }

    // Store current frame in history ring and update reference state
    void updatePrevFrameState(const float* logMag, int effectiveMax);

    // Get the reference frame for flux computation (respects diffFrames_)
    const float* getReferenceFrame() const;

    // Compute per-band flux from current and max-filtered reference
    void computeBandFlux(const float* logMag, const float* maxRef, int numBins);

    // Compute confidence based on flux ratio
    float computeConfidence(float flux, float mean) const;

    // Hi-res bass helpers
    void computeHiResBassFlux(const float* bassLogMag, int numBins);
    void updateBassPrevFrameState(const float* bassLogMag, int numBins);
    const float* getBassReferenceFrame() const;
};
