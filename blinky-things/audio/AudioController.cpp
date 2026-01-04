#include "AudioController.h"
#include "../inputs/SerialConsole.h"
#include <math.h>

// ============================================================================
// Named Constants for rhythm-based pulse modulation
// ============================================================================

// Beat proximity thresholds for pulse modulation
// When phase is near 0 or 1, we're "on beat"; near 0.5 we're "off beat"
// distFromBeat ranges from 0 (on beat) to 0.5 (off beat)
static constexpr float PULSE_NEAR_BEAT_THRESHOLD = 0.2f;   // Below this = boost transients
static constexpr float PULSE_FAR_FROM_BEAT_THRESHOLD = 0.3f;  // Above this = suppress transients
// Transition zone width (derived): 0.3 - 0.2 = 0.1

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

    // Reset OSS buffer
    for (int i = 0; i < OSS_BUFFER_SIZE; i++) {
        ossBuffer_[i] = 0.0f;
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

    // Reset level tracking
    prevLevel_ = 0.0f;

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

    // 4. Get onset strength for rhythm analysis
    //    Use spectral flux from ensemble when available, fallback to level derivative
    float onsetStrength = 0.0f;

    // Get spectral flux from ensemble's SpectralFluxDetector
    const IDetector* fluxDetector = ensemble_.getDetector(DetectorType::SPECTRAL_FLUX);
    if (fluxDetector && fluxDetector->getLastRawValue() > 0.0f) {
        onsetStrength = fluxDetector->getLastRawValue();
    } else {
        // Fallback: use level derivative as onset strength
        float level = mic_.getLevel();
        onsetStrength = (level > prevLevel_) ? (level - prevLevel_) * 5.0f : 0.0f;
        prevLevel_ = level;
    }

    // Track when we last had significant audio
    if (onsetStrength > 0.1f || mic_.getLevel() > 0.1f) {
        lastSignificantAudioMs_ = nowMs;
    }

    // 3. Add sample to onset strength buffer
    addOssSample(onsetStrength);

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

void AudioController::addOssSample(float onsetStrength) {
    ossBuffer_[ossWriteIdx_] = onsetStrength;
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

    // Convert BPM range to lag range (in frames at ~60 Hz)
    // Formula: lag = 60 / bpm * frameRate
    // At 60 Hz: 200 BPM = 18 frames, 60 BPM = 60 frames
    // NOTE: This assumes consistent 60 Hz frame rate. See header for implications.
    constexpr float ASSUMED_FRAME_RATE = 60.0f;
    int minLag = static_cast<int>(60.0f / bpmMax * ASSUMED_FRAME_RATE);
    int maxLag = static_cast<int>(60.0f / bpmMin * ASSUMED_FRAME_RATE);

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

    // Autocorrelation: find the lag with maximum correlation
    float maxCorrelation = 0.0f;
    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag; lag++) {
        float correlation = 0.0f;
        int count = ossCount_ - lag;

        for (int i = 0; i < count; i++) {
            int idx1 = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            int idx2 = (ossWriteIdx_ - 1 - i - lag + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            correlation += ossBuffer_[idx1] * ossBuffer_[idx2];
        }

        correlation /= static_cast<float>(count);

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

    // DEBUG: Print correlation results (only when debug enabled)
    if (shouldPrintDebug) {
        float detectedBpm = (bestLag > 0) ? 60.0f / (static_cast<float>(bestLag) / 60.0f) : 0.0f;
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
    // Advance phase based on current tempo estimate
    float phaseIncrement = dt * 1000.0f / beatPeriodMs_;
    phase_ += phaseIncrement;

    // Safety check: if phase becomes NaN or infinite, reset to 0
    if (!isfinite(phase_)) {
        phase_ = 0.0f;
    }

    // Wrap phase at 1.0 using fmodf (safe for large jumps, prevents infinite loops)
    phase_ = fmodf(phase_, 1.0f);
    if (phase_ < 0.0f) phase_ += 1.0f;

    // Gradually adapt phase toward target (derived from autocorrelation)
    if (periodicityStrength_ > activationThreshold) {
        float phaseDiff = targetPhase_ - phase_;

        // Handle wraparound
        if (phaseDiff > 0.5f) phaseDiff -= 1.0f;
        if (phaseDiff < -0.5f) phaseDiff += 1.0f;

        // Apply gradual correction
        phase_ += phaseDiff * phaseAdaptRate * dt * 10.0f;

        // Re-wrap after correction (fmodf is safe for any phase value)
        phase_ = fmodf(phase_, 1.0f);
        if (phase_ < 0.0f) phase_ += 1.0f;
    }

    // Decay periodicity during silence
    uint32_t silenceMs = nowMs - lastSignificantAudioMs_;
    if (silenceMs > 3000) {
        periodicityStrength_ *= 0.995f;
    }
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
        if (distFromBeat < PULSE_NEAR_BEAT_THRESHOLD) {
            // Near beat: boost transient
            modulation = pulseBoostOnBeat;
        } else if (distFromBeat > PULSE_FAR_FROM_BEAT_THRESHOLD) {
            // Away from beat: suppress transient
            modulation = pulseSuppressOffBeat;
        } else {
            // Transition zone: interpolate between boost and suppress
            float transitionWidth = PULSE_FAR_FROM_BEAT_THRESHOLD - PULSE_NEAR_BEAT_THRESHOLD;
            float t = (distFromBeat - PULSE_NEAR_BEAT_THRESHOLD) / transitionWidth;
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
