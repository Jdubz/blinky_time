#include "MusicMode.h"
#include <Arduino.h>

MusicMode::MusicMode(ISystemTime& time)
    : time_(time) {
    reset();
}

void MusicMode::reset() {
    active = false;
    bpm = 120.0f;
    phase = 0.0f;
    beatNumber = 0;

    beatHappened = false;
    quarterNote = false;
    halfNote = false;
    wholeNote = false;

    beatPeriodMs_ = 500.0f;  // 120 BPM default
    errorIntegral_ = 0.0f;
    confidence_ = 0.0f;
    stableBeats_ = 0;
    missedBeats_ = 0;
    lastMissedBeatCheck_ = 0;

    intervalIndex_ = 0;
    intervalCount_ = 0;
    lastOnsetTime_ = 0;

    // Clear interval buffer
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) {
        onsetIntervals_[i] = 0;
    }
}

void MusicMode::update(float dt) {
    // Clear one-shot events at start of frame
    beatHappened = false;
    quarterNote = false;
    halfNote = false;
    wholeNote = false;

    // Update phase (may trigger beat events)
    updatePhase(dt);

    // Check for missed beats (no onsets for too long)
    // Only check once per beat period to avoid incrementing missedBeats_ every frame
    uint32_t nowMs = time_.millis();
    uint32_t timeSinceOnset = nowMs - lastOnsetTime_;
    uint32_t timeSinceCheck = nowMs - lastMissedBeatCheck_;

    // Only check at beat intervals when active
    if (active && timeSinceCheck > beatPeriodMs_) {
        if (timeSinceOnset > beatPeriodMs_ * 1.5f) {  // 1.5x tolerance
            missedBeats_++;
            confidence_ -= 0.05f;
            if (confidence_ < 0.0f) confidence_ = 0.0f;
        }
        lastMissedBeatCheck_ = nowMs;
    }

    // Check activation/deactivation conditions
    if (!active && shouldActivate()) {
        active = true;
        Serial.println(F("[MUSIC] Mode activated"));
        Serial.print(F("[MUSIC] BPM: ")); Serial.println(bpm);
    }

    if (active && shouldDeactivate()) {
        active = false;
        stableBeats_ = 0;
        missedBeats_ = 0;
        Serial.println(F("[MUSIC] Mode deactivated"));
    }
}

void MusicMode::updatePhase(float dt) {
    // Advance phase based on current BPM
    float dtMs = dt * 1000.0f;  // Convert to milliseconds
    phase += dtMs / beatPeriodMs_;

    // Wrap phase (0.0 - 1.0) and trigger beat events
    // Handle edge case where phase >= 2.0 (large dt or very fast BPM)
    if (phase >= 1.0f) {
        // Safety: Clamp phase to prevent overflow (should never exceed ~10 in practice)
        // If phase is extremely large, something is wrong - clamp and continue
        if (phase > 100.0f) {
            Serial.print(F("[MUSIC] WARNING: Phase overflow detected: "));
            Serial.println(phase);
            phase = 1.0f;  // Reset to minimal overflow
        }

        // Count how many beats occurred (normally 1, but handles edge cases)
        uint32_t beatsToAdd = (uint32_t)phase;
        phase = fmodf(phase, 1.0f);  // Properly wrap to [0, 1)

        // Safety: Cap beatsToAdd to prevent beatNumber overflow
        // In practice, this should never exceed 2-3 beats
        if (beatsToAdd > 10) {
            Serial.print(F("[MUSIC] WARNING: Excessive beats detected: "));
            Serial.println(beatsToAdd);
            beatsToAdd = 1;  // Treat as single beat to avoid corruption
        }

        beatNumber += beatsToAdd;

        // Set beat event flags based on the final beat that occurred
        // NOTE: If multiple beats occurred (beatsToAdd > 1), intermediate beats are skipped.
        // This is acceptable behavior - it means frame rate is very low relative to BPM.
        // Consumers should check beatHappened for any beat event.
        beatHappened = true;
        quarterNote = true;              // Every beat is a quarter note
        halfNote = (beatNumber % 2 == 0);     // Every 2nd beat
        wholeNote = (beatNumber % 4 == 0);    // Every 4th beat
    }
}

void MusicMode::onOnsetDetected(uint32_t timestampMs, bool isLowBand) {
    // Calculate and store inter-onset interval (if we have a previous onset)
    if (lastOnsetTime_ != 0) {
        uint32_t interval = timestampMs - lastOnsetTime_;

        // Only store intervals in valid BPM range (300-1000ms = 200-60 BPM)
        // This validation also ensures safe narrowing to uint16_t (max value = 1000)
        if (interval >= 300 && interval <= 1000) {
            // Safe cast: interval is validated to be <= 1000, well within uint16_t range (0-65535)
            onsetIntervals_[intervalIndex_] = (uint16_t)interval;
            intervalIndex_ = (intervalIndex_ + 1) % MAX_INTERVALS;
            if (intervalCount_ < MAX_INTERVALS) intervalCount_++;
        }
    }

    // Calculate phase error (expected: onset near phase 0.0 or 1.0)
    float error = phase;
    if (error > 0.5f) error -= 1.0f;  // Wrap to -0.5 to 0.5 range

    // PLL correction (Proportional-Integral controller)
    errorIntegral_ += error;
    // Anti-windup: clamp integral to prevent excessive accumulation
    errorIntegral_ = clampFloat(errorIntegral_, -10.0f, 10.0f);
    float correction = pllKp * error + pllKi * errorIntegral_;

    // Adjust beat period based on correction
    beatPeriodMs_ *= (1.0f - correction);
    bpm = 60000.0f / beatPeriodMs_;

    // Clamp BPM to valid range
    bpm = clampFloat(bpm, bpmMin, bpmMax);
    beatPeriodMs_ = 60000.0f / bpm;

    // Update confidence based on phase error
    float absError = absFloat(error);
    if (absError < 0.2f) {  // Onset within 20% of expected position
        stableBeats_++;
        missedBeats_ = 0;
        confidence_ += 0.1f;
        if (confidence_ > 1.0f) confidence_ = 1.0f;
    } else {
        missedBeats_++;
        confidence_ -= 0.1f;
        if (confidence_ < 0.0f) confidence_ = 0.0f;
    }

    // Periodically estimate tempo (every 8 intervals to reduce CPU)
    if (intervalCount_ > 0 && intervalCount_ % 8 == 0) {
        estimateTempo();
    }

    lastOnsetTime_ = timestampMs;
}

void MusicMode::estimateTempo() {
    // Need at least 3 intervals for meaningful analysis
    if (intervalCount_ < 3) return;

    // Simple histogram-based autocorrelation
    // Count inter-onset intervals (IOI) falling into BPM bins
    uint16_t histogram[40] = {0};  // 40 bins covering 60-200 BPM

    // Iterate through stored intervals directly (already filtered to 300-1000ms range)
    for (uint8_t i = 0; i < intervalCount_; i++) {
        uint16_t ioi = onsetIntervals_[i];

        // Convert IOI to BPM bin (60-200 BPM range, 20ms bin width)
        // 60 BPM = 1000ms, 200 BPM = 300ms
        // Intervals are pre-filtered to valid range in onOnsetDetected()
        uint8_t bin = (ioi - 300) / 20;  // 20ms bins
        if (bin < 40) {
            histogram[bin]++;
        }
    }

    // Find peak in histogram
    uint8_t peakBin = 0;
    uint16_t peakValue = 0;
    for (uint8_t i = 0; i < 40; i++) {
        if (histogram[i] > peakValue) {
            peakValue = histogram[i];
            peakBin = i;
        }
    }

    // Convert bin back to BPM and update if confident
    if (peakValue >= 3) {  // Need at least 3 matching intervals
        uint32_t ioi = 300 + (peakBin * 20);
        float newBPM = 60000.0f / ioi;

        // Clamp new BPM to valid range before mixing
        newBPM = clampFloat(newBPM, bpmMin, bpmMax);

        // Smooth update (80% old, 20% new)
        bpm = bpm * 0.8f + newBPM * 0.2f;
        beatPeriodMs_ = 60000.0f / bpm;

        // Boost confidence when tempo estimation succeeds
        confidence_ += 0.2f;
        if (confidence_ > 1.0f) confidence_ = 1.0f;
    }
}

bool MusicMode::shouldActivate() const {
    return (confidence_ >= activationThreshold &&
            stableBeats_ >= minBeatsToActivate);
}

bool MusicMode::shouldDeactivate() const {
    return (confidence_ < activationThreshold * 0.5f ||
            missedBeats_ >= maxMissedBeats);
}
