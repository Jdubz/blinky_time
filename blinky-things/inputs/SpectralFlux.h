#pragma once
#include <stdint.h>

/**
 * SpectralFlux - FFT-based onset detection
 *
 * Computes spectral flux by comparing magnitude spectra between consecutive frames.
 * Spectral flux measures the amount of change in the frequency content,
 * which spikes during transients (drums, bass drops, etc.).
 *
 * Algorithm:
 * 1. Collect 256 samples (16ms at 16kHz)
 * 2. Apply Hamming window
 * 3. Compute FFT → 128 frequency bins
 * 4. Calculate magnitude for each bin
 * 5. Compute half-wave rectified flux: sum of positive magnitude differences
 * 6. Detect spikes in flux signal
 *
 * Memory: ~2.5KB (256 floats × 2 + 128 floats)
 * CPU: ~2ms per frame on Cortex-M4 @ 64MHz
 */

namespace SpectralFluxConstants {
    // FFT configuration
    constexpr int FFT_SIZE = 256;           // 256-point FFT
    constexpr int NUM_BINS = FFT_SIZE / 2;  // 128 frequency bins

    // At 16kHz sample rate:
    // - Bin 0: DC (0 Hz)
    // - Bin 1: 62.5 Hz
    // - Bin 2: 125 Hz (sub-bass)
    // - Bins 3-6: 187-375 Hz (kick drum fundamental)
    // - Bins 7-12: 437-750 Hz (snare fundamental)
    constexpr float BIN_FREQ_HZ = 16000.0f / FFT_SIZE;  // 62.5 Hz per bin

    // Default analysis range (focus on bass-mid for onset detection)
    constexpr int DEFAULT_MIN_BIN = 1;   // Skip DC
    constexpr int DEFAULT_MAX_BIN = 64;  // Up to 4kHz (captures most transient energy)
}

class SpectralFlux {
public:
    SpectralFlux();

    /**
     * Initialize the spectral flux detector
     * Must be called before use
     */
    void begin();

    /**
     * Reset all state (call when changing modes)
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
     * Process the current frame and compute spectral flux
     * Call this after addSamples() returns true
     * @return Spectral flux value (0.0 = no change, higher = more change)
     */
    float process();

    /**
     * Get the current flux value (last computed)
     */
    float getFlux() const { return currentFlux_; }

    /**
     * Get the average flux (for threshold comparison)
     */
    float getAverageFlux() const { return averageFlux_; }

    /**
     * Check if enough samples are buffered for processing
     */
    bool isFrameReady() const { return sampleCount_ >= SpectralFluxConstants::FFT_SIZE; }

    /**
     * Set analysis frequency range (in bins)
     * @param minBin Lowest bin to analyze (1 = skip DC)
     * @param maxBin Highest bin to analyze (max 128)
     */
    void setAnalysisRange(int minBin, int maxBin);

private:
    // Sample ring buffer (accumulates until we have FFT_SIZE samples)
    int16_t sampleBuffer_[SpectralFluxConstants::FFT_SIZE];
    int sampleCount_;
    int writeIndex_;

    // FFT buffers (allocated statically to avoid heap fragmentation)
    float vReal_[SpectralFluxConstants::FFT_SIZE];
    float vImag_[SpectralFluxConstants::FFT_SIZE];

    // Previous frame magnitudes for flux calculation
    float prevMagnitude_[SpectralFluxConstants::NUM_BINS];

    // State
    float currentFlux_;
    float averageFlux_;
    bool hasPrevFrame_;

    // Analysis range
    int minBin_;
    int maxBin_;

    // Internal methods
    void applyHammingWindow();
    void computeMagnitudes();
    float computeFlux();
};
