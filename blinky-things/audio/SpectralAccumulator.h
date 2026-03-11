#pragma once

#include <string.h>

/**
 * SpectralAccumulator - Accumulates raw mel band statistics between beats
 *
 * Called every frame with raw mel bands (pre-compression, pre-whitening).
 * At beat fire, produces a feature vector for the BeatSyncNN:
 *   - 2 subdivision means (first/second half of interval): 26 × 2 = 52
 *   - Whole-beat peak: 26
 *   - Duration in frames (normalized): 1
 *   Total: 79 floats per beat
 *
 * Memory: ~416 bytes (3 × 26 × 4 + 2 × 26 × 4 + counters)
 * CPU: negligible (~26 additions per frame, division at beat fire)
 *
 * Design: Uses raw mel bands from SharedSpectralAnalysis::getRawMelBands()
 * which depend only on 8 fundamental constants (sample rate, FFT size, hop,
 * mel bands, mel range, mel scale, log compression, window). Changes to
 * compressor, whitening, BandFlux, etc. do NOT affect these features.
 */
class SpectralAccumulator {
public:
    static constexpr int MEL_BANDS = 26;

    // 26 × 2 subdivision means + 26 peak + 1 duration = 79
    static constexpr int FEATURES_PER_BEAT = MEL_BANDS * 2 + MEL_BANDS + 1;

    // Typical beat length at 120 BPM in frames (62.5 Hz frame rate)
    // Used to normalize the duration feature
    static constexpr float TYPICAL_BEAT_FRAMES = 31.25f;

    /**
     * Accumulate one frame of raw mel bands.
     * Call every frame with SharedSpectralAnalysis::getRawMelBands().
     */
    void accumulate(const float* rawMelBands) {
        // Accumulate running sum for overall mean
        for (int i = 0; i < MEL_BANDS; i++) {
            sum_[i] += rawMelBands[i];
            if (rawMelBands[i] > peak_[i]) {
                peak_[i] = rawMelBands[i];
            }
        }

        // First-half accumulation (will be finalized in getFeatures)
        // We accumulate all frames into firstHalfSum_ and track when
        // we've passed the midpoint. This is retrospective: at extraction
        // time, we know the total frame count and can split.
        if (!midpointReached_) {
            for (int i = 0; i < MEL_BANDS; i++) {
                firstHalfSum_[i] += rawMelBands[i];
            }
            firstHalfCount_++;
        } else {
            for (int i = 0; i < MEL_BANDS; i++) {
                secondHalfSum_[i] += rawMelBands[i];
            }
            secondHalfCount_++;
        }

        frameCount_++;

        // Mark midpoint when we've accumulated half the expected frames.
        // We estimate expected length from TYPICAL_BEAT_FRAMES initially,
        // then from the actual previous beat length once available.
        if (!midpointReached_ && frameCount_ >= expectedHalfFrames_) {
            midpointReached_ = true;
        }
    }

    /**
     * Extract feature vector and reset for next beat interval.
     *
     * @param outFeatures: output buffer, must have room for FEATURES_PER_BEAT floats
     */
    void getFeatures(float* outFeatures) {
        // If midpoint wasn't reached (very short interval), split evenly
        if (!midpointReached_ && frameCount_ > 1) {
            // Retrospective split: redistribute accumulated frames
            // Since all frames went to firstHalfSum_, we need to approximate.
            // Use the overall mean for both subdivisions.
            int half = frameCount_ / 2;
            if (half < 1) half = 1;

            float invTotal = (frameCount_ > 0) ? 1.0f / frameCount_ : 0.0f;
            for (int i = 0; i < MEL_BANDS; i++) {
                float avg = sum_[i] * invTotal;
                outFeatures[i] = avg;                      // first half mean
                outFeatures[MEL_BANDS + i] = avg;          // second half mean
                outFeatures[MEL_BANDS * 2 + i] = peak_[i]; // peak
            }
        } else {
            // Normal case: compute subdivision means
            float invFirst = (firstHalfCount_ > 0) ? 1.0f / firstHalfCount_ : 0.0f;
            float invSecond = (secondHalfCount_ > 0) ? 1.0f / secondHalfCount_ : 0.0f;

            for (int i = 0; i < MEL_BANDS; i++) {
                outFeatures[i] = firstHalfSum_[i] * invFirst;
                outFeatures[MEL_BANDS + i] = secondHalfSum_[i] * invSecond;
                outFeatures[MEL_BANDS * 2 + i] = peak_[i];
            }
        }

        // Duration (normalized by typical beat length)
        outFeatures[MEL_BANDS * 3] = (float)frameCount_ / TYPICAL_BEAT_FRAMES;

        // Reset for next interval.
        // Caller owns expectedHalfFrames_ via setExpectedBeatFrames() after reset.
        reset();
    }

    /**
     * Reset accumulator state without extracting features.
     * Call on tempo change or initialization.
     */
    void reset() {
        memset(sum_, 0, sizeof(sum_));
        memset(peak_, 0, sizeof(peak_));
        memset(firstHalfSum_, 0, sizeof(firstHalfSum_));
        memset(secondHalfSum_, 0, sizeof(secondHalfSum_));
        frameCount_ = 0;
        firstHalfCount_ = 0;
        secondHalfCount_ = 0;
        midpointReached_ = false;
    }

    /**
     * Set expected beat length in frames (called when tempo changes).
     * Improves subdivision split accuracy.
     */
    void setExpectedBeatFrames(int frames) {
        expectedHalfFrames_ = frames / 2;
        if (expectedHalfFrames_ < 1) expectedHalfFrames_ = 1;
    }

    /** Get current frame count in this interval. */
    uint16_t getFrameCount() const { return frameCount_; }

private:
    // Running sum of raw mel bands (for overall mean / fallback)
    float sum_[MEL_BANDS] = {0};

    // Running max of raw mel bands
    float peak_[MEL_BANDS] = {0};

    // First-half accumulation
    float firstHalfSum_[MEL_BANDS] = {0};
    uint16_t firstHalfCount_ = 0;

    // Second-half accumulation
    float secondHalfSum_[MEL_BANDS] = {0};
    uint16_t secondHalfCount_ = 0;

    // Total frame count for this interval
    uint16_t frameCount_ = 0;

    // Expected half-beat length in frames (adaptive)
    uint16_t expectedHalfFrames_ = 16;  // ~120 BPM default

    // Whether we've passed the midpoint of the current interval
    bool midpointReached_ = false;
};
