#pragma once

#include "DetectionResult.h"

/**
 * IDetector - Abstract interface for onset detection algorithms
 *
 * All detectors implement this interface, allowing the EnsembleDetector
 * to run them uniformly and combine their results.
 *
 * Design principles:
 * 1. Detectors are stateful - they maintain history for threshold adaptation
 * 2. Detectors are independent - no shared mutable state between detectors
 * 3. Spectral detectors receive pre-computed FFT data to avoid redundant computation
 * 4. Each detector outputs DetectionResult with strength + confidence
 *
 * Usage:
 *   detector->configure(config);
 *   detector->reset();
 *   // In update loop:
 *   DetectionResult result = detector->detect(frame, dt);
 */
class IDetector {
public:
    virtual ~IDetector() = default;

    /**
     * Configure the detector with tuning parameters
     * Called once during setup or when parameters change
     *
     * @param config DetectorConfig with weight, threshold, enabled flag
     */
    virtual void configure(const DetectorConfig& config) = 0;

    /**
     * Get current configuration
     */
    virtual const DetectorConfig& getConfig() const = 0;

    /**
     * Detect transients in the current audio frame
     * Called once per frame (~60Hz)
     *
     * @param frame AudioFrame containing level, spectral data, timestamp
     * @param dt Time since last frame in seconds
     * @return DetectionResult with strength, confidence, and detected flag
     */
    virtual DetectionResult detect(const AudioFrame& frame, float dt) = 0;

    /**
     * Reset detector state
     * Called when switching modes or after silence
     * Clears history buffers and resets averages
     */
    virtual void reset() = 0;

    /**
     * Get detector type
     */
    virtual DetectorType type() const = 0;

    /**
     * Get detector name for logging/display
     */
    virtual const char* name() const = 0;

    /**
     * Check if detector requires spectral data
     * If true, AudioFrame must have valid spectral data before detect() is called
     */
    virtual bool requiresSpectralData() const = 0;

    /**
     * Get the last raw detection value (before thresholding)
     * Useful for debugging and visualization
     */
    virtual float getLastRawValue() const = 0;

    /**
     * Get the current adaptive threshold
     * Useful for debugging threshold behavior
     */
    virtual float getCurrentThreshold() const = 0;
};

/**
 * BaseDetector - Common functionality for all detectors
 *
 * Provides shared utilities:
 * - Local median adaptive threshold computation
 * - Threshold buffer management
 * - Cooldown tracking
 * - Configuration storage
 *
 * Subclasses implement the detection algorithm in detectImpl()
 */
class BaseDetector : public IDetector {
public:
    BaseDetector()
        : config_()
        , lastTransientMs_(0)
        , thresholdBufferIdx_(0)
        , thresholdBufferCount_(0)
        , lastRawValue_(0.0f)
        , currentThreshold_(0.0f)
    {
        for (int i = 0; i < THRESHOLD_BUFFER_SIZE; i++) {
            thresholdBuffer_[i] = 0.0f;
        }
    }

    void configure(const DetectorConfig& config) override {
        config_ = config;
    }

    const DetectorConfig& getConfig() const override {
        return config_;
    }

    void reset() override {
        thresholdBufferIdx_ = 0;
        thresholdBufferCount_ = 0;
        lastTransientMs_ = 0;
        lastRawValue_ = 0.0f;
        currentThreshold_ = 0.0f;
        for (int i = 0; i < THRESHOLD_BUFFER_SIZE; i++) {
            thresholdBuffer_[i] = 0.0f;
        }
        resetImpl();
    }

    float getLastRawValue() const override {
        return lastRawValue_;
    }

    float getCurrentThreshold() const override {
        return currentThreshold_;
    }

protected:
    // Subclass-specific reset
    virtual void resetImpl() = 0;

    // Configuration
    DetectorConfig config_;

    // Cooldown tracking
    uint32_t lastTransientMs_;

    // Local median adaptive threshold
    static constexpr int THRESHOLD_BUFFER_SIZE = 16;
    float thresholdBuffer_[THRESHOLD_BUFFER_SIZE];
    int thresholdBufferIdx_;
    int thresholdBufferCount_;

    // Debug values
    float lastRawValue_;
    float currentThreshold_;

    /**
     * Compute local median from threshold buffer
     * Uses insertion sort - efficient for small buffers
     */
    float computeLocalMedian() const {
        constexpr float COLD_START_MINIMUM = 0.01f;

        if (thresholdBufferCount_ < 3) {
            return COLD_START_MINIMUM;
        }

        // Copy to temporary for sorting
        float sorted[THRESHOLD_BUFFER_SIZE];
        int n = (thresholdBufferCount_ < THRESHOLD_BUFFER_SIZE)
                ? thresholdBufferCount_ : THRESHOLD_BUFFER_SIZE;

        for (int i = 0; i < n; i++) {
            sorted[i] = thresholdBuffer_[i];
        }

        // Insertion sort
        for (int i = 1; i < n; i++) {
            float key = sorted[i];
            int j = i - 1;
            while (j >= 0 && sorted[j] > key) {
                sorted[j + 1] = sorted[j];
                j--;
            }
            sorted[j + 1] = key;
        }

        return sorted[n / 2];
    }

    /**
     * Update threshold buffer with new value
     */
    void updateThresholdBuffer(float value) {
        thresholdBuffer_[thresholdBufferIdx_] = value;
        thresholdBufferIdx_ = (thresholdBufferIdx_ + 1) % THRESHOLD_BUFFER_SIZE;
        if (thresholdBufferCount_ < THRESHOLD_BUFFER_SIZE) {
            thresholdBufferCount_++;
        }
    }

    /**
     * Check if cooldown has elapsed
     */
    bool cooldownElapsed(uint32_t nowMs, uint32_t cooldownMs) const {
        return (int32_t)(nowMs - lastTransientMs_) > (int32_t)cooldownMs;
    }

    /**
     * Mark transient detected (updates cooldown timer)
     */
    void markTransient(uint32_t nowMs) {
        lastTransientMs_ = nowMs;
    }

    /**
     * Clamp value to 0-1 range
     */
    static float clamp01(float x) {
        return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    }

    /**
     * Return maximum of two values
     */
    static float maxf(float a, float b) {
        return a > b ? a : b;
    }

    /**
     * Return minimum of two values
     */
    static float minf(float a, float b) {
        return a < b ? a : b;
    }
};
