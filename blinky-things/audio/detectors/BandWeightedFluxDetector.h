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

    // === TUNING PARAMETERS ===
    // Public for SettingsRegistry registration and ConfigStorage persistence.
    // All params are safe to modify at runtime between frames.
    float gamma = 20.0f;                 // Log compression strength (1-100)
    float bassWeight = 2.0f;             // Bass band weight (0-5)
    float midWeight = 1.5f;              // Mid band weight (0-5)
    float highWeight = 0.1f;             // High band weight (0-2, low = suppress hi-hats)
    float minOnsetDelta = 0.3f;          // Min flux jump from prev frame (0-2, pad rejection)
    float bandDominanceGate = 0.0f;      // Min band-dominance ratio (0=disabled, 0-1)
    float decayRatioThreshold = 0.0f;    // Post-onset decay confirmation (0=disabled, 0-1)
    float crestGate = 0.0f;              // Spectral crest factor gate (0=disabled, 0-20)
    float perBandThreshMult = 1.5f;      // Per-band threshold multiplier (0.5-5)
    uint8_t maxBin = 64;                 // Max FFT bin to analyze (16-128)
    uint8_t confirmFrames = 3;           // Frames to wait for decay check (0-6)
    uint8_t diffFrames = 1;              // Temporal reference depth (1-3, SuperFlux diff_frames)
    bool perBandThreshEnabled = false;   // Per-band independent detection
    bool hiResBassEnabled = false;       // Hi-res bass via Goertzel (runtime toggle)
    bool peakPickEnabled = true;         // Local-max peak picking (Phase 2.6, SuperFlux-style)

    // === GETTER/SETTER API ===
    // Convenience wrappers. Some have side effects (setHiResBass resets bass history)
    // or bounds checks (setDecayFrames, setDiffFrames). New code may access public members directly.
    void setGamma(float g) { gamma = g; }
    float getGamma() const { return gamma; }
    void setBassWeight(float w) { bassWeight = w; }
    float getBassWeight() const { return bassWeight; }
    void setMidWeight(float w) { midWeight = w; }
    float getMidWeight() const { return midWeight; }
    void setHighWeight(float w) { highWeight = w; }
    float getHighWeight() const { return highWeight; }
    void setMaxBin(int bin) { maxBin = bin; }
    int getMaxBin() const { return maxBin; }
    void setMinOnsetDelta(float d) { minOnsetDelta = d; }
    float getMinOnsetDelta() const { return minOnsetDelta; }
    void setBandDominanceGate(float r) { bandDominanceGate = r; }
    float getBandDominanceGate() const { return bandDominanceGate; }
    void setDecayRatio(float r) { decayRatioThreshold = r; }
    float getDecayRatio() const { return decayRatioThreshold; }
    void setDecayFrames(int f) { if (f >= 0 && f <= MAX_CONFIRM_FRAMES) confirmFrames = static_cast<uint8_t>(f); }
    int getDecayFrames() const { return confirmFrames; }
    void setCrestGate(float c) { crestGate = c; }
    float getCrestGate() const { return crestGate; }
    void setPerBandThresh(bool e) { perBandThreshEnabled = e; }
    bool getPerBandThresh() const { return perBandThreshEnabled; }
    void setPerBandThreshMult(float m) { perBandThreshMult = m; }
    float getPerBandThreshMult() const { return perBandThreshMult; }
    void setDiffFrames(int f) { if (f >= 1 && f <= MAX_HISTORY_FRAMES) diffFrames = static_cast<uint8_t>(f); }
    int getDiffFrames() const { return diffFrames; }
    void setHiResBass(bool e);  // Has side effects (resets bass history)
    bool getHiResBass() const { return hiResBassEnabled; }
    void setPeakPickEnabled(bool e) { peakPickEnabled = e; }
    bool getPeakPickEnabled() const { return peakPickEnabled; }

    // Debug access (read-only runtime state)
    float getBassFlux() const { return bassFlux_; }
    float getMidFlux() const { return midFlux_; }
    float getHighFlux() const { return highFlux_; }
    float getCombinedFlux() const { return combinedFlux_; }
    float getAverageFlux() const { return averageFlux_; }
    float getHiResBassFlux() const { return hiResBassFlux_; }

    // Pre-threshold continuous ODF value (Phase 2.4: unified ODF for beat tracker).
    // Returns the band-weighted combination (bass*w + mid*w + high*w), NOT raw per-bin flux.
    // Post-log-compression, pre-additive-threshold, pre-cooldown, pre-peak-picking.
    float getPreThresholdFlux() const { return combinedFlux_; }

protected:
    void resetImpl() override;

private:
    // Band boundary constants (FFT bin indices at 16kHz/256-point = 62.5 Hz/bin)
    static constexpr int BASS_MIN = 1;   // 62.5 Hz
    static constexpr int BASS_MAX = 7;   // 375 Hz (exclusive: bins 1-6)
    static constexpr int MID_MIN = 7;    // 437 Hz
    static constexpr int MID_MAX = 33;   // 2000 Hz (exclusive: bins 7-32)
    static constexpr int HIGH_MIN = 33;  // 2062 Hz
    // HIGH_MAX = maxBin

    // Max bins we store (64 bins = up to 4kHz, sufficient for onset detection)
    static constexpr int MAX_STORED_BINS = 64;

    // Multi-frame history for temporal max-filter (SuperFlux diff_frames)
    // Frame 0 = most recent previous, frame 1 = 2 frames ago, etc.
    static constexpr int MAX_HISTORY_FRAMES = 3;
    float historyLogMag_[MAX_HISTORY_FRAMES][MAX_STORED_BINS];
    int historyCount_;   // How many valid history frames we have (0..MAX_HISTORY_FRAMES)
    float prevCombinedFlux_;  // Previous frame's combined flux (for onset delta check)

    // Per-band flux values (for debug/streaming)
    float bassFlux_;
    float midFlux_;
    float highFlux_;
    float combinedFlux_;

    // Running mean for additive threshold
    float averageFlux_;
    int frameCount_;

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

    // Peak picking internal state (Phase 2.6)
    float ppPrevFlux_;           // Previous frame's combined flux (for local max check)
    DetectionResult ppPendingResult_; // Buffered detection result from previous frame
    bool ppHasPending_;          // Whether there's a pending detection to confirm/reject

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

    // Get the reference frame for flux computation (respects diffFrames)
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
