#pragma once

#include <stdint.h>
#include <math.h>

/**
 * BassSpectralAnalysis - High-resolution bass analysis via Goertzel algorithm
 *
 * Computes 12 DFT bins (31.25-375 Hz) from a 512-sample window at 16 kHz,
 * giving 31.25 Hz/bin resolution — double the FFT-256's 62.5 Hz/bin.
 * This resolves kick drum energy (40-80 Hz) across multiple bins instead of
 * cramming it into 1-2 bins, improving kick vs sustained-bass discrimination.
 *
 * Uses Goertzel algorithm (not full FFT-512) since we only need 12 bins.
 * Much cheaper: ~0.7 ms vs ~3 ms for full FFT-512.
 *
 * Runs with 50% overlap: accumulates 256 new samples, then processes the
 * full 512-sample window (256 old + 256 new). This gives one bass frame
 * per FFT-256 frame, synchronized with the existing spectral pipeline.
 *
 * Processing pipeline (every ~16 ms):
 * 1. Accumulate 256 new samples into 512-sample ring buffer
 * 2. Apply Hamming window to 512 samples → float buffer (stack-local)
 * 3. Goertzel for bins 1-12 (31.25-375 Hz) → magnitudes_[12]
 * 4. Soft-knee compressor (same algorithm as SharedSpectralAnalysis)
 * 5. Per-bin whitening (independent state)
 *
 * Memory: ~1.3 KB
 * - int16_t sampleBuffer_[512] = 1024 bytes
 * - float magnitudes_[12] + prevMagnitudes_[12] = 96 bytes
 * - float binRunningMax_[12] = 48 bytes
 * - Compressor + scalar state ~50 bytes
 *
 * CPU: ~0.7 ms per frame on Cortex-M4 @ 64 MHz
 */

namespace BassConstants {
    constexpr int WINDOW_SIZE = 512;           // 512-sample analysis window
    constexpr int HOP_SIZE = 256;              // 50% overlap (= FFT-256 frame size)
    constexpr int NUM_BASS_BINS = 12;          // Bins 1-12: 31.25-375 Hz
    constexpr int FIRST_BIN = 1;               // Skip DC (bin 0)
    constexpr float SAMPLE_RATE = 16000.0f;
    constexpr float BIN_FREQ_HZ = SAMPLE_RATE / WINDOW_SIZE;  // 31.25 Hz/bin
}

class BassSpectralAnalysis {
public:
    BassSpectralAnalysis();

    /**
     * Initialize. Must be called before use.
     */
    void begin();

    /**
     * Reset all state.
     */
    void reset();

    // --- Master toggle ---
    bool enabled = false;  // Default off; enable via "set bandflux_hiresbass 1"

    // --- Compressor parameters (mirrors SharedSpectralAnalysis defaults) ---
    bool compressorEnabled = true;
    float compThresholdDb = -30.0f;
    float compRatio = 3.0f;
    float compKneeDb = 15.0f;
    float compMakeupDb = 6.0f;
    float compAttackTau = 0.001f;
    float compReleaseTau = 2.0f;

    // --- Whitening parameters ---
    bool whitenEnabled = true;
    float whitenDecay = 0.997f;
    float whitenFloor = 0.001f;

    /**
     * Add samples to the ring buffer.
     * @return true when HOP_SIZE (256) new samples accumulated → ready to process
     */
    bool addSamples(const int16_t* samples, int count);

    /**
     * Process the current 512-sample window.
     * Call after addSamples() returns true.
     */
    void process();

    /**
     * Check if new samples are ready for processing.
     */
    bool hasSamples() const { return newSampleCount_ >= BassConstants::HOP_SIZE; }

    // --- Accessors ---

    const float* getMagnitudes() const { return magnitudes_; }
    const float* getPrevMagnitudes() const { return prevMagnitudes_; }
    int getNumBins() const { return BassConstants::NUM_BASS_BINS; }
    bool hasPreviousFrame() const { return hasPrevFrame_; }
    bool isFrameReady() const { return frameReady_; }
    void resetFrameReady() { frameReady_ = false; }

private:
    // 512-sample ring buffer
    int16_t sampleBuffer_[BassConstants::WINDOW_SIZE];
    int writeIndex_;
    int totalSamplesWritten_;  // Total samples written (for initial fill)
    int newSampleCount_;       // Samples accumulated since last process()

    // Output: 12 bass bins
    float magnitudes_[BassConstants::NUM_BASS_BINS];
    float prevMagnitudes_[BassConstants::NUM_BASS_BINS];

    // Per-bin whitening state
    float binRunningMax_[BassConstants::NUM_BASS_BINS];

    // Compressor state
    float smoothedGainDb_;

    // State flags
    bool frameReady_;
    bool hasPrevFrame_;

    // --- Processing steps ---
    void savePreviousFrame();
    void applyCompressor();
    void whitenMagnitudes();

    /**
     * Goertzel magnitude for a single DFT bin k of N-point window.
     * Mathematically equivalent to |DFT[k]| from a full FFT.
     */
    static float goertzelMagnitude(const float* windowedSamples, int N, int k);

    static bool safeIsFinite(float x) {
        return (x == x) && (x >= -3.4e38f) && (x <= 3.4e38f);
    }
};
