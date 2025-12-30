#include "AudioController.h"
#include <math.h>

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
    // Initialize microphone
    if (!mic_.begin(sampleRate)) {
        return false;
    }

    // Reset rhythm tracking state
    for (int i = 0; i < OSS_BUFFER_SIZE; i++) {
        ossBuffer_[i] = 0.0f;
    }
    ossWriteIdx_ = 0;
    ossCount_ = 0;

    bpm_ = 120.0f;
    beatPeriodMs_ = 500.0f;
    periodicityStrength_ = 0.0f;

    phase_ = 0.0f;
    errorIntegral_ = 0.0f;
    lastPhaseError_ = 0.0f;
    lastOnsetMs_ = 0;

    confidence_ = 0.0f;
    confidenceSmooth_ = 0.0f;

    lastAutocorrMs_ = 0;

    // Reset output
    control_ = AudioControl();

    return true;
}

void AudioController::end() {
    mic_.end();
}

// ===== MAIN UPDATE =====

const AudioControl& AudioController::update(float dt) {
    uint32_t nowMs = time_.millis();

    // 1. Update microphone (transient detection, level)
    mic_.update(dt);

    // 2. Get onset strength for rhythm analysis
    float onsetStrength = 0.0f;
    uint8_t mode = mic_.getDetectionMode();

    // Use spectral flux for rhythm analysis when available
    if (mode == static_cast<uint8_t>(DetectionMode::SPECTRAL_FLUX) ||
        mode == static_cast<uint8_t>(DetectionMode::HYBRID)) {
        onsetStrength = mic_.getLastFluxValue();
    } else {
        // Fall back to transient strength for other modes
        onsetStrength = mic_.getTransient();
    }

    // 3. Add sample to onset strength buffer
    addOssSample(onsetStrength);

    // 4. Run autocorrelation periodically
    if (nowMs - lastAutocorrMs_ >= AUTOCORR_PERIOD_MS) {
        runAutocorrelation(nowMs);
        lastAutocorrMs_ = nowMs;
    }

    // 5. Update phase tracking
    updatePhase(dt, nowMs);

    // 6. Handle transient events
    float transient = mic_.getTransient();
    if (transient > 0.0f) {
        onTransientDetected(nowMs, transient);
    }

    // 7. Synthesize output
    synthesizeEnergy();
    synthesizePulse();
    synthesizePhase();
    synthesizeRhythmStrength();

    return control_;
}

// ===== CONFIGURATION =====

void AudioController::setDetectionMode(uint8_t mode) {
    mic_.detectionMode = mode;
}

uint8_t AudioController::getDetectionMode() const {
    return mic_.getDetectionMode();
}

void AudioController::setBpmRange(float minBpm, float maxBpm) {
    bpmMin_ = clampf(minBpm, 30.0f, 120.0f);
    bpmMax_ = clampf(maxBpm, 80.0f, 300.0f);

    // Ensure min < max
    if (bpmMin_ >= bpmMax_) {
        bpmMin_ = 60.0f;
        bpmMax_ = 200.0f;
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
    // Need at least 2 seconds of data
    if (ossCount_ < 120) {  // 2 seconds at 60 Hz
        return;
    }

    // Convert BPM range to lag range (in frames at 60 Hz)
    // lag = 60 / bpm * frameRate
    // At 60 Hz: 200 BPM = 18 frames, 60 BPM = 60 frames
    int minLag = static_cast<int>(60.0f / bpmMax_ * 60.0f);
    int maxLag = static_cast<int>(60.0f / bpmMin_ * 60.0f);

    // Clamp lag range
    if (minLag < 10) minLag = 10;
    if (maxLag > ossCount_ / 2) maxLag = ossCount_ / 2;
    if (minLag >= maxLag) return;

    // Simple autocorrelation: R(lag) = sum(signal[i] * signal[i - lag])
    float maxCorrelation = 0.0f;
    int bestLag = 0;
    float signalEnergy = 0.0f;

    // Compute signal energy for normalization
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        signalEnergy += ossBuffer_[idx] * ossBuffer_[idx];
    }

    if (signalEnergy < 0.001f) {
        // No signal - decay confidence
        periodicityStrength_ *= 0.9f;
        return;
    }

    // Search for best lag
    for (int lag = minLag; lag <= maxLag; lag++) {
        float correlation = 0.0f;
        int count = ossCount_ - lag;

        for (int i = 0; i < count; i++) {
            int idx1 = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            int idx2 = (ossWriteIdx_ - 1 - i - lag + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            correlation += ossBuffer_[idx1] * ossBuffer_[idx2];
        }

        // Normalize by count
        correlation /= static_cast<float>(count);

        if (correlation > maxCorrelation) {
            maxCorrelation = correlation;
            bestLag = lag;
        }
    }

    // Compute periodicity strength (normalized correlation)
    float normCorrelation = maxCorrelation / (signalEnergy / static_cast<float>(ossCount_) + 0.001f);
    periodicityStrength_ = clampf(normCorrelation * 2.0f, 0.0f, 1.0f);  // Scale up for usable range

    // Convert lag to BPM (assuming 60 Hz frame rate)
    if (bestLag > 0 && periodicityStrength_ > 0.3f) {
        float newBpm = 60.0f / (static_cast<float>(bestLag) / 60.0f);
        newBpm = clampf(newBpm, bpmMin_, bpmMax_);

        // Smooth BPM changes
        float bpmBlend = 0.2f;  // Blend factor (higher = faster adaptation)
        bpm_ = bpm_ * (1.0f - bpmBlend) + newBpm * bpmBlend;
        beatPeriodMs_ = 60000.0f / bpm_;
    }
}

void AudioController::updatePhase(float dt, uint32_t nowMs) {
    // Advance phase based on current BPM
    float phaseIncrement = dt * 1000.0f / beatPeriodMs_;
    phase_ += phaseIncrement;

    // Wrap phase at 1.0
    while (phase_ >= 1.0f) {
        phase_ -= 1.0f;
    }

    // Decay confidence if no transients detected for a while
    uint32_t silenceMs = nowMs - lastOnsetMs_;
    if (silenceMs > 2000) {  // 2 seconds of silence
        confidence_ *= 0.995f;  // Slow decay
    }

    // Smooth confidence for output
    float smoothingFactor = 0.1f;
    confidenceSmooth_ = confidenceSmooth_ * (1.0f - smoothingFactor) + confidence_ * smoothingFactor;
}

void AudioController::onTransientDetected(uint32_t nowMs, float strength) {
    lastOnsetMs_ = nowMs;

    // Calculate phase error: transient should occur near phase 0 or 1
    // phase 0 = on beat, phase 0.5 = off beat
    float phaseError = phase_;
    if (phaseError > 0.5f) {
        phaseError = phaseError - 1.0f;  // Map 0.5-1.0 to -0.5-0.0
    }
    lastPhaseError_ = phaseError;

    // Only apply PLL correction if we have some rhythm confidence
    if (periodicityStrength_ > 0.3f) {
        // Adaptive PLL gains: more aggressive when confidence is low
        float adaptiveFactor = 2.0f - confidence_;
        float kp = pllKp * adaptiveFactor;
        float ki = pllKi * adaptiveFactor;

        // Phase snap for large errors when confidence is low
        if (absf(phaseError) > 0.3f && confidence_ < 0.4f) {
            // Snap to beat
            phase_ = 0.0f;
            errorIntegral_ = 0.0f;
        } else {
            // Gradual PLL correction
            errorIntegral_ += phaseError;
            errorIntegral_ = clampf(errorIntegral_, -5.0f, 5.0f);

            float correction = kp * phaseError + ki * errorIntegral_;
            beatPeriodMs_ *= (1.0f - correction * 0.1f);  // Subtle tempo adjustment
            beatPeriodMs_ = clampf(beatPeriodMs_, 60000.0f / bpmMax_, 60000.0f / bpmMin_);
            bpm_ = 60000.0f / beatPeriodMs_;
        }

        // Update confidence based on phase error
        if (absf(phaseError) < 0.2f) {
            // On-beat transient: boost confidence
            confidence_ += 0.1f * strength;
        } else if (absf(phaseError) > 0.4f) {
            // Off-beat transient: reduce confidence slightly
            confidence_ -= 0.05f;
        }
        confidence_ = clampf(confidence_, 0.0f, 1.0f);
    } else {
        // No strong rhythm detected - just reset phase on transients
        phase_ = 0.0f;
    }
}

// ===== OUTPUT SYNTHESIS =====

void AudioController::synthesizeEnergy() {
    float energy = mic_.getLevel();

    // Boost energy when rhythm is locked and near beat
    if (confidenceSmooth_ > activationThreshold) {
        // Calculate distance from beat (0 at phase 0 or 1, max at 0.5)
        float distFromBeat = absf(phase_ - 0.5f);  // 0 at off-beat, 0.5 at on-beat
        distFromBeat = 0.5f - distFromBeat;        // Invert: 0.5 at on-beat, 0 at off-beat

        // Apply boost near beats
        float beatBoost = distFromBeat * 2.0f * energyBoostOnBeat * confidenceSmooth_;
        energy *= (1.0f + beatBoost);
    }

    control_.energy = clampf(energy, 0.0f, 1.0f);
}

void AudioController::synthesizePulse() {
    float pulse = mic_.getTransient();

    // Apply beat-aligned modulation when rhythm is strong
    if (pulse > 0.0f && confidenceSmooth_ > activationThreshold) {
        // Calculate how close we are to a beat
        float distFromBeat = phase_ < 0.5f ? phase_ : (1.0f - phase_);

        // On-beat (small distance): boost
        // Off-beat (large distance): suppress
        float modulation;
        if (distFromBeat < 0.2f) {
            // Near beat: boost
            modulation = pulseBoostOnBeat;
        } else if (distFromBeat > 0.3f) {
            // Away from beat: suppress
            modulation = pulseSuppressOffBeat;
        } else {
            // Transition zone: interpolate
            float t = (distFromBeat - 0.2f) / 0.1f;  // 0 at 0.2, 1 at 0.3
            modulation = pulseBoostOnBeat * (1.0f - t) + pulseSuppressOffBeat * t;
        }

        // Apply modulation scaled by confidence
        float blend = confidenceSmooth_;
        pulse *= (1.0f - blend) + modulation * blend;
    }

    control_.pulse = clampf(pulse, 0.0f, 1.0f);
}

void AudioController::synthesizePhase() {
    control_.phase = phase_;
}

void AudioController::synthesizeRhythmStrength() {
    // Combine periodicity strength with confidence
    // Both need to be high for strong rhythm output
    float strength = periodicityStrength_ * 0.5f + confidenceSmooth_ * 0.5f;

    // Apply activation threshold
    if (strength < activationThreshold * 0.5f) {
        strength = 0.0f;
    }

    control_.rhythmStrength = clampf(strength, 0.0f, 1.0f);
}
