#pragma once
#include <stdint.h>
#include <arduinoFFT.h>
#include <math.h>

/**
 * SpectralFlux - FFT-based onset detection (header-only implementation)
 *
 * Computes spectral flux by comparing magnitude spectra between consecutive frames.
 * Spectral flux measures the amount of change in the frequency content,
 * which spikes during transients (drums, bass drops, etc.).
 *
 * Algorithm:
 * 1. Collect 256 samples (16ms at 16kHz)
 * 2. Apply Hamming window
 * 3. Compute FFT -> 128 frequency bins
 * 4. Calculate magnitude for each bin
 * 5. Compute half-wave rectified flux: sum of positive magnitude differences
 * 6. Detect spikes in flux signal
 *
 * Memory: ~2.5KB (256 floats x 2 + 128 floats)
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
    SpectralFlux()
        : sampleCount_(0)
        , writeIndex_(0)
        , currentFlux_(0.0f)
        , averageFlux_(0.0f)
        , hasPrevFrame_(false)
        , minBin_(SpectralFluxConstants::DEFAULT_MIN_BIN)
        , maxBin_(SpectralFluxConstants::DEFAULT_MAX_BIN)
    {
    }

    /**
     * Initialize the spectral flux detector
     * Must be called before use
     */
    void begin() {
        reset();
    }

    /**
     * Reset all state (call when changing modes)
     */
    void reset() {
        sampleCount_ = 0;
        writeIndex_ = 0;
        currentFlux_ = 0.0f;
        averageFlux_ = 0.0f;
        hasPrevFrame_ = false;

        // Clear buffers
        for (int i = 0; i < SpectralFluxConstants::FFT_SIZE; i++) {
            sampleBuffer_[i] = 0;
            vReal_[i] = 0.0f;
            vImag_[i] = 0.0f;
        }
        for (int i = 0; i < SpectralFluxConstants::NUM_BINS; i++) {
            prevMagnitude_[i] = 0.0f;
        }
    }

    /**
     * Add samples to the analysis buffer
     * @param samples Pointer to int16_t sample buffer
     * @param count Number of samples to add
     * @return true if a new FFT frame is ready for processing
     */
    bool addSamples(const int16_t* samples, int count) {
        for (int i = 0; i < count && sampleCount_ < SpectralFluxConstants::FFT_SIZE; i++) {
            sampleBuffer_[writeIndex_] = samples[i];
            writeIndex_ = (writeIndex_ + 1) % SpectralFluxConstants::FFT_SIZE;
            sampleCount_++;
        }
        return sampleCount_ >= SpectralFluxConstants::FFT_SIZE;
    }

    /**
     * Process the current frame and compute spectral flux
     * Call this after addSamples() returns true
     * @return Spectral flux value (0.0 = no change, higher = more change)
     */
    float process() {
        if (sampleCount_ < SpectralFluxConstants::FFT_SIZE) {
            return 0.0f;  // Not enough samples
        }

        // Copy samples to vReal_, starting from the oldest sample in ring buffer
        // Since we always fill exactly FFT_SIZE samples before processing,
        // writeIndex_ points to the oldest sample
        for (int i = 0; i < SpectralFluxConstants::FFT_SIZE; i++) {
            int idx = (writeIndex_ + i) % SpectralFluxConstants::FFT_SIZE;
            // Normalize int16 to float (-1.0 to 1.0)
            vReal_[i] = sampleBuffer_[idx] / 32768.0f;
            vImag_[i] = 0.0f;
        }

        // Apply Hamming window
        applyHammingWindow();

        // Compute FFT in place
        // ArduinoFFT requires buffer references at construction time
        // We create a temporary instance each frame (small overhead, but safe)
        ArduinoFFT<float> fft(vReal_, vImag_, SpectralFluxConstants::FFT_SIZE, 16000.0f);
        fft.compute(FFTDirection::Forward);

        // Compute magnitudes (stored back in vReal_[0..NUM_BINS-1])
        computeMagnitudes();

        // Compute spectral flux
        if (hasPrevFrame_) {
            currentFlux_ = computeFlux();

            // Update running average (exponential moving average, ~0.5s time constant at 60fps)
            const float alpha = 0.03f;  // ~33 frames to reach 63%
            averageFlux_ += alpha * (currentFlux_ - averageFlux_);

            // SAFETY: Reset if averageFlux becomes corrupted
            if (!safeIsFinite(averageFlux_)) {
                averageFlux_ = 0.0f;
            }
        } else {
            currentFlux_ = 0.0f;
            hasPrevFrame_ = true;
        }

        // Save current magnitudes for next frame
        for (int i = 0; i < SpectralFluxConstants::NUM_BINS; i++) {
            prevMagnitude_[i] = vReal_[i];
        }

        // Reset sample buffer for next frame
        sampleCount_ = 0;
        // writeIndex_ continues from where it was (ring buffer style)

        return currentFlux_;
    }

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
    void setAnalysisRange(int minBin, int maxBin) {
        minBin_ = (minBin < 0) ? 0 : minBin;
        maxBin_ = (maxBin > SpectralFluxConstants::NUM_BINS) ? SpectralFluxConstants::NUM_BINS : maxBin;
        if (minBin_ >= maxBin_) {
            minBin_ = SpectralFluxConstants::DEFAULT_MIN_BIN;
            maxBin_ = SpectralFluxConstants::DEFAULT_MAX_BIN;
        }
    }

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

    // Portable isfinite check (works on all platforms)
    static bool safeIsFinite(float x) {
        // NaN != NaN, and Inf/-Inf are outside this range
        return (x == x) && (x >= -3.4e38f) && (x <= 3.4e38f);
    }

    void applyHammingWindow() {
        // Hamming window: w(n) = 0.54 - 0.46 * cos(2*pi*n/(N-1))
        // Pre-computed coefficients would be faster but use more memory
        const float alpha = 0.54f;
        const float beta = 0.46f;
        const float twoPiOverN = 2.0f * 3.14159265f / (SpectralFluxConstants::FFT_SIZE - 1);

        for (int i = 0; i < SpectralFluxConstants::FFT_SIZE; i++) {
            float window = alpha - beta * cosf(twoPiOverN * i);
            vReal_[i] *= window;
        }
    }

    void computeMagnitudes() {
        // Compute magnitude for each frequency bin
        // Only need first half (bins 0 to NUM_BINS-1) due to symmetry
        for (int i = 0; i < SpectralFluxConstants::NUM_BINS; i++) {
            float real = vReal_[i];
            float imag = vImag_[i];

            // SAFETY: Check for NaN/Inf from FFT output
            if (!safeIsFinite(real)) real = 0.0f;
            if (!safeIsFinite(imag)) imag = 0.0f;

            // Store in vReal_ temporarily (we'll copy to prevMagnitude_ after flux calc)
            float mag = sqrtf(real * real + imag * imag);

            // SAFETY: Ensure magnitude is valid
            vReal_[i] = safeIsFinite(mag) ? mag : 0.0f;
        }
    }

    float computeFlux() {
        // SuperFlux algorithm: Half-wave rectified spectral flux with max-filter
        // The max-filter on previous frame magnitudes suppresses vibrato/pitch wobble
        // by smoothing out small frequency variations before computing differences.
        // Reference: Böck & Widmer, "Maximum Filter Vibrato Suppression for Onset Detection"
        float flux = 0.0f;

        int minB = (minBin_ < 0) ? 0 : minBin_;
        int maxB = (maxBin_ > SpectralFluxConstants::NUM_BINS) ? SpectralFluxConstants::NUM_BINS : maxBin_;

        for (int i = minB; i < maxB; i++) {
            // Apply 3-bin max-filter to previous frame magnitudes
            // This smooths pitch variations while preserving onset edges
            float maxPrev = prevMagnitude_[i];
            if (i > 0) {
                float left = prevMagnitude_[i - 1];
                if (left > maxPrev) maxPrev = left;
            }
            if (i < SpectralFluxConstants::NUM_BINS - 1) {
                float right = prevMagnitude_[i + 1];
                if (right > maxPrev) maxPrev = right;
            }

            // Half-wave rectified difference against max-filtered previous
            float diff = vReal_[i] - maxPrev;
            if (diff > 0.0f && safeIsFinite(diff)) {
                flux += diff;
            }
        }

        // Normalize by number of bins analyzed
        int numBins = maxB - minB;
        if (numBins > 0) {
            flux /= numBins;
        }

        // SAFETY: Final NaN/Inf check
        return safeIsFinite(flux) ? flux : 0.0f;
    }
};
