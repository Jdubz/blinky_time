#pragma once

#include <stdint.h>

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
    constexpr int NUM_MEL_BANDS = 26;       // Standard for speech/music analysis
    constexpr float MEL_MIN_FREQ = 60.0f;   // Hz (below fundamental bass)
    constexpr float MEL_MAX_FREQ = 8000.0f; // Hz (Nyquist limit at 16kHz)

    // Frequency bin ranges for different detectors
    constexpr int BASS_MIN_BIN = 1;    // 62.5 Hz
    constexpr int BASS_MAX_BIN = 6;    // 375 Hz
    constexpr int MID_MIN_BIN = 7;     // 437.5 Hz
    constexpr int MID_MAX_BIN = 32;    // 2 kHz
    constexpr int HIGH_MIN_BIN = 33;   // 2.0625 kHz
    constexpr int HIGH_MAX_BIN = 128;  // 8 kHz
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
     * Check if enough samples are buffered for processing
     */
    bool hasSamples() const { return sampleCount_ >= SpectralConstants::FFT_SIZE; }

    /**
     * Clear the frame ready flag (call after all detectors have processed)
     */
    void resetFrameReady() { frameReady_ = false; }

    // --- Accessors for spectral data ---

    /**
     * Get magnitude spectrum (128 bins)
     * Valid after process() returns, until next process() call
     */
    const float* getMagnitudes() const { return magnitudes_; }

    /**
     * Get phase spectrum (128 bins, in radians [-pi, pi])
     * Valid after process() returns, until next process() call
     */
    const float* getPhases() const { return phases_; }

    /**
     * Get previous frame magnitudes (for spectral flux computation)
     */
    const float* getPrevMagnitudes() const { return prevMagnitudes_; }

    /**
     * Get mel-scaled bands (26 bands)
     * Log-compressed, suitable for perceptual analysis
     * Valid after process() returns, until next process() call
     */
    const float* getMelBands() const { return melBands_; }

    /**
     * Get previous frame mel bands (for mel flux computation)
     */
    const float* getPrevMelBands() const { return prevMelBands_; }

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

private:
    // Sample ring buffer
    int16_t sampleBuffer_[SpectralConstants::FFT_SIZE];
    int sampleCount_;
    int writeIndex_;

    // FFT buffers (for in-place FFT computation)
    float vReal_[SpectralConstants::FFT_SIZE];
    float vImag_[SpectralConstants::FFT_SIZE];

    // Output buffers
    float magnitudes_[SpectralConstants::NUM_BINS];      // Raw magnitudes (HFC, ComplexDomain use these)
    float phases_[SpectralConstants::NUM_BINS];
    float prevMagnitudes_[SpectralConstants::NUM_BINS];
    float melBands_[SpectralConstants::NUM_MEL_BANDS];   // Whitened mel bands (SpectralFlux, Novelty use these)
    float prevMelBands_[SpectralConstants::NUM_MEL_BANDS];

    // Mel-band whitening: per-band running maximum for adaptive normalization
    // Applied to mel bands (not raw magnitudes) because:
    // - HFC/ComplexDomain need raw magnitudes for absolute energy metrics
    // - SpectralFlux/Novelty compute change metrics that benefit from normalization
    // - 26 bands vs 128 bins = less memory, more perceptually meaningful
    float melRunningMax_[SpectralConstants::NUM_MEL_BANDS];

    // Derived features (computed from raw magnitudes)
    float totalEnergy_;
    float spectralCentroid_;

    // State
    bool frameReady_;
    bool hasPrevFrame_;

    // Helper methods
    void applyHammingWindow();
    void computeFFT();
    void computeMagnitudesAndPhases();
    void computeMelBands();
    void whitenMelBands();
    void computeDerivedFeatures();
    void savePreviousFrame();

    // Mel filterbank helpers
    static float hzToMel(float hz);
    static float melToHz(float mel);
    static int hzToBin(float hz);

    // Portable isfinite check
    static bool safeIsFinite(float x) {
        return (x == x) && (x >= -3.4e38f) && (x <= 3.4e38f);
    }

    // Clamp to 0-1 range
    static float clamp01(float x) {
        return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    }
};
