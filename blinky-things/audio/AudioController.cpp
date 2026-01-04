#include "AudioController.h"
#include "../inputs/SerialConsole.h"
#include <math.h>

// ============================================================================
// Named Constants for rhythm-based pulse modulation
// ============================================================================

// Beat proximity thresholds for pulse modulation
// Beat proximity thresholds moved to AudioController class as tunable parameters
// See pulseNearBeatThreshold and pulseFarFromBeatThreshold in AudioController.h

// ===== CONSTRUCTION =====

AudioController::AudioController(IPdmMic& pdm, ISystemTime& time)
    : time_(time)
    , mic_(pdm, time)
{
}

AudioController::~AudioController() {
    end();
}

// ===== LIFECYCLE =====

bool AudioController::begin(uint32_t sampleRate) {
    if (!mic_.begin(sampleRate)) {
        return false;
    }

    // Initialize ensemble detector
    ensemble_.begin();

    // Reset OSS buffer and timestamps
    for (int i = 0; i < OSS_BUFFER_SIZE; i++) {
        ossBuffer_[i] = 0.0f;
        ossTimestamps_[i] = 0;
    }
    ossWriteIdx_ = 0;
    ossCount_ = 0;

    // Reset tempo estimation
    bpm_ = 120.0f;
    beatPeriodMs_ = 500.0f;
    periodicityStrength_ = 0.0f;

    // Reset phase tracking
    phase_ = 0.0f;
    targetPhase_ = 0.0f;

    // Reset timing
    lastAutocorrMs_ = 0;
    lastSignificantAudioMs_ = 0;

    // Reset output
    control_ = AudioControl();
    lastEnsembleOutput_ = EnsembleOutput();

    return true;
}

void AudioController::end() {
    mic_.end();
}

// ===== MAIN UPDATE =====

const AudioControl& AudioController::update(float dt) {
    uint32_t nowMs = time_.millis();

    // 1. Update microphone (level normalization, gain control)
    mic_.update(dt);

    // 2. Feed samples to ensemble detector from mic's ring buffer
    //    This provides samples for all spectral detectors (FFT-based)
    static int16_t sampleBuffer[256];  // Matches FFT_SIZE
    int samplesRead = mic_.getSamplesForExternal(sampleBuffer, 256);
    if (samplesRead > 0) {
        ensemble_.addSamples(sampleBuffer, samplesRead);
    }

    // 3. Run ensemble detector with current audio frame data
    //    The ensemble uses level for time-domain detectors (drummer)
    //    and spectral data when available
    lastEnsembleOutput_ = ensemble_.update(
        mic_.getLevel(),
        mic_.getRawLevel(),
        nowMs,
        dt
    );

    // 4. Get onset strength for rhythm analysis using multi-band RMS energy
    //    This is INDEPENDENT of transient detection - analyzes energy patterns
    float onsetStrength = 0.0f;

    // Get spectral data from ensemble
    const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();

    if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
        const float* magnitudes = spectral.getMagnitudes();
        int numBins = spectral.getNumBins();

        // Multi-band RMS energy calculation
        // Sample rate: 16kHz, FFT size: 256, bin resolution: 62.5 Hz/bin
        // Bass: bins 1-10 (62.5Hz-625Hz), Mid: bins 11-40 (687.5Hz-2.5kHz), High: bins 41-127 (2.56kHz-7.94kHz)
        float bassEnergy = 0.0f;
        float midEnergy = 0.0f;
        float highEnergy = 0.0f;
        int bassBinCount = 0, midBinCount = 0, highBinCount = 0;

        for (int i = 1; i < 11 && i < numBins; i++) {
            bassEnergy += magnitudes[i] * magnitudes[i];
            bassBinCount++;
        }
        for (int i = 11; i < 41 && i < numBins; i++) {
            midEnergy += magnitudes[i] * magnitudes[i];
            midBinCount++;
        }
        for (int i = 41; i < numBins; i++) {  // Extended to Nyquist for complete spectral coverage
            highEnergy += magnitudes[i] * magnitudes[i];
            highBinCount++;
        }

        // RMS (root mean square) - use actual bin count for accurate normalization
        bassEnergy = bassBinCount > 0 ? sqrtf(bassEnergy / (float)bassBinCount) : 0.0f;
        midEnergy = midBinCount > 0 ? sqrtf(midEnergy / (float)midBinCount) : 0.0f;
        highEnergy = highBinCount > 0 ? sqrtf(highEnergy / (float)highBinCount) : 0.0f;

        // Weighted sum: emphasize bass and mid for rhythm (where most beats occur)
        onsetStrength = 0.5f * bassEnergy + 0.3f * midEnergy + 0.2f * highEnergy;
    } else {
        // Fallback when no spectral data: use normalized level
        onsetStrength = mic_.getLevel();
    }

    // Track when we last had significant audio
    if (onsetStrength > 0.1f || mic_.getLevel() > 0.1f) {
        lastSignificantAudioMs_ = nowMs;
    }

    // 3. Add sample to onset strength buffer with timestamp
    addOssSample(onsetStrength, nowMs);

    // 4. Run autocorrelation periodically (every 500ms)
    if (nowMs - lastAutocorrMs_ >= AUTOCORR_PERIOD_MS) {
        runAutocorrelation(nowMs);
        lastAutocorrMs_ = nowMs;
    }

    // 5. Update phase tracking
    updatePhase(dt, nowMs);

    // 6. Synthesize output
    synthesizeEnergy();
    synthesizePulse();
    synthesizePhase();
    synthesizeRhythmStrength();

    return control_;
}

// ===== CONFIGURATION =====

void AudioController::setDetectorEnabled(DetectorType type, bool enabled) {
    ensemble_.setDetectorEnabled(type, enabled);
}

void AudioController::setDetectorWeight(DetectorType type, float weight) {
    ensemble_.setDetectorWeight(type, weight);
}

void AudioController::setDetectorThreshold(DetectorType type, float threshold) {
    ensemble_.setDetectorThreshold(type, threshold);
}

void AudioController::setBpmRange(float minBpm, float maxBpm) {
    bpmMin = clampf(minBpm, 30.0f, 120.0f);
    bpmMax = clampf(maxBpm, 80.0f, 300.0f);

    if (bpmMin >= bpmMax) {
        bpmMin = 60.0f;
        bpmMax = 200.0f;
    }
}

void AudioController::lockHwGain(int gain) {
    mic_.lockHwGain(gain);
}

void AudioController::unlockHwGain() {
    mic_.unlockHwGain();
}

bool AudioController::isHwGainLocked() const {
    return mic_.isHwGainLocked();
}

int AudioController::getHwGain() const {
    return mic_.getHwGain();
}

// ===== RHYTHM TRACKING =====

void AudioController::addOssSample(float onsetStrength, uint32_t timestampMs) {
    ossBuffer_[ossWriteIdx_] = onsetStrength;
    ossTimestamps_[ossWriteIdx_] = timestampMs;
    ossWriteIdx_ = (ossWriteIdx_ + 1) % OSS_BUFFER_SIZE;
    if (ossCount_ < OSS_BUFFER_SIZE) {
        ossCount_++;
    }
}

void AudioController::runAutocorrelation(uint32_t nowMs) {
    // Need at least 3 seconds of data for reliable tempo estimation
    if (ossCount_ < 180) {
        return;
    }

    // Convert BPM range to time-based lag range using actual timestamps
    // Formula: lagMs = 60000 / bpm (milliseconds per beat)
    // 200 BPM = 300ms, 60 BPM = 1000ms
    float minLagMs = 60000.0f / bpmMax;  // Minimum period in milliseconds
    float maxLagMs = 60000.0f / bpmMin;  // Maximum period in milliseconds

    // Convert to sample indices using actual elapsed time in buffer
    int mostRecentIdx = (ossWriteIdx_ - 1 + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    int oldestIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;

    // Handle potential timestamp wraparound (after ~49 days)
    int32_t bufferDurationMs = (int32_t)(ossTimestamps_[mostRecentIdx] - ossTimestamps_[oldestIdx]);
    if (bufferDurationMs < 0 || bufferDurationMs > 10000) {
        // Wraparound detected or invalid duration - use expected value for full buffer
        bufferDurationMs = 6000;  // 6 seconds @ 60 Hz
    }

    // Estimate samples per millisecond from buffer
    // Fallback should never be needed (ossCount_ >= 180 ensures bufferDurationMs > 0)
    // but use defensive 60 Hz assumption if it somehow occurs
    float samplesPerMs = bufferDurationMs > 0 ? (float)ossCount_ / (float)bufferDurationMs : 0.06f;

    int minLag = static_cast<int>(minLagMs * samplesPerMs);
    int maxLag = static_cast<int>(maxLagMs * samplesPerMs);

    if (minLag < 10) minLag = 10;
    if (maxLag > ossCount_ / 2) maxLag = ossCount_ / 2;
    if (minLag >= maxLag) return;

    // Compute signal energy for normalization
    float signalEnergy = 0.0f;
    float maxOss = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        signalEnergy += ossBuffer_[idx] * ossBuffer_[idx];
        if (ossBuffer_[idx] > maxOss) maxOss = ossBuffer_[idx];
    }

    // DEBUG: Print autocorrelation diagnostics (only when debug enabled)
    static uint32_t lastDebugMs = 0;
    bool shouldPrintDebug = (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) &&
                            (nowMs - lastDebugMs > 2000);
    if (shouldPrintDebug) {
        lastDebugMs = nowMs;
        Serial.print(F("{\"type\":\"RHYTHM_DEBUG\",\"ossCount\":"));
        Serial.print(ossCount_);
        Serial.print(F(",\"sigEnergy\":"));
        Serial.print(signalEnergy, 4);
        Serial.print(F(",\"maxOss\":"));
        Serial.print(maxOss, 4);
        Serial.print(F(",\"strength\":"));
        Serial.print(periodicityStrength_, 3);
        Serial.println(F("}"));
    }

    if (signalEnergy < 0.001f) {
        // No signal - decay periodicity
        periodicityStrength_ *= 0.9f;
        return;
    }

    // Autocorrelation: compute correlation for all lags
    // We'll store correlations to find multiple peaks
    static float correlationAtLag[200];  // Max lag range (200 BPM @ 60Hz = 30 samples, 60 BPM @ 60Hz = 60 samples)
    int correlationSize = maxLag - minLag + 1;
    if (correlationSize > 200) correlationSize = 200;

    float maxCorrelation = 0.0f;
    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag && (lag - minLag) < 200; lag++) {
        float correlation = 0.0f;
        int count = ossCount_ - lag;

        for (int i = 0; i < count; i++) {
            int idx1 = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            int idx2 = (ossWriteIdx_ - 1 - i - lag + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            correlation += ossBuffer_[idx1] * ossBuffer_[idx2];
        }

        correlation /= static_cast<float>(count);
        correlationAtLag[lag - minLag] = correlation;

        if (correlation > maxCorrelation) {
            maxCorrelation = correlation;
            bestLag = lag;
        }
    }

    // Compute periodicity strength (normalized correlation)
    float avgEnergy = signalEnergy / static_cast<float>(ossCount_);
    float normCorrelation = maxCorrelation / (avgEnergy + 0.001f);

    // Smooth periodicity strength updates
    float newStrength = clampf(normCorrelation * 1.5f, 0.0f, 1.0f);
    periodicityStrength_ = periodicityStrength_ * 0.7f + newStrength * 0.3f;

    // === MULTI-PEAK EXTRACTION ===
    // Find up to 4 peaks in autocorrelation function for multi-hypothesis tracking
    AutocorrPeak peaks[4];
    int numPeaks = 0;

    // Find local maxima above threshold
    for (int lag = minLag; lag <= maxLag && (lag - minLag) < correlationSize; lag++) {
        int lagIdx = lag - minLag;
        float correlation = correlationAtLag[lagIdx];
        float normCorr = correlation / (avgEnergy + 0.001f);

        // Skip if below minimum strength threshold
        if (normCorr < multiHypothesis_.minPeakStrength) continue;

        // Check if below minimum relative height (must be >70% of max peak)
        if (normCorr < normCorrelation * multiHypothesis_.minRelativePeakHeight) continue;

        // Check if this is a local maximum (±2 neighbors must be lower or equal)
        bool isLocalMax = true;
        bool hasStrictlyLowerNeighbor = false;

        for (int delta = -2; delta <= 2; delta++) {
            if (delta == 0) continue;
            int neighborLag = lag + delta;
            if (neighborLag < minLag || neighborLag > maxLag) continue;
            int neighborIdx = neighborLag - minLag;
            // Defensive: check both bounds explicitly (neighborIdx should never be < 0 due to line 340, but be safe)
            if (neighborIdx < 0 || neighborIdx >= correlationSize) continue;

            float neighborCorr = correlationAtLag[neighborIdx];
            if (neighborCorr > correlation) {
                isLocalMax = false;
                break;
            }
            if (neighborCorr < correlation) {
                hasStrictlyLowerNeighbor = true;
            }
        }

        // Accept as peak if local max with at least one strictly lower neighbor
        if (isLocalMax && hasStrictlyLowerNeighbor && numPeaks < 4) {
            peaks[numPeaks].lag = lag;
            peaks[numPeaks].correlation = correlation;
            peaks[numPeaks].normCorrelation = normCorr;
            numPeaks++;
        }
    }

    // Sort peaks by strength (descending) - simple bubble sort
    for (int i = 0; i < numPeaks - 1; i++) {
        for (int j = i + 1; j < numPeaks; j++) {
            if (peaks[j].normCorrelation > peaks[i].normCorrelation) {
                AutocorrPeak temp = peaks[i];
                peaks[i] = peaks[j];
                peaks[j] = temp;
            }
        }
    }

    // === UPDATE MULTI-HYPOTHESIS TRACKER ===
    // Check if any hypotheses are active
    bool hasActiveHypothesis = false;
    for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
        if (multiHypothesis_.hypotheses[i].active) {
            hasActiveHypothesis = true;
            break;
        }
    }

    // For each peak, find matching hypothesis or create new one
    for (int i = 0; i < numPeaks; i++) {
        float bpm = 60000.0f / (static_cast<float>(peaks[i].lag) / samplesPerMs);
        bpm = clampf(bpm, bpmMin, bpmMax);

        // Find matching hypothesis
        int matchSlot = multiHypothesis_.findMatchingHypothesis(bpm);

        if (matchSlot >= 0) {
            // Update existing hypothesis
            TempoHypothesis& hypo = multiHypothesis_.hypotheses[matchSlot];
            hypo.strength = peaks[i].normCorrelation;
            hypo.lastUpdateMs = nowMs;
            hypo.correlationPeak = peaks[i].correlation;
            hypo.lagSamples = peaks[i].lag;
            hypo.bpm = bpm;  // Update BPM (may drift slightly)
            hypo.beatPeriodMs = 60000.0f / bpm;
        } else if (peaks[i].normCorrelation > multiHypothesis_.minPeakStrength ||
                   (!hasActiveHypothesis && i == 0)) {
            // Create new hypothesis if:
            // 1. Peak meets strength threshold, OR
            // 2. This is the first peak and NO hypotheses are active (ensures primary gets initialized)
            multiHypothesis_.createHypothesis(bpm, peaks[i].normCorrelation, nowMs,
                                              peaks[i].lag, peaks[i].correlation);
        }
    }

    // DEBUG: Print correlation results (only when debug enabled)
    if (shouldPrintDebug) {
        // Use adaptive timing formula (not 60 Hz assumption)
        float detectedBpm = (bestLag > 0) ? 60000.0f / (static_cast<float>(bestLag) / samplesPerMs) : 0.0f;
        Serial.print(F("{\"type\":\"RHYTHM_DEBUG2\",\"bestLag\":"));
        Serial.print(bestLag);
        Serial.print(F(",\"maxCorr\":"));
        Serial.print(maxCorrelation, 6);
        Serial.print(F(",\"normCorr\":"));
        Serial.print(normCorrelation, 4);
        Serial.print(F(",\"newStr\":"));
        Serial.print(newStrength, 3);
        Serial.print(F(",\"bpm\":"));
        Serial.print(detectedBpm, 1);
        Serial.println(F("}"));
    }

    // Update tempo if periodicity is strong enough
    // Safety: bestLag > 0 check prevents division by zero in calculations below
    if (bestLag > 0 && periodicityStrength_ > 0.25f) {
        float newBpm = 60.0f / (static_cast<float>(bestLag) / 60.0f);
        newBpm = clampf(newBpm, bpmMin, bpmMax);

        // Smooth BPM changes
        bpm_ = bpm_ * 0.8f + newBpm * 0.2f;
        beatPeriodMs_ = 60000.0f / bpm_;

        // Derive target phase from autocorrelation
        // Find where we are in the current beat cycle by looking at recent samples
        // The position of maximum correlation in the recent window indicates phase
        int recentWindow = bestLag;  // Look at one beat period
        float maxRecent = 0.0f;
        int maxRecentIdx = 0;

        for (int i = 0; i < recentWindow && i < ossCount_; i++) {
            int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            if (ossBuffer_[idx] > maxRecent) {
                maxRecent = ossBuffer_[idx];
                maxRecentIdx = i;
            }
        }

        // Convert to phase (0 = just had a beat, approaching 1 = beat coming)
        if (maxRecent > 0.05f) {
            targetPhase_ = static_cast<float>(maxRecentIdx) / static_cast<float>(bestLag);
            targetPhase_ = clampf(targetPhase_, 0.0f, 1.0f);
        }
    }
}

void AudioController::updatePhase(float dt, uint32_t nowMs) {
    // Determine if we have significant audio (for hypothesis decay strategy)
    int32_t silenceMs = static_cast<int32_t>(nowMs - lastSignificantAudioMs_);
    if (silenceMs < 0) silenceMs = 0;  // Handle wraparound
    bool hasSignificantAudio = (silenceMs < 1000);  // Less than 1 second of silence

    // === UPDATE ALL ACTIVE HYPOTHESES ===
    for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
        if (multiHypothesis_.hypotheses[i].active) {
            multiHypothesis_.updateHypothesis(i, dt, nowMs, hasSignificantAudio);
        }
    }

    // === PROMOTE BEST HYPOTHESIS IF NEEDED ===
    multiHypothesis_.promoteBestHypothesis(nowMs);

    // === USE PRIMARY HYPOTHESIS FOR OUTPUT ===
    TempoHypothesis& primary = multiHypothesis_.getPrimary();

    if (primary.active && primary.strength > 0.25f) {
        // Safety check for NaN/infinity in primary hypothesis
        if (!isfinite(primary.phase)) {
            primary.phase = 0.0f;
        }
        if (!isfinite(primary.bpm) || primary.bpm < 1.0f) {
            primary.bpm = 120.0f;  // Safe default
        }

        // Use primary hypothesis values
        phase_ = primary.phase;
        bpm_ = primary.bpm;
        periodicityStrength_ = primary.strength;

        // Gradually adapt phase toward target (derived from autocorrelation)
        // This provides additional smoothing on top of hypothesis tracking
        if (periodicityStrength_ > activationThreshold) {
            float phaseDiff = targetPhase_ - phase_;

            // Handle wraparound
            if (phaseDiff > 0.5f) phaseDiff -= 1.0f;
            if (phaseDiff < -0.5f) phaseDiff += 1.0f;

            // Apply gradual correction
            phase_ += phaseDiff * phaseAdaptRate * dt * 10.0f;

            // Re-wrap after correction
            phase_ = fmodf(phase_, 1.0f);
            if (phase_ < 0.0f) phase_ += 1.0f;

            // Update primary hypothesis phase with corrected value
            primary.phase = phase_;
        }
    } else {
        // No active primary hypothesis - fall back to legacy behavior
        // Advance phase based on current tempo estimate
        float phaseIncrement = dt * 1000.0f / beatPeriodMs_;
        phase_ += phaseIncrement;

        // Safety check: if phase becomes NaN or infinite, reset to 0
        if (!isfinite(phase_)) {
            phase_ = 0.0f;
        }

        // Wrap phase at 1.0 using fmodf
        phase_ = fmodf(phase_, 1.0f);
        if (phase_ < 0.0f) phase_ += 1.0f;

        // Decay periodicity during silence
        if (silenceMs > 3000) {
            float decayFactor = expf(-0.138629f * dt);
            periodicityStrength_ *= decayFactor;
        }
    }

    // === DEBUG OUTPUT ===
    multiHypothesis_.printDebug(nowMs);
}

// ===== OUTPUT SYNTHESIS =====

void AudioController::synthesizeEnergy() {
    float energy = mic_.getLevel();

    // Apply beat-aligned energy boost when rhythm is locked
    if (periodicityStrength_ > activationThreshold) {
        // Distance from beat: 0 at phase 0 or 1, max 0.5 at phase 0.5
        float distFromBeat = phase_ < 0.5f ? phase_ : (1.0f - phase_);
        // Convert to proximity: 1.0 at beat, 0.0 at off-beat
        float nearBeat = 1.0f - distFromBeat * 2.0f;

        // Boost near beats
        float beatBoost = nearBeat * energyBoostOnBeat * periodicityStrength_;
        energy *= (1.0f + beatBoost);
    }

    control_.energy = clampf(energy, 0.0f, 1.0f);
}

void AudioController::synthesizePulse() {
    // Use ensemble transient strength instead of single-detector output
    float pulse = lastEnsembleOutput_.transientStrength;

    // Apply beat-aligned modulation when rhythm is detected (visual effect only)
    if (pulse > 0.0f && periodicityStrength_ > activationThreshold) {
        float distFromBeat = phase_ < 0.5f ? phase_ : (1.0f - phase_);

        float modulation;
        if (distFromBeat < pulseNearBeatThreshold) {
            // Near beat: boost transient
            modulation = pulseBoostOnBeat;
        } else if (distFromBeat > pulseFarFromBeatThreshold) {
            // Away from beat: suppress transient
            modulation = pulseSuppressOffBeat;
        } else {
            // Transition zone: interpolate between boost and suppress
            float transitionWidth = pulseFarFromBeatThreshold - pulseNearBeatThreshold;
            float t = (distFromBeat - pulseNearBeatThreshold) / transitionWidth;
            modulation = pulseBoostOnBeat * (1.0f - t) + pulseSuppressOffBeat * t;
        }

        // Blend modulation based on periodicity strength
        pulse *= (1.0f - periodicityStrength_) + modulation * periodicityStrength_;
    }

    control_.pulse = clampf(pulse, 0.0f, 1.0f);
}

void AudioController::synthesizePhase() {
    control_.phase = phase_;
}

void AudioController::synthesizeRhythmStrength() {
    // Single metric: autocorrelation periodicity strength
    float strength = periodicityStrength_;

    // Apply activation threshold with soft knee
    if (strength < activationThreshold) {
        strength *= strength / activationThreshold;  // Quadratic falloff below threshold
    }

    control_.rhythmStrength = clampf(strength, 0.0f, 1.0f);
}

// ============================================================================
// TempoHypothesis Implementation
// ============================================================================

void TempoHypothesis::updatePhase(float dt) {
    float phaseIncrement = dt * 1000.0f / beatPeriodMs;
    float oldPhase = phase;
    phase += phaseIncrement;

    // Accumulate fractional beats
    beatsSinceUpdate += phaseIncrement;

    // Detect phase wraparound to increment beat count
    // This happens when phase crosses from <1.0 to >=1.0
    if (phase >= 1.0f && beatCount < 65535) {
        // Count how many complete beats occurred (usually 1, but could be >1 if dt is large)
        uint16_t beatsElapsed = static_cast<uint16_t>(phase);  // Integer part
        beatCount += beatsElapsed;
        if (beatCount > 65535) beatCount = 65535;  // Clamp to max
    }

    // Wrap phase to [0, 1)
    phase = fmodf(phase, 1.0f);
    if (phase < 0.0f) phase += 1.0f;
}

void TempoHypothesis::applyBeatDecay() {
    if (beatsSinceUpdate < 0.01f) return;  // No decay needed yet

    // Phrase-aware decay: half-life = 32 beats
    // decay_factor = exp(-ln(2) * beats / 32) = exp(-0.02166 * beats)
    float decayFactor = expf(-0.02166f * beatsSinceUpdate);
    strength *= decayFactor;

    // Reset accumulator
    beatsSinceUpdate = 0.0f;

    // Deactivate if too weak
    if (strength < 0.1f) {
        active = false;
    }
}

void TempoHypothesis::applySilenceDecay(float dt) {
    // Time-based decay: 5-second half-life
    // decay_factor = exp(-ln(2) * dt / 5.0) = exp(-0.138629 * dt)
    float decayFactor = expf(-0.138629f * dt);
    strength *= decayFactor;

    // Deactivate if too weak
    if (strength < 0.1f) {
        active = false;
    }
}

float TempoHypothesis::computeConfidence(float strengthWeight, float consistencyWeight, float longevityWeight) const {
    // Normalize beat count to [0, 1] over 32 beats (8 bars)
    float normalizedLongevity = (beatCount > 32) ? 1.0f : static_cast<float>(beatCount) / 32.0f;

    // Phase consistency (1.0 = perfect, 0.0 = terrible)
    // avgPhaseError is in range [0, 1], where 0 = perfect
    float phaseConsistency = 1.0f - avgPhaseError;
    if (phaseConsistency < 0.0f) phaseConsistency = 0.0f;
    if (phaseConsistency > 1.0f) phaseConsistency = 1.0f;

    // Weighted combination
    return strengthWeight * strength +
           consistencyWeight * phaseConsistency +
           longevityWeight * normalizedLongevity;
}

// ============================================================================
// MultiHypothesisTracker Implementation
// ============================================================================

int MultiHypothesisTracker::createHypothesis(float bpm, float strength, uint32_t nowMs, int lagSamples, float correlation) {
    int slot = findBestSlot();
    if (slot < 0) return -1;  // No slot available (shouldn't happen)

    // Store old hypothesis info for debug
    bool wasActive = hypotheses[slot].active;
    float oldBpm = hypotheses[slot].bpm;

    TempoHypothesis& hypo = hypotheses[slot];
    hypo.bpm = bpm;
    hypo.beatPeriodMs = 60000.0f / bpm;
    hypo.phase = 0.0f;
    hypo.strength = strength;
    hypo.confidence = strength;  // Initial confidence = strength
    hypo.avgPhaseError = 0.5f;  // Neutral value - prevents over-confidence in new hypotheses
    hypo.lastUpdateMs = nowMs;
    hypo.createdMs = nowMs;
    hypo.beatCount = 0;
    hypo.beatsSinceUpdate = 0.0f;
    hypo.correlationPeak = correlation;
    hypo.lagSamples = lagSamples;
    hypo.active = true;
    hypo.priority = static_cast<uint8_t>(slot);

    // Debug output for creation/eviction
    if (debugLevel >= HypothesisDebugLevel::EVENTS) {
        if (wasActive) {
            Serial.print(F("{\"type\":\"HYPO_EVICT\",\"slot\":"));
            Serial.print(slot);
            Serial.print(F(",\"oldBpm\":"));
            Serial.print(oldBpm, 1);
            Serial.print(F(",\"newBpm\":"));
            Serial.print(bpm, 1);
            Serial.println(F("}"));
        }
        Serial.print(F("{\"type\":\"HYPO_CREATE\",\"slot\":"));
        Serial.print(slot);
        Serial.print(F(",\"bpm\":"));
        Serial.print(bpm, 1);
        Serial.print(F(",\"strength\":"));
        Serial.print(strength, 2);
        Serial.println(F("}"));
    }

    return slot;
}

int MultiHypothesisTracker::findBestSlot() {
    // First, check for inactive slots
    for (int i = 0; i < MAX_HYPOTHESES; i++) {
        if (!hypotheses[i].active) {
            return i;
        }
    }

    // All slots active - find LRU (least recently updated)
    int oldestSlot = 0;
    uint32_t oldestTime = hypotheses[0].lastUpdateMs;

    for (int i = 1; i < MAX_HYPOTHESES; i++) {
        if (hypotheses[i].lastUpdateMs < oldestTime) {
            oldestTime = hypotheses[i].lastUpdateMs;
            oldestSlot = i;
        }
    }

    // Don't evict primary if it's still strong (>0.5 strength)
    if (oldestSlot == 0 && hypotheses[0].strength > 0.5f) {
        // Evict tertiary (slot 2) instead
        return 2;
    }

    return oldestSlot;
}

int MultiHypothesisTracker::findMatchingHypothesis(float bpm) const {
    for (int i = 0; i < MAX_HYPOTHESES; i++) {
        if (!hypotheses[i].active) continue;

        // Guard against division by zero (hypotheses[i].bpm should never be 0, but be defensive)
        if (hypotheses[i].bpm < 1.0f) continue;

        float error = absf(bpm - hypotheses[i].bpm) / hypotheses[i].bpm;
        if (error < bpmMatchTolerance) {
            return i;
        }
    }
    return -1;  // No match
}

void MultiHypothesisTracker::updateWithPeaks(const AutocorrPeak* peaks, int numPeaks, uint32_t nowMs) {
    // For each peak, find matching hypothesis or create new one
    for (int i = 0; i < numPeaks; i++) {
        // Convert lag to BPM (this will be done by caller, peaks already have BPM)
        // For now, assume peaks contain lag and we compute BPM here
        // This will be refined in Phase 2 when we modify runAutocorrelation

        // Find matching hypothesis
        // TODO: This will be fully implemented in Phase 2
        // For now, this is a placeholder
    }
}

void MultiHypothesisTracker::updateHypothesis(int index, float dt, uint32_t nowMs, bool hasSignificantAudio) {
    if (index < 0 || index >= MAX_HYPOTHESES) return;
    if (!hypotheses[index].active) return;

    TempoHypothesis& hypo = hypotheses[index];

    // Always advance phase (for prediction)
    hypo.updatePhase(dt);

    // Update confidence
    hypo.confidence = hypo.computeConfidence(strengthWeight, consistencyWeight, longevityWeight);

    if (hasSignificantAudio) {
        // Active music: apply beat-count decay if enough beats accumulated
        if (hypo.beatsSinceUpdate > 1.0f) {
            hypo.applyBeatDecay();
        }
    } else {
        // Silence: apply time-based decay after grace period
        int32_t silenceMs = static_cast<int32_t>(nowMs - hypo.lastUpdateMs);
        if (silenceMs < 0) silenceMs = 0;  // Handle wraparound

        if (static_cast<uint32_t>(silenceMs) > silenceGracePeriodMs) {
            hypo.applySilenceDecay(dt);
        }
    }
}

void MultiHypothesisTracker::promoteBestHypothesis(uint32_t nowMs) {
    // Find best non-primary hypothesis
    int bestSlot = 0;
    float bestConfidence = hypotheses[0].confidence;

    for (int i = 1; i < MAX_HYPOTHESES; i++) {
        if (!hypotheses[i].active) continue;

        if (hypotheses[i].confidence > bestConfidence) {
            bestConfidence = hypotheses[i].confidence;
            bestSlot = i;
        }
    }

    // Promote if significantly better and has enough history
    if (bestSlot != 0 &&
        bestConfidence > hypotheses[0].confidence + promotionThreshold &&
        hypotheses[bestSlot].beatCount >= minBeatsBeforePromotion) {

        // Debug output
        if (debugLevel >= HypothesisDebugLevel::EVENTS) {
            Serial.print(F("{\"type\":\"HYPO_PROMOTE\",\"from\":"));
            Serial.print(bestSlot);
            Serial.print(F(",\"to\":0,\"bpm\":"));
            Serial.print(hypotheses[bestSlot].bpm, 1);
            Serial.print(F(",\"conf\":"));
            Serial.print(bestConfidence, 2);
            Serial.println(F("}"));
        }

        // Swap slots (not just pointers - actually swap data)
        TempoHypothesis temp = hypotheses[0];
        hypotheses[0] = hypotheses[bestSlot];
        hypotheses[bestSlot] = temp;

        // Update priority values
        hypotheses[0].priority = 0;
        hypotheses[bestSlot].priority = static_cast<uint8_t>(bestSlot);
    }
}

void MultiHypothesisTracker::printDebug(uint32_t nowMs, const char* eventType, int slotIndex) const {
    if (debugLevel == HypothesisDebugLevel::OFF) return;

    // EVENT-level messages are printed immediately by the calling functions
    // (createHypothesis, promoteBestHypothesis)

    // SUMMARY and DETAILED are rate-limited
    if (debugLevel >= HypothesisDebugLevel::SUMMARY) {
        // Rate limit to every 2 seconds
        if (nowMs - lastDebugPrintMs_ < 2000) return;
        const_cast<MultiHypothesisTracker*>(this)->lastDebugPrintMs_ = nowMs;

        if (debugLevel == HypothesisDebugLevel::SUMMARY) {
            // Print primary hypothesis only
            const TempoHypothesis& primary = hypotheses[0];
            if (primary.active) {
                Serial.print(F("{\"type\":\"HYPO_PRIMARY\",\"bpm\":"));
                Serial.print(primary.bpm, 1);
                Serial.print(F(",\"phase\":"));
                Serial.print(primary.phase, 2);
                Serial.print(F(",\"strength\":"));
                Serial.print(primary.strength, 2);
                Serial.print(F(",\"conf\":"));
                Serial.print(primary.confidence, 2);
                Serial.print(F(",\"beats\":"));
                Serial.print(primary.beatCount);
                Serial.println(F("}"));
            }
        } else if (debugLevel == HypothesisDebugLevel::DETAILED) {
            // Print all hypotheses
            Serial.print(F("{\"type\":\"HYPO_ALL\",\"h\":["));
            for (int i = 0; i < MAX_HYPOTHESES; i++) {
                if (i > 0) Serial.print(F(","));
                if (hypotheses[i].active) {
                    Serial.print(F("{\"s\":"));
                    Serial.print(i);
                    Serial.print(F(",\"bpm\":"));
                    Serial.print(hypotheses[i].bpm, 1);
                    Serial.print(F(",\"ph\":"));
                    Serial.print(hypotheses[i].phase, 2);
                    Serial.print(F(",\"str\":"));
                    Serial.print(hypotheses[i].strength, 2);
                    Serial.print(F(",\"conf\":"));
                    Serial.print(hypotheses[i].confidence, 2);
                    Serial.print(F(",\"b\":"));
                    Serial.print(hypotheses[i].beatCount);
                    Serial.print(F("}"));
                } else {
                    Serial.print(F("{\"s\":"));
                    Serial.print(i);
                    Serial.print(F(",\"inactive\":true}"));
                }
            }
            Serial.println(F("]}"));
        }
    }
}
