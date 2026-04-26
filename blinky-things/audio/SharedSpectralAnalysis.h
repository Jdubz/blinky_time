#pragma once

#include <stdint.h>
#include <math.h>  // isfinite() used by safeIsFinite() inline member
#include "../hal/PlatformDetect.h"

/**
 * SharedSpectralAnalysis - Shared FFT computation for ensemble detectors
 *
 * Runs FFT once per frame and provides spectral data to all detectors that need it.
 * This avoids redundant FFT computation when running multiple spectral detectors.
 *
 * Provides:
 * - Magnitude spectrum (128 bins, 0-8kHz at 16kHz sample rate)
 * - Phase spectrum (128 bins, for complex domain detection)
 * - Mel-scaled bands (26 bands, 60-8000 Hz, for perceptual analysis)
 *
 * Memory: ~4KB
 * - FFT buffers: 256*2 floats = 2KB
 * - Magnitude: 128 floats = 512B
 * - Phase: 128 floats = 512B
 * - Mel bands: 26 floats = 104B
 * - Mel filterbank: Pre-computed in PROGMEM = 0B RAM
 *
 * CPU: ~2ms per frame on Cortex-M4 @ 64MHz
 */

namespace SpectralConstants {
    // FFT configuration
    constexpr int FFT_SIZE = 256;           // 256-point FFT
    constexpr int NUM_BINS = FFT_SIZE / 2;  // 128 frequency bins (positive only)
    constexpr float SAMPLE_RATE = 16000.0f; // Sample rate in Hz
    constexpr float BIN_FREQ_HZ = SAMPLE_RATE / FFT_SIZE;  // 62.5 Hz per bin

    // Mel filterbank configuration
    // Mel band count: set to match the deployed NN model's expected input.
    // FrameOnsetNN auto-detects mel-vs-hybrid from model shape; the band
    // count itself must match the model exactly — mismatched dimensions
    // silently feed wrong frequencies. Update all three constants together
    // and re-add the triplet to the static_assert below.
    //
    // Triplet history:
    //   26 / 60-8000  — pre-v27 baseline (kept for back-compat)
    //   30 / 40-4000  — v27/v28/v29/v30/v31/v32 (b129-b150)
    //   60 / 30-8000  — v33 (forced down from 80 by disk constraints; still
    //                  2× v32 mel resolution, still extends fmax to Nyquist)
    //   80 / 30-8000  — v34+ (full Schluter '14 spec, blocked on bigger disk)
    constexpr int NUM_MEL_BANDS = 30;       // v32: 30 focused mel bands (40-4000 Hz)
    constexpr float MEL_MIN_FREQ = 40.0f;   // Hz (covers kick fundamental)
    constexpr float MEL_MAX_FREQ = 4000.0f; // Hz (snare wire cutoff; hi-hat excluded)

    // Log-mel dB range: maps [-MEL_DB_RANGE, 0] dB to [0, 1].
    // MUST match ml-training base.yaml mel_db_range.
    constexpr float MEL_DB_RANGE = 60.0f;

    // Validate mel config triplet — all three must change together.
    // Catches mismatched constants at compile time (silent frequency misalignment otherwise).
    static_assert(
        (NUM_MEL_BANDS == 26 && MEL_MIN_FREQ == 60.0f && MEL_MAX_FREQ == 8000.0f) ||
        (NUM_MEL_BANDS == 30 && MEL_MIN_FREQ == 40.0f && MEL_MAX_FREQ == 4000.0f) ||
        (NUM_MEL_BANDS == 60 && MEL_MIN_FREQ == 30.0f && MEL_MAX_FREQ == 8000.0f) ||
        (NUM_MEL_BANDS == 80 && MEL_MIN_FREQ == 30.0f && MEL_MAX_FREQ == 8000.0f),
        "Mel config mismatch: NUM_MEL_BANDS/MEL_MIN_FREQ/MEL_MAX_FREQ must be a valid triplet"
    );

    // Frequency bin ranges for different detectors
    constexpr int BASS_MIN_BIN = 1;    // 62.5 Hz
    constexpr int BASS_MAX_BIN = 6;    // 375 Hz
    constexpr int MID_MIN_BIN = 7;     // 437.5 Hz
    constexpr int MID_MAX_BIN = 32;    // 2 kHz
    constexpr int HIGH_MIN_BIN = 33;   // 2.0625 kHz
    constexpr int HIGH_MAX_BIN = 127;  // 8 kHz (last usable bin = NUM_BINS - 1)
}

/**
 * MelFilterbank - Pre-computed mel filterbank coefficients
 *
 * Stored in PROGMEM to save RAM. Each mel band is defined by:
 * - start bin, center bin, end bin (triangular filter)
 * - Filter weights are computed on-the-fly (linear interpolation)
 *
 * Mel scale formula:
 *   mel = 2595 * log10(1 + hz/700)
 *   hz = 700 * (10^(mel/2595) - 1)
 */
struct MelBandDef {
    uint8_t startBin;   // First FFT bin in this band
    uint8_t centerBin;  // Center (peak) FFT bin
    uint8_t endBin;     // Last FFT bin in this band
};

class SharedSpectralAnalysis {
public:
    SharedSpectralAnalysis();

    /**
     * Initialize the spectral analyzer
     * Must be called before use
     */
    void begin();

    /**
     * Reset all state (call when switching modes or after silence)
     */
    void reset();

    // --- Tuning parameters (public, persisted via ConfigStorage) ---

    // Per-bin spectral whitening (Stowell & Plumbley, ICMC 2007)
    bool whitenEnabled = true;
    float whitenDecay = 0.997f;    // Per-frame peak decay (~5s memory at 60fps)
    float whitenFloor = 0.001f;    // Floor to avoid amplifying noise
    bool whitenBassBypass = false;  // Skip whitening for bass bins 1-6 (preserve kick contrast)

    // Minimum statistics noise estimation + spectral subtraction (Martin 2001)
    // Per-bin noise floor tracking with oversubtraction. Inserted after FFT,
    // before preWhitenMagnitudes — benefits both BandFlux and NN paths.
    bool noiseEstEnabled = false;   // Default OFF until A/B validated on hardware
    float noiseSmoothAlpha = 0.92f;    // Power smoothing (0.9-0.99, higher = slower)
    float noiseReleaseFactor = 0.999f; // Noise floor release rate (0.99-0.9999, ~16s at 0.999)
    float noiseOversubtract = 1.5f;    // Oversubtraction factor (1.0-3.0)
    float noiseFloorRatio = 0.02f;     // Spectral floor as fraction of original (prevents zero-out)

    // Soft-knee compressor (Giannoulis/Massberg/Reiss, JAES 2012)
    bool compressorEnabled = true;
    float compThresholdDb = -30.0f;  // dB threshold
    float compRatio = 3.0f;          // 3:1 compression ratio
    float compKneeDb = 15.0f;        // Soft knee width in dB
    float compMakeupDb = 6.0f;       // Makeup gain in dB
    float compAttackTau = 0.001f;    // Attack time constant (seconds). Values below ~16ms are instantaneous at 62.5 fps
    float compReleaseTau = 2.0f;     // Release time constant (seconds). Intentionally slow: this is a
                                     // spectral normalizer (not audio output). Slow release preserves
                                     // transient dynamics while compressing sustained level differences.

    // Spectral flux band weights (v74: exposed for tuning, was hardcoded).
    // Each band's raw flux is normalized by bin count before weighting.
    // Not auto-normalized — setting all to 0.0 silences spectral flux and kills BPM.
    float bassFluxWeight = 0.5f;   // Bins 1-6: kicks (62-375 Hz)
    float midFluxWeight  = 0.2f;   // Bins 7-32: vocals/pads (437-2000 Hz)
    float highFluxWeight = 0.3f;   // Bins 33-127: snares/hi-hats (2-8 kHz)

    /**
     * Add samples to the analysis buffer
     * @param samples Pointer to int16_t sample buffer
     * @param count Number of samples to add
     * @return true if a new FFT frame is ready for processing
     */
    bool addSamples(const int16_t* samples, int count);

    /**
     * Process the current frame - compute FFT, magnitudes, phases, mel bands
     * Call this after addSamples() returns true
     * Sets frameReady flag to true when complete
     */
    void process();

    /**
     * Check if a new spectral frame is ready
     * Reset by calling resetFrameReady() after detectors have consumed data
     */
    bool isFrameReady() const { return frameReady_; }

    /**
     * Monotonic frame counter — incremented each time process() produces a new frame.
     * Use this instead of isFrameReady() when you need to detect new frames
     * independently of the ensemble detector's resetFrameReady() call.
     */
    uint32_t getFrameCount() const { return frameCount_; }

    /**
     * Check if enough samples are buffered for processing
     */
    bool hasSamples() const { return sampleCount_ >= SpectralConstants::FFT_SIZE; }

    /**
     * Clear the frame ready flag (call after all detectors have processed)
     */
    void resetFrameReady() { frameReady_ = false; }

    // --- Accessors for spectral data ---

    /**
     * Get magnitude spectrum (128 bins) — compressed + whitened
     * After process(): magnitudes are soft-knee compressed then per-bin whitened.
     * totalEnergy_ and spectralCentroid_ reflect the pre-whitened (compressed-only) state.
     * Valid after process() returns, until next process() call
     */
    const float* getMagnitudes() const { return magnitudes_; }

    /**
     * Get raw magnitude spectrum (128 bins) — NO compression, NO whitening
     * Saved after computeMagnitudesAndPhases() but before applyCompressor().
     * BandFlux already applies log(1+gamma*mag) internally, making upstream
     * compression and whitening redundant. This matches how reference systems
     * (SuperFlux, BTrack) feed their ODF: raw FFT magnitudes only.
     */
    const float* getPreWhitenMagnitudes() const { return preWhitenMagnitudes_; }

    /**
     * Get phase spectrum (128 bins, in radians [-pi, pi])
     * Valid after process() returns, until next process() call
     */
    const float* getPhases() const { return phases_; }

    /**
     * Get previous frame magnitudes (compressed-but-not-whitened).
     * Saved after applyCompressor(), before whitenMagnitudes().
     * Used internally for spectral flux computation.
     */
    const float* getPrevMagnitudes() const { return prevMagnitudes_; }

    /**
     * Get mel-scaled bands (26 bands) — compressed + whitened
     * Log-compressed, suitable for perceptual analysis
     * Valid after process() returns, until next process() call
     */
    const float* getMelBands() const { return melBands_; }

    /**
     * Get mel-scaled bands (26 bands) — post-noise-subtraction, NO compression, NO whitening
     * Computed from pre-compressor magnitudes with only log compression.
     * When noise estimation is enabled, these bands reflect noise-subtracted spectra.
     * Closely matches the training pipeline (firmware_mel_spectrogram()).
     * Used for NN inference and mic calibration streaming.
     *
     * Note: firmware applies a 1e-6 silence threshold (bandEnergy < 1e-6 → 0.0)
     * that the Python path does not. In practice this only affects near-silent
     * frames and has negligible impact on inference accuracy.
     */
    const float* getRawMelBands() const { return rawMelBands_; }

    /** Linear mel energy (before log compression). Used by FrameOnsetNN
     *  for PCEN normalization — PCEN needs linear input, not log. */
    const float* getLinearMelBands() const { return linearMelBands_; }

    /**
     * Get number of FFT bins
     */
    int getNumBins() const { return SpectralConstants::NUM_BINS; }

    /**
     * Get number of mel bands
     */
    int getNumMelBands() const { return SpectralConstants::NUM_MEL_BANDS; }

    /**
     * Check if we have a previous frame (for flux calculations)
     */
    bool hasPreviousFrame() const { return hasPrevFrame_; }

    /**
     * Get total spectral energy (sum of magnitudes)
     */
    float getTotalEnergy() const { return totalEnergy_; }

    /**
     * Get spectral centroid (center of mass of spectrum)
     * Returns frequency in Hz
     */
    float getSpectralCentroid() const { return spectralCentroid_; }

    /**
     * Get band-weighted half-wave rectified spectral flux (HWR).
     * Computed from compressed-but-not-whitened magnitudes (after soft-knee
     * compressor, before per-bin whitening) to preserve absolute transient
     * contrast. Weighted: bass 50% (bins 1-6), mid 20% (7-32), high 30% (33-127).
     * Peaks at broadband transients (kicks, snares), zero during sustain.
     * Independent of NN — drives ACF + comb bank for BPM estimation.
     */
    float getSpectralFlux() const { return spectralFlux_; }

    /**
     * Get bass-only half-wave rectified spectral flux (bins 1-6, 62-375 Hz).
     * Isolates kick drum energy — the primary periodic signal for pattern detection.
     * Normalized by bass bin count for consistent scaling.
     */
    float getBassFlux() const { return bassFlux_; }

    /**
     * Get spectral flatness (Wiener entropy) of current frame.
     * Range 0-1: 0 = pure tone, 1 = white noise.
     * Drums are noise-like (~0.5-0.8), pitched instruments are tonal (~0.1-0.3).
     * Computed from compressed magnitudes. Scale-invariant, so value matches
     * training regardless of compressor gain.
     */
    float getSpectralFlatness() const { return spectralFlatness_; }

    /**
     * Get raw SuperFlux spectral flux from PRE-COMPRESSOR magnitudes.
     * Matches the training pipeline's STFT-based flux computation exactly.
     * Used exclusively for NN hybrid input — NOT for ACF tempo estimation
     * (which uses the compressed getSpectralFlux() for absolute contrast).
     *
     * The regular getSpectralFlux() operates on compressed magnitudes where
     * per-frame gain changes corrupt magnitude differences. This raw version
     * uses the same pre-compressor magnitudes as the mel bands (getRawMelBands),
     * ensuring all NN features are in the same domain.
     */
    float getRawSpectralFlux() const { return rawSpectralFlux_; }

    // --- Phase 2a shape features (pre-compressor, pre-whitening) ---
    // All computed from preWhitenMagnitudes_ in computeShapeFeaturesRaw(),
    // matching the Python reference in ml-training/analysis/features.py.

    /**
     * Spectral centroid — centre-of-mass bin index on raw magnitudes.
     * Drums (broadband) shift centroid higher per hit; tonal impulses
     * stay concentrated at the fundamental. Range [0, NUM_BINS-1).
     */
    float getRawCentroid() const { return rawCentroid_; }

    /**
     * Crest factor — peak / RMS on raw magnitudes.
     * Transients have high crest; sustained tones are low crest.
     */
    float getRawCrest() const { return rawCrest_; }

    /**
     * Spectral rolloff — bin index below which 85% of energy lies.
     * Narrow-band tonal impulses have low rolloff; drums have high.
     */
    float getRawRolloff() const { return rawRolloff_; }

    /**
     * High-frequency content (Masri 1996): sum of k · |X[k]|² across bins.
     * Percussion has broadband HF energy; tonal impulses are low-freq-dominant.
     */
    float getRawHFC() const { return rawHFC_; }

    /**
     * Raw flatness (Wiener entropy on pre-compressor magnitudes).
     * Range [0, 1]: 0 = pure tone, 1 = white noise.
     *
     * THIS is what the NN consumes — AudioTracker routes this value into
     * FrameOnsetNN::infer. The separate `spectralFlatness_` (compressed-mag
     * version, via `getSpectralFlatness`) is kept for stream continuity but
     * is NOT used by the NN; it was the result of an historical oversight
     * where the model trained on raw-mag flatness while firmware streamed
     * compressed-mag flatness. Aligned in b136 (gap 4 in the plan).
     */
    float getRawFlatness() const { return rawFlatness_; }

    // --- Parity-test hooks (tests/parity/ only) ---
    // These let the native parity harness inject a known magnitude spectrum
    // and invoke the shape-feature math in isolation — without running FFT,
    // noise subtraction, or compressor. No-op / low-cost in production; the
    // compiler will typically inline the setter call that never fires.

    /** Overwrite the internal pre-compressor magnitude snapshot from an
     *  externally-computed spectrum. Caller must pass exactly NUM_BINS
     *  float values (bin 0 = DC, bin 1..N-1 = positive frequencies). */
    void setPreWhitenMagnitudesForTest(const float* mags) {
        for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
            preWhitenMagnitudes_[i] = mags[i];
        }
    }

    /** Run the shape-feature computation on the currently-held
     *  preWhitenMagnitudes_. The parity harness calls this after
     *  setPreWhitenMagnitudesForTest(); production always runs it via
     *  process(). */
    void runShapeFeaturesForTest() { computeShapeFeaturesRaw(); }

    /** Run the full pre-compressor analysis chain on the currently-held
     *  preWhitenMagnitudes_. In order:
     *    1. computeRawMelBands     — mel output in rawMelBands_/linearMelBands_
     *    2. computeRawSpectralFluxAndSavePrev — raw flux + advance prev
     *    3. computeShapeFeaturesRaw — centroid/crest/rolloff/hfc/flatness
     *
     *  Also advances the internal `hasPrevFrame_` flag after the first call
     *  so the next call's raw-flux computation has a valid previous frame.
     *  Caller injects one frame at a time in temporal order — this gives
     *  the parity harness full pre-compressor feature parity. */
    void runRawFeaturesForTest() {
        computeRawMelBands();
        computeRawSpectralFluxAndSavePrev();
        computeShapeFeaturesRaw();
        hasPrevFrame_ = true;
    }

    // --- Compressor/whitening debug accessors ---

    /**
     * Get current smoothed compressor gain in dB
     * Positive = boosting, negative = attenuating
     * Includes makeup gain. 0 when compressor disabled.
     */
    float getSmoothedGainDb() const { return smoothedGainDb_; }

    /**
     * Get frame RMS level in dB (pre-compression)
     * Useful for seeing what the compressor is responding to
     */
    float getFrameRmsDb() const { return frameRmsDb_; }

private:
    // Sample ring buffer
    int16_t sampleBuffer_[SpectralConstants::FFT_SIZE];
    int sampleCount_;
    int writeIndex_;

    // FFT buffers (for in-place FFT computation)
    float vReal_[SpectralConstants::FFT_SIZE];
    float vImag_[SpectralConstants::FFT_SIZE];

    // Output buffers
    float magnitudes_[SpectralConstants::NUM_BINS];      // Compressed + whitened magnitudes (all detectors see this state)
    float preWhitenMagnitudes_[SpectralConstants::NUM_BINS]; // Post-FFT, post-noise-subtraction, PRE-compressor AND pre-whitening. Snapshot taken in process() before applyCompressor() runs. Used by raw flux + raw mel bands for the NN hybrid input (must match training pipeline's STFT magnitudes).
    float phases_[SpectralConstants::NUM_BINS];
    float prevMagnitudes_[SpectralConstants::NUM_BINS];
    float prevRawMagnitudes_[SpectralConstants::NUM_BINS]; // Previous frame pre-compressor mags (for raw flux)
    float melBands_[SpectralConstants::NUM_MEL_BANDS];   // Whitened mel bands (SpectralFlux, Novelty use these)
    float rawMelBands_[SpectralConstants::NUM_MEL_BANDS]; // Pre-compressor mel bands (noise-subtracted if enabled, no whitening) for NN + calibration
    float linearMelBands_[SpectralConstants::NUM_MEL_BANDS]; // Linear mel energy (pre-log) for PCEN
    float linearMelInvWeightSum_[SpectralConstants::NUM_MEL_BANDS]; // Precomputed 1/weightSum per band

    // Mel-band whitening: per-band running maximum for adaptive normalization
    // Applied to mel bands (not raw magnitudes) because:
    // - HFC/ComplexDomain need raw magnitudes for absolute energy metrics
    // - SpectralFlux/Novelty compute change metrics that benefit from normalization
    // - 26 bands vs 128 bins = less memory, more perceptually meaningful
    float melRunningMax_[SpectralConstants::NUM_MEL_BANDS];

    // Per-bin spectral whitening state (128 bins)
    float binRunningMax_[SpectralConstants::NUM_BINS];

    // Noise estimation state (Martin 2001 minimum statistics, simplified)
    float smoothedPower_[SpectralConstants::NUM_BINS];  // Smoothed power spectral density
    float noiseFloorEst_[SpectralConstants::NUM_BINS];  // Estimated noise floor per bin

    // Compressor state
    float smoothedGainDb_;
    float frameRmsDb_;

    // Derived features (computed from raw magnitudes)
    float totalEnergy_;
    float spectralCentroid_;
    float spectralFlux_;
    float spectralFlatness_;          // Wiener entropy: 0=tone, 1=noise (drum discriminator)
    float bassFlux_;           // Bass-only spectral flux (bins 1-6, kicks only)
    float rawSpectralFlux_;    // SuperFlux from pre-compressor mags (for NN hybrid input)
    // Phase 2a shape features — computed from preWhitenMagnitudes_ (pre-compressor)
    // to match ml-training/analysis/features.py reference implementations.
    float rawCentroid_;         // Centre-of-mass bin index on raw magnitudes
    float rawCrest_;            // max / RMS on raw magnitudes (transient-peakiness)
    float rawRolloff_;          // Bin index below which 85% of energy lies
    float rawHFC_;              // Masri 1996: sum of bin-weighted energy (k · |X|²)
    float rawFlatness_;         // Wiener entropy on raw mags (matches Python training code)

    // State
    bool frameReady_;
    bool hasPrevFrame_;
    uint32_t frameCount_;  // Monotonic counter for NN stream (survives resetFrameReady)

    // Helper methods
    void applyHammingWindow();
    void computeFFT();
    void computeMagnitudesAndPhases();
    void estimateAndSubtractNoise();
    void applyCompressor();
    void whitenMagnitudes();
    void computeMelBands();
    void computeMelBandsFrom(const float* inputMagnitudes, float* outputMelBands);
    void computeRawMelBands();
    void whitenMelBands();
    void computeDerivedFeatures();
    void computeShapeFeaturesRaw();  // Phase 2a: centroid/crest/rolloff/HFC/flatness on preWhitenMagnitudes_
    void computeRawSpectralFluxAndSavePrev();  // SuperFlux + advance prevRawMagnitudes_
    void savePrevCompressedMagnitudes();

    static bool safeIsFinite(float x) {
        return isfinite(x);
    }

    // Clamp to 0-1 range
    static float clamp01(float x) {
        return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    }
};
