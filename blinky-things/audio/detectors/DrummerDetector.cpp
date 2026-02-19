#include "DrummerDetector.h"
#include <math.h>

DrummerDetector::DrummerDetector()
    : attackBufferIdx_(0)
    , attackBufferInitialized_(false)
    , recentAverage_(0.0f)
    , prevRawLevel_(0.0f)
    , attackMultiplier_(1.1f)   // 10% rise required
    , averageTau_(0.8f)         // ~1 second average tracking
    , minRiseRate_(0.04f)       // Minimum frame-over-frame rise (calibrated Feb 2026)
{
    for (int i = 0; i < ATTACK_BUFFER_SIZE; i++) {
        attackBuffer_[i] = 0.0f;
    }
}

void DrummerDetector::resetImpl() {
    attackBufferIdx_ = 0;
    attackBufferInitialized_ = false;
    recentAverage_ = 0.0f;
    prevRawLevel_ = 0.0f;

    for (int i = 0; i < ATTACK_BUFFER_SIZE; i++) {
        attackBuffer_[i] = 0.0f;
    }
}

DetectionResult DrummerDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled
    if (!config_.enabled) {
        return DetectionResult::none();
    }

    float rawLevel = frame.level;

    // Track recent average with exponential moving average
    float alpha = expFactor(dt, averageTau_);
    recentAverage_ += alpha * (rawLevel - recentAverage_);

    // Get baseline level from ~50-70ms ago (oldest entry in ring buffer)
    float baselineLevel = attackBuffer_[attackBufferIdx_];

    // Initialize attack buffer on first frames
    if (!attackBufferInitialized_) {
        for (int i = 0; i < ATTACK_BUFFER_SIZE; i++) {
            attackBuffer_[i] = rawLevel;
        }
        attackBufferInitialized_ = true;
    }

    // Compute local adaptive threshold using median
    float localMedian = computeLocalMedian();

    // Apply configured threshold multiplier
    float effectiveThreshold = maxf(localMedian * config_.threshold, 0.001f);

    // Store raw value for debugging
    lastRawValue_ = rawLevel;
    currentThreshold_ = effectiveThreshold;

    // Detection criteria: LOUD + SUDDEN + SHARP RISE
    // NOTE: Cooldown is now applied at ensemble level (EnsembleFusion), not per-detector
    bool isLoudEnough = rawLevel > effectiveThreshold;
    bool isAttacking = rawLevel > baselineLevel * attackMultiplier_;
    // Require minimum frame-over-frame rise to reject slow swells/crescendos
    bool isSharpRise = (rawLevel - prevRawLevel_) > minRiseRate_;

    DetectionResult result;

    if (isLoudEnough && isAttacking && isSharpRise) {
        // Calculate strength: 0.0 at threshold, 1.0 at 2x threshold
        float ratio = rawLevel / maxf(localMedian, 0.001f);
        float strength = clamp01((ratio - config_.threshold) / config_.threshold);

        // Calculate confidence based on signal clarity
        float confidence = computeConfidence(rawLevel, localMedian, ratio);

        result = DetectionResult::hit(strength, confidence);
        // NOTE: markTransient() removed - cooldown now at ensemble level
    } else {
        result = DetectionResult::none();
    }

    // Update ring buffer with current level (overwrites oldest entry)
    attackBuffer_[attackBufferIdx_] = rawLevel;
    attackBufferIdx_ = (attackBufferIdx_ + 1) % ATTACK_BUFFER_SIZE;

    // Track previous level for rise rate check
    prevRawLevel_ = rawLevel;

    // Update threshold buffer for adaptive threshold computation
    updateThresholdBuffer(rawLevel);

    return result;
}

float DrummerDetector::computeConfidence(float rawLevel, float median, float ratio) const {
    // Confidence is higher when:
    // 1. Signal is clearly above noise floor (high ratio)
    // 2. Median is stable (not fluctuating wildly)
    // 3. Recent average is consistent

    // Base confidence on how far above threshold we are
    // ratio = 2.0 at threshold, 3.0 = 1.5x threshold, 4.0 = 2x threshold
    float ratioConfidence = clamp01((ratio - 1.0f) / 3.0f);

    // Adjust based on signal-to-noise estimate
    // If rawLevel >> median, we're confident
    // If rawLevel is barely above median, less confident
    float snrConfidence = clamp01((rawLevel / maxf(median, 0.001f) - 1.0f) / 2.0f);

    // Combine: geometric mean of components
    float confidence = sqrtf(ratioConfidence * snrConfidence);

    // Clamp to reasonable range (never 100% confident on time-domain alone)
    return clamp01(confidence * 0.9f + 0.1f);
}
