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

    // Clear comb filter state
    for (int i = 0; i < NUM_TEMPO_FILTERS; i++) {
        tempoEnergy_[i] = 0.0f;
    }
    for (int i = 0; i < COMB_DELAY_SIZE; i++) {
        combDelayLine_[i] = 0.0f;
    }
    combDelayIdx_ = 0;
    lastOnsetStrength_ = 0.0f;

    // BPM lock state
    bpmLocked_ = false;
}

void MusicMode::update(float dt) {
    // Clear one-shot events at start of frame
    beatHappened = false;
    quarterNote = false;
    halfNote = false;
    wholeNote = false;

    // Update BPM lock state with hysteresis
    // Lock when confidence rises above threshold, unlock when it falls below unlock threshold
    if (!bpmLocked_ && confidence_ >= bpmLockThreshold) {
        bpmLocked_ = true;
    } else if (bpmLocked_ && confidence_ < bpmUnlockThreshold) {
        bpmLocked_ = false;
    }

    // Update phase (may trigger beat events)
    updatePhase(dt);

    // Update comb filter tempo estimation every frame
    // This provides continuous tempo tracking even between onsets
    updateTempoFilters(lastOnsetStrength_, dt);
    lastOnsetStrength_ = 0.0f;  // Reset after use

    // Check for missed beats (no onsets for too long)
    // Only check once per beat period to avoid incrementing missedBeats_ every frame
    uint32_t nowMs = time_.millis();
    uint32_t timeSinceOnset = nowMs - lastOnsetTime_;
    uint32_t timeSinceCheck = nowMs - lastMissedBeatCheck_;

    // Only check at beat intervals when active
    if (active && timeSinceCheck > beatPeriodMs_) {
        if (timeSinceOnset > beatPeriodMs_ * 1.5f) {  // 1.5x tolerance
            missedBeats_++;
            confidence_ -= missedBeatPenalty;
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
    // Store onset strength for comb filter processing in next update()
    // Low band onsets (bass) get higher weight for tempo tracking
    lastOnsetStrength_ = isLowBand ? 1.0f : 0.7f;

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
    float absError = absFloat(error);

    // ADAPTIVE PLL GAINS
    // High confidence = tight tracking (lower gains), Low confidence = fast acquisition (higher gains)
    // This allows quick lock-on when uncertain, and stable tracking when locked
    float adaptiveFactor = 2.0f - confidence_;  // Range: 1.0 (high conf) to 2.0 (low conf)
    float adaptiveKp = pllKp * adaptiveFactor;
    float adaptiveKi = pllKi * adaptiveFactor;

    // Phase jump on large error with low confidence
    // If we're way off and not confident, snap to onset rather than slowly correct
    if (absError > phaseSnapThreshold && confidence_ < phaseSnapConfidence) {
        phase = 0.0f;  // Snap phase to onset
        errorIntegral_ = 0.0f;  // Clear integral windup
    } else {
        // Normal PLL correction (Proportional-Integral controller)
        errorIntegral_ += error;
        // Anti-windup: clamp integral to prevent excessive accumulation
        errorIntegral_ = clampFloat(errorIntegral_, -10.0f, 10.0f);
        float correction = adaptiveKp * error + adaptiveKi * errorIntegral_;

        // Adjust beat period based on correction
        beatPeriodMs_ *= (1.0f - correction);
        bpm = 60000.0f / beatPeriodMs_;

        // Clamp BPM to valid range
        bpm = clampFloat(bpm, bpmMin, bpmMax);
        beatPeriodMs_ = 60000.0f / bpm;
    }

    // Update confidence based on phase error
    if (absError < stablePhaseThreshold) {  // Onset within threshold of expected position
        stableBeats_++;
        missedBeats_ = 0;
        confidence_ += confidenceIncrement;
        if (confidence_ > 1.0f) confidence_ = 1.0f;
    } else {
        missedBeats_++;
        confidence_ -= confidenceDecrement;
        if (confidence_ < 0.0f) confidence_ = 0.0f;
    }

    // Periodically estimate tempo using histogram (backup method, every 8 intervals)
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

        // OCTAVE DETECTION: When BPM < 100, check if we're detecting half-time
        // This happens when hi-hats or other subdivisions cause the histogram
        // to lock onto half-note intervals (kick-to-kick) instead of quarter notes
        if (newBPM < 100.0f && newBPM >= 50.0f) {
            // Check if there are also hits at half the interval (double BPM)
            // Half interval would be in a different bin
            uint16_t halfIoi = ioi / 2;  // e.g., 1000ms -> 500ms
            if (halfIoi >= 300) {  // Only if half-interval is in valid range
                uint8_t halfBin = (halfIoi - 300) / 20;
                if (halfBin < 40) {
                    uint16_t halfBinValue = histogram[halfBin];
                    // Also check adjacent bins (timing jitter)
                    if (halfBin > 0) halfBinValue += histogram[halfBin - 1];
                    if (halfBin < 39) halfBinValue += histogram[halfBin + 1];

                    // If we have significant evidence at double-tempo, use that instead
                    // Threshold: at least 2 hits at half-interval, or half the peak value
                    if (halfBinValue >= 2 || halfBinValue >= peakValue / 2) {
                        newBPM = 60000.0f / halfIoi;  // Double the tempo
                    }
                }
            }
        }

        // Clamp new BPM to valid range before mixing
        newBPM = clampFloat(newBPM, bpmMin, bpmMax);

        // Smooth update using histogramBlend parameter
        bpm = bpm * (1.0f - histogramBlend) + newBPM * histogramBlend;
        beatPeriodMs_ = 60000.0f / bpm;

        // Boost confidence when tempo estimation succeeds
        confidence_ += confidenceIncrement * 2.0f;  // Double increment for histogram match
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

/**
 * COMB FILTER RESONATOR BANK - Continuous tempo estimation
 *
 * Each comb filter resonates at a specific tempo (60-200 BPM).
 * When onsets occur at regular intervals matching a filter's period,
 * that filter accumulates energy (resonance). The filter with highest
 * energy indicates the dominant tempo.
 *
 * Advantages over histogram:
 * - Continuous tracking (every frame, not just every 8 onsets)
 * - Better octave handling (filters at related tempos compete)
 * - Faster convergence (resonance builds quickly)
 * - Handles tempo drift naturally
 *
 * Reference: Scheirer, "Tempo and Beat Analysis of Acoustic Musical Signals"
 */
void MusicMode::updateTempoFilters(float onsetStrength, float dt) {
    // Store current onset in delay line
    combDelayLine_[combDelayIdx_] = onsetStrength;

    // Update each tempo hypothesis
    for (int i = 0; i < NUM_TEMPO_FILTERS; i++) {
        float targetBpm = filterIndexToBPM(i);
        float periodFrames = bpmToFramePeriod(targetBpm);
        int period = (int)(periodFrames + 0.5f);  // Round to nearest frame

        // Clamp period to valid delay line range
        if (period < 1) period = 1;
        if (period >= COMB_DELAY_SIZE) period = COMB_DELAY_SIZE - 1;

        // Get onset from 1 beat ago (comb filter feedback)
        int delayIdx = (combDelayIdx_ - period + COMB_DELAY_SIZE) % COMB_DELAY_SIZE;
        float delayedOnset = combDelayLine_[delayIdx];

        // Comb filter resonance: current + weighted delayed (reinforces periodic signals)
        float resonance = onsetStrength + combFeedback * delayedOnset;

        // Exponential decay of accumulated energy with new resonance added
        tempoEnergy_[i] = tempoFilterDecay * tempoEnergy_[i] + (1.0f - tempoFilterDecay) * resonance;
    }

    // Advance delay line index
    combDelayIdx_ = (combDelayIdx_ + 1) % COMB_DELAY_SIZE;

    // Find peak tempo hypothesis
    int peakIdx = 0;
    float peakEnergy = 0.0f;
    for (int i = 0; i < NUM_TEMPO_FILTERS; i++) {
        if (tempoEnergy_[i] > peakEnergy) {
            peakEnergy = tempoEnergy_[i];
            peakIdx = i;
        }
    }

    // Store peak energy for debugging
    peakTempoEnergy_ = peakEnergy;

    // Update BPM if we have significant energy (avoid updating on noise)
    // Threshold scales with overall energy to adapt to different volume levels
    float energySum = 0.0f;
    for (int i = 0; i < NUM_TEMPO_FILTERS; i++) {
        energySum += tempoEnergy_[i];
    }
    float avgEnergy = energySum / NUM_TEMPO_FILTERS;

    // Only update BPM from comb filters when confidence is LOW (acquisition phase)
    // When confidence is HIGH, the PLL in onOnsetDetected() is the primary tempo tracker
    // This prevents competing updates that cause BPM jitter
    if (confidence_ < combConfidenceThreshold && peakEnergy > avgEnergy * 1.5f && peakEnergy > 0.02f) {
        float newBPM = filterIndexToBPM(peakIdx);

        // Apply BPM locking rate limit when locked
        if (bpmLocked_) {
            float maxDelta = bpmLockMaxChange * dt;  // Max change this frame
            float delta = newBPM - bpm;
            if (absFloat(delta) > maxDelta) {
                // Rate limit: move toward new BPM at max rate
                newBPM = bpm + (delta > 0 ? maxDelta : -maxDelta);
            }
        }

        // Blend based on inverse confidence: lower confidence = more comb filter influence
        // At confidence=0: 10% new BPM, at confidence=0.5: 5% new BPM
        float blendFactor = 0.05f + 0.05f * (1.0f - confidence_ * 2.0f);
        bpm = bpm * (1.0f - blendFactor) + newBPM * blendFactor;
        bpm = clampFloat(bpm, bpmMin, bpmMax);
        beatPeriodMs_ = 60000.0f / bpm;
    }
}
