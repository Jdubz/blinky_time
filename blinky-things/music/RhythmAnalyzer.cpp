#include "RhythmAnalyzer.h"
#include <math.h>

RhythmAnalyzer::RhythmAnalyzer() {
    reset();
}

void RhythmAnalyzer::reset() {
    writeIdx_ = 0;
    frameCount_ = 0;
    detectedPeriodMs = 0.0f;
    periodicityStrength = 0.0f;
    beatLikelihood = 0.0f;
    lastAutocorrMs_ = 0;
    lastPhaseUpdateMs_ = 0;
    currentPhase_ = 0.0f;
    frameRate_ = 60.0f;

    for (int i = 0; i < BUFFER_SIZE; i++) {
        ossHistory_[i] = 0.0f;
    }
}

void RhythmAnalyzer::addSample(float onsetStrength) {
    // Store onset strength in circular buffer
    ossHistory_[writeIdx_] = onsetStrength;
    writeIdx_ = (writeIdx_ + 1) % BUFFER_SIZE;

    if (frameCount_ < BUFFER_SIZE) {
        frameCount_++;
    }
}

bool RhythmAnalyzer::update(uint32_t nowMs, float frameRate) {
    // Store frame rate for phase calculations
    frameRate_ = frameRate;

    // Update phase with elapsed time (even when not running autocorrelation)
    if (detectedPeriodMs > 0.0f && lastPhaseUpdateMs_ > 0) {
        uint32_t dtMs = nowMs - lastPhaseUpdateMs_;
        updatePhase(dtMs);
    }
    lastPhaseUpdateMs_ = nowMs;

    // Throttle autocorrelation to reduce CPU
    // Use signed arithmetic to handle millis() wraparound
    if ((int32_t)(nowMs - lastAutocorrMs_) < (int32_t)autocorrUpdateIntervalMs) {
        return false;  // Not time for autocorrelation yet
    }
    lastAutocorrMs_ = nowMs;

    // Need full buffer for reliable autocorrelation
    if (frameCount_ < BUFFER_SIZE) {
        return false;
    }

    // Convert BPM range to frame periods
    // Example: @ 60 Hz, 120 BPM = 500ms period = 30 frames
    float msPerFrame = 1000.0f / frameRate;  // e.g., 16.67 ms @ 60 Hz
    float minPeriodFrames = (60000.0f / maxBPM) / msPerFrame;  // e.g., 18 frames @ 200 BPM, 60 Hz
    float maxPeriodFrames = (60000.0f / minBPM) / msPerFrame;  // e.g., 60 frames @ 60 BPM, 60 Hz

    // Safety: Clamp to buffer size
    if (maxPeriodFrames > BUFFER_SIZE / 2) {
        maxPeriodFrames = BUFFER_SIZE / 2;  // Can't detect periods longer than half buffer
    }
    if (minPeriodFrames < 2) {
        minPeriodFrames = 2;  // Need at least 2 frames for correlation
    }

    // Autocorrelation on OSS buffer
    float periodFrames, strength;
    autocorrelate(ossHistory_, BUFFER_SIZE, minPeriodFrames, maxPeriodFrames,
                  periodFrames, strength);

    // Store results if confidence threshold met
    if (strength > minPeriodicityStrength) {
        float newPeriodMs = periodFrames * msPerFrame;

        // Apply tempo smoothing (80% old, 20% new) if we had a previous detection
        if (detectedPeriodMs > 0.0f) {
            // Check if tempo changed significantly (>10% difference)
            float tempoDiff = fabsf(detectedPeriodMs - newPeriodMs);
            if (tempoDiff > detectedPeriodMs * TEMPO_CHANGE_THRESHOLD) {
                // Big tempo change - reset phase for resync
                currentPhase_ = 0.0f;
            }
            // Smooth tempo estimate
            detectedPeriodMs = detectedPeriodMs * TEMPO_SMOOTHING_OLD_WEIGHT + newPeriodMs * TEMPO_SMOOTHING_NEW_WEIGHT;
        } else {
            // First detection - accept immediately
            detectedPeriodMs = newPeriodMs;
            currentPhase_ = 0.0f;
        }

        periodicityStrength = strength;

        // Update cached beatLikelihood for external access
        beatLikelihood = getBeatLikelihood();

        return true;
    } else {
        // Weak or no pattern detected
        detectedPeriodMs = 0.0f;
        periodicityStrength = 0.0f;
        currentPhase_ = 0.0f;
        beatLikelihood = 0.0f;
        return false;
    }
}

void RhythmAnalyzer::autocorrelate(const float* signal, int length,
                                   float minPeriod, float maxPeriod,
                                   float& outPeriod, float& outStrength) {
    // Simple autocorrelation: R(lag) = sum(signal[i] * signal[i - lag])
    // Search for peak in autocorrelation within [minPeriod, maxPeriod]

    int minLag = (int)minPeriod;
    int maxLag = (int)maxPeriod;
    if (maxLag >= length) maxLag = length - 1;

    float maxCorr = 0.0f;
    int bestLag = minLag;  // Initialize to minimum valid lag (not 0)

    // Compute autocorrelation for each lag in range
    for (int lag = minLag; lag <= maxLag; lag++) {
        float corr = 0.0f;
        int count = 0;

        // Sum of products: signal[i] * signal[i - lag]
        for (int i = lag; i < length; i++) {
            corr += signal[i] * signal[i - lag];
            count++;
        }

        if (count > 0) {
            corr /= count;  // Normalize by number of samples

            if (corr > maxCorr) {
                maxCorr = corr;
                bestLag = lag;
            }
        }
    }

    // Normalize strength (0-1 range)
    // Compare to autocorrelation at lag=0 (signal energy)
    float energy = 0.0f;
    for (int i = 0; i < length; i++) {
        energy += signal[i] * signal[i];
    }
    energy /= length;

    outPeriod = (float)bestLag;
    outStrength = (energy > 0.0f) ? (maxCorr / energy) : 0.0f;

    // Clamp strength to [0, 1]
    outStrength = clamp(outStrength, 0.0f, 1.0f);
}

void RhythmAnalyzer::updatePhase(uint32_t dtMs) {
    // Increment phase based on detected period and elapsed time
    // Phase tracks position within beat cycle (0.0 = beat, 1.0 = next beat)

    if (detectedPeriodMs <= 0.0f) {
        currentPhase_ = 0.0f;
        return;
    }

    // Increment phase by time ratio: dtMs / periodMs
    currentPhase_ += (float)dtMs / detectedPeriodMs;

    // Wrap phase to [0, 1) - always use fmodf to handle all cases including millis() wraparound
    currentPhase_ = fmodf(currentPhase_, 1.0f);
    if (currentPhase_ < 0.0f) currentPhase_ += 1.0f;  // Handle negative result from fmodf
}

float RhythmAnalyzer::getBeatLikelihood() const {
    if (detectedPeriodMs <= 0.0f || periodicityStrength < minPeriodicityStrength) {
        return 0.0f;  // No pattern detected
    }

    // Check current onset strength against recent average
    // If we're at a beat position, onset strength should be elevated
    float currentOSS = getSample(0);  // Most recent frame

    // Compute average over one period using stored frame rate
    float msPerFrame = 1000.0f / frameRate_;
    int periodFrames = (int)(detectedPeriodMs / msPerFrame);
    if (periodFrames < 1) periodFrames = 1;
    if (periodFrames > BUFFER_SIZE - 1) periodFrames = BUFFER_SIZE - 1;

    // Compute average including current sample (i=0 to periodFrames-1)
    float avgOSS = 0.0f;
    for (int i = 0; i < periodFrames; i++) {
        avgOSS += getSample(i);
    }
    avgOSS /= periodFrames;

    // Beat likelihood: current OSS relative to period average
    float ratio = (avgOSS > 0.0f) ? (currentOSS / avgOSS) : 0.0f;

    // Also consider phase: likelihood peaks near phase = 0 (beat position)
    // Phase modulation: cos(2π * phase) maps to [-1, 1], shift to [0, 1]
    float phaseFactor = 0.5f + 0.5f * cosf(TWO_PI * currentPhase_);

    // Combine ratio and phase, weighted by periodicity strength
    float likelihood = (ratio - 1.0f) * phaseFactor * periodicityStrength;

    // Clamp to [0, 1]
    return clamp(likelihood, 0.0f, 1.0f);
}

bool RhythmAnalyzer::confirmPastBeat(int framesAgo, float threshold) {
    if (framesAgo <= 0 || framesAgo >= frameCount_) {
        return false;  // Out of range
    }

    // Get OSS at target frame
    float targetOSS = getSample(framesAgo);

    // Compare to neighbors (expect spike at beat)
    float beforeOSS = getSample(framesAgo + 1);
    float afterOSS = (framesAgo > 1) ? getSample(framesAgo - 1) : 0.0f;
    float avgNeighbor = (beforeOSS + afterOSS) / 2.0f;

    // Confirmed if target is significantly higher than neighbors
    return (targetOSS > avgNeighbor * threshold);
}
