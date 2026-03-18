#include "AudioTracker.h"
#include <math.h>
#include <string.h>

// Max ACF lag array size: 4 * (OSS_FRAMES_PER_MIN / bpmMin) = 4 * 66 = 264
// Subtract minLag (~20) + 1 = 245. Round up for safety.
static constexpr int MAX_ACF_SIZE = 280;

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PLL_ONSET_FLOOR=0.1

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ============================================================================
// Construction / Lifecycle
// ============================================================================

AudioTracker::AudioTracker(IPdmMic& pdm, ISystemTime& time)
    : time_(time)
    , mic_(pdm, time)
    , spectral_()
{
}

AudioTracker::~AudioTracker() {
    end();
}

bool AudioTracker::begin(uint32_t sampleRate) {
    if (!mic_.begin(sampleRate)) return false;
    spectral_.begin();
    combFilterBank_.init(OSS_FRAME_RATE);

    bool nnOk = frameBeatNN_.begin();
    nnActive_ = nnOk && frameBeatNN_.isReady();

    // Initialize onset density window to current time to avoid
    // instant flush on first update (nowMs >> 0 would fire immediately)
    onsetDensityWindowStart_ = time_.millis();

    return true;
}

void AudioTracker::end() {
    mic_.end();
}

int AudioTracker::getHwGain() const {
    return mic_.getHwGain();
}

// ============================================================================
// Main Update Loop
// ============================================================================

const AudioControl& AudioTracker::update(float dt) {
    uint32_t nowMs = time_.millis();

    // 1. Mic input
    mic_.update(dt);

    // 2. Feed spectral analysis
    static int16_t sampleBuffer[256];  // static: avoid 512 bytes on stack per frame
    int samplesRead = mic_.getSamplesForExternal(sampleBuffer, 256);
    if (samplesRead > 0) {
        spectral_.addSamples(sampleBuffer, samplesRead);
    }

    // 3. Process spectral data when samples are available
    bool newSpectralFrame = false;
    if (spectral_.hasSamples()) {
        spectral_.process();
    }

    // 4. NN inference → onset activation for pulse detection + PLL phase refinement.
    //    Only run NN when a new spectral frame is ready. Between frames,
    //    use last activation. Note: NN output is NOT used for BPM estimation
    //    (spectral flux handles that) — it drives visual pulse and PLL correction.
    float odf = 0.0f;
    uint32_t currentFrameCount = spectral_.getFrameCount();
    if (nnActive_ && currentFrameCount > lastSpectralFrameCount_) {
        lastSpectralFrameCount_ = currentFrameCount;
        frameBeatNN_.setProfileEnabled(nnProfile);
        odf = frameBeatNN_.infer(spectral_.getRawMelBands());
        odf = clampf(odf, 0.0f, 1.0f);
        newSpectralFrame = true;
    } else if (!nnActive_) {
        // Fallback: use mic level as simple energy ODF
        odf = mic_.getLevel();
        newSpectralFrame = true;  // mic level updates every frame
    } else {
        // NN active but no new spectral frame — skip OSS/comb update
        // to avoid duplicate samples in the ACF buffer.
        // Still run PLL (free-running) and output synthesis.
        odf = frameBeatNN_.getLastBeat();
    }

    // Track significant audio for silence detection
    if (mic_.getLevel() > 0.05f) {
        lastSignificantAudioMs_ = nowMs;
    }

    // 5. Pulse detection runs every frame (uses raw ODF, before gating)
    updatePulseDetection(odf, dt, nowMs);

    // 6. ODF information gate — suppress noise-driven false beats
    //    Applied after pulse detection so transient sensitivity is unaffected.
    if (odf < odfGateThreshold) {
        odf = 0.02f;
    }

    // 7-8. Feed DSP components only on new spectral frames.
    //      BPM estimation uses spectral flux (NN-independent broadband transient
    //      signal) — not NN onset activation. This decouples tempo estimation from
    //      the NN, which can't distinguish on-beat from off-beat onsets. Syncopated
    //      kicks, hi-hats, and off-beat transients in NN output would corrupt ACF
    //      periodicity. Spectral flux is a raw acoustic transient signal that
    //      preserves the periodic structure ACF needs.
    //
    //      NN onset activation is used for:
    //        - Pulse detection (visual sparks/flashes, step 5 above)
    //        - PLL phase refinement (onset-gated correction, step 10 below)
    //        - Energy synthesis (ODF peak-hold, in synthesizeOutputs)
    if (newSpectralFrame) {
        float flux = spectral_.getSpectralFlux();
        // Apply contrast sharpening (power-law) before buffering.
        // Squaring sharpens peaks relative to baseline, improving ACF.
        float fluxContrast = (odfContrast == 2.0f) ? flux * flux : powf(flux, odfContrast);

        combFilterBank_.feedbackGain = combFeedback;
        combFilterBank_.process(fluxContrast);
        addOssSample(fluxContrast);
    }

    // 9. Periodic ACF for tempo estimation
    //    First ACF fires after ~1s (ossCount_ >= 60 at ~66 Hz), then every
    //    acfPeriodMs (~150ms = ~9 frames). The 60-sample minimum ensures
    //    enough OSS data for meaningful autocorrelation.
    if (ossCount_ >= 60 && (nowMs - lastAcfMs_ >= acfPeriodMs)) {
        lastAcfMs_ = nowMs;
        runAutocorrelation();
    }

    // 10. PLL update (free-running every frame + onset correction)
    updatePll(odf, nowMs);

    // 11. Decay periodicity during silence (between ACF runs)
    if (nowMs - lastSignificantAudioMs_ > 2000) {
        periodicityStrength_ *= 0.998f;  // ~0.5s half-life at 62.5 Hz
    }

    // 12. Output synthesis
    synthesizeOutputs(dt, nowMs);

    return control_;
}

// ============================================================================
// OSS Buffer
// ============================================================================

void AudioTracker::addOssSample(float odf) {
    ossBuffer_[ossWriteIdx_] = odf;
    ossWriteIdx_ = (ossWriteIdx_ + 1) % OSS_BUFFER_SIZE;
    if (ossCount_ < OSS_BUFFER_SIZE) ossCount_++;
}

// ============================================================================
// Autocorrelation + Tempo Estimation
// ============================================================================

void AudioTracker::runAutocorrelation() {
    // Linearize circular OSS buffer (static: avoid 1.4 KB on stack per call)
    static float ossLinear[OSS_BUFFER_SIZE];
    int startIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    for (int i = 0; i < ossCount_; i++) {
        ossLinear[i] = ossBuffer_[(startIdx + i) % OSS_BUFFER_SIZE];
    }

    // Compute mean for mean subtraction (important for spectral flux with non-zero baseline)
    float ossMean = 0.0f;
    for (int i = 0; i < ossCount_; i++) ossMean += ossLinear[i];
    ossMean /= ossCount_;

    // Lag range from BPM limits
    int minLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMax);
    int maxLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMin);
    if (maxLag >= ossCount_ / 2) maxLag = ossCount_ / 2 - 1;
    if (minLag < 1) minLag = 1;

    // Extended range for harmonic enhancement
    int harmonicMaxLag = maxLag * 4;
    if (harmonicMaxLag >= ossCount_ - 1) harmonicMaxLag = ossCount_ - 2;

    // Compute ACF at all lags (fixed-size array, no VLA)
    int acfSize = harmonicMaxLag - minLag + 1;
    if (acfSize > MAX_ACF_SIZE) acfSize = MAX_ACF_SIZE;
    if (acfSize <= 0) return;  // Safety: not enough data

    float acf[MAX_ACF_SIZE];  // Fixed size on stack (~1.1 KB)
    float signalEnergy = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        float v = ossLinear[i] - ossMean;
        signalEnergy += v * v;
    }

    for (int lagIdx = 0; lagIdx < acfSize; lagIdx++) {
        int lag = minLag + lagIdx;
        float sum = 0.0f;
        int count = ossCount_ - lag;
        if (count <= 0) { acf[lagIdx] = 0.0f; continue; }
        for (int i = 0; i < count; i++) {
            sum += (ossLinear[i] - ossMean) * (ossLinear[i + lag] - ossMean);
        }
        acf[lagIdx] = sum / count;
    }

    // Percival harmonic enhancement (fold 2nd+4th harmonics into fundamental)
    percivalEnhance(acf, minLag, maxLag, harmonicMaxLag, acfSize);

    // Find peak in fundamental range with Rayleigh prior weighting
    float bestCorr = -1e30f;
    int bestLag = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);

    // Precompute Rayleigh sigma from rayleighBpm
    float rayleighLag = OSS_FRAMES_PER_MIN / rayleighBpm;
    float rayleighSigma2 = rayleighLag * rayleighLag;

    for (int lagIdx = 0; lagIdx <= maxLag - minLag && lagIdx < acfSize; lagIdx++) {
        int lag = minLag + lagIdx;
        float lagF = static_cast<float>(lag);

        // Compute Rayleigh weight directly per lag (no comb bin lookup)
        float rayleighW = (lagF / rayleighSigma2) * expf(-lagF * lagF / (2.0f * rayleighSigma2));

        float weighted = acf[lagIdx] * rayleighW;
        if (weighted > bestCorr) {
            bestCorr = weighted;
            bestLag = lag;
        }
    }

    // Compute periodicity strength from unweighted ACF at best lag
    float avgEnergy = signalEnergy / ossCount_;
    if (avgEnergy > 1e-10f && bestLag - minLag < acfSize) {
        float normCorr = acf[bestLag - minLag] / avgEnergy;
        float newStrength = clampf(normCorr * 1.5f, 0.0f, 1.0f);
        periodicityStrength_ = periodicityStrength_ * 0.5f + newStrength * 0.5f;
    }

    // ACF candidate BPM
    float acfBpm = OSS_FRAMES_PER_MIN / static_cast<float>(bestLag);

    // Tempo selection: ACF primary, comb bank validates
    float combBpm = combFilterBank_.getPeakBPM();
    float agreement = fabsf(acfBpm - combBpm) / acfBpm;

    float newBpm = acfBpm;
    if (agreement < 0.10f) {
        // ACF and comb agree — high confidence, use average
        newBpm = (acfBpm + combBpm) * 0.5f;
    }

    // Clamp to valid range
    newBpm = clampf(newBpm, bpmMin, bpmMax);

    // Smooth BPM update (EMA) — only if meaningful change
    float bpmChange = fabsf(newBpm - bpm_) / bpm_;
    if (bpmChange > 0.05f) {
        bpm_ = bpm_ * tempoSmoothing + newBpm * (1.0f - tempoSmoothing);
    }
    beatPeriodFrames_ = OSS_FRAME_RATE * 60.0f / bpm_;
}

void AudioTracker::percivalEnhance(float* acf, int minLag, int maxLag,
                                    int harmonicMaxLag, int acfSize) {
    // Percival 2014: fold 2nd and 4th harmonic ACF peaks into fundamental.
    // This gives the fundamental lag a unique advantage over its harmonics.
    // NOTE: In-place modification means earlier lags accumulate before later lags
    // read them. This matches AudioController behavior (proven via A/B testing).
    for (int lagIdx = 0; lagIdx <= maxLag - minLag && lagIdx < acfSize; lagIdx++) {
        int lag = minLag + lagIdx;

        // 2nd harmonic: ACF[2L] folds into ACF[L]
        int harm2Idx = lag * 2 - minLag;
        if (harm2Idx >= 0 && harm2Idx < acfSize) {
            acf[lagIdx] += percivalWeight2 * acf[harm2Idx];
        }

        // 4th harmonic: ACF[4L] folds into ACF[L]
        int harm4Idx = lag * 4 - minLag;
        if (harm4Idx >= 0 && harm4Idx < acfSize) {
            acf[lagIdx] += percivalWeight4 * acf[harm4Idx];
        }
    }
}

// ============================================================================
// PLL Phase Tracking
// ============================================================================

void AudioTracker::updatePll(float odf, uint32_t nowMs) {
    // Free-running sawtooth: advance phase at current BPM
    float phaseIncrement = 1.0f / beatPeriodFrames_;
    pllPhase_ += phaseIncrement;

    // Beat wrap
    if (pllPhase_ >= 1.0f) {
        pllPhase_ -= 1.0f;
        beatCount_++;
    }

    // Onset-gated PLL correction:
    // Only correct when a strong onset aligns with expected beat (within ±25%).
    // Phase 0 = on-beat. An onset at phase 0.1 means our clock ran ahead (beat
    // was later than predicted). Correct by pulling phase back toward 0.
    bool isStrongOnset = (odf > odfBaseline_ * pulseThresholdMult) && (odf > pllOnsetFloor);

    if (isStrongOnset) {
        // Phase error: distance from nearest beat boundary.
        // Positive = we're past the beat (clock ahead), negative = before the beat.
        float phaseError = pllPhase_;
        if (phaseError > 0.5f) phaseError -= 1.0f;  // Center: [-0.5, +0.5]

        // Only correct if onset is near expected beat (within ±25% of period)
        if (fabsf(phaseError) < pllNearBeatWindow) {
            // Scale correction by onset strength: strong kicks get full
            // correction, weak onsets get proportionally less. Prevents
            // marginal transients from pulling phase as much as clear beats.
            float correctionScale = clampf(
                (odf - pllOnsetFloor) / (1.0f - pllOnsetFloor), 0.0f, 1.0f);

            // Proportional correction: pull phase back toward beat boundary.
            // Negative feedback: subtract error to reduce it.
            pllPhase_ -= pllKp * phaseError * correctionScale;

            // Integral correction: persistent frequency bias for systematic offset.
            pllIntegral_ = pllIntegralDecay * pllIntegral_ + phaseError * correctionScale;
            pllPhase_ -= pllKi * pllIntegral_;

            // Keep phase in [0, 1)
            if (pllPhase_ < 0.0f) pllPhase_ += 1.0f;
            if (pllPhase_ >= 1.0f) pllPhase_ -= 1.0f;
        }
    }

    // Decay integral during silence (prevent wind-up)
    if (nowMs - lastSignificantAudioMs_ > 2000) {
        pllIntegral_ *= pllSilenceDecay;
    }
}

// ============================================================================
// Pulse Detection (Floor-Tracking Baseline)
// ============================================================================

void AudioTracker::updatePulseDetection(float odf, float dt, uint32_t nowMs) {
    // Floor-tracking baseline (matched to AudioController):
    // Slow rise (peaks don't inflate baseline), fast drop (floor drops caught quickly).
    // Uses fixed alpha, not dt-based exponential, to match proven AudioController behavior.
    if (odf < odfBaseline_) {
        odfBaseline_ += (odf - odfBaseline_) * baselineFastDrop;   // fast drop
    } else {
        odfBaseline_ += (odf - odfBaseline_) * baselineSlowRise;   // slow rise
    }

    // ODF peak hold for energy synthesis (fast attack, ~100ms release at 62.5 Hz)
    if (odf > odfPeakHold_) {
        odfPeakHold_ = odf;
    } else {
        odfPeakHold_ *= odfPeakHoldDecay;
    }

    // Pulse detection: fire when ODF exceeds baseline threshold
    float pulseThreshold = odfBaseline_ * pulseThresholdMult;
    if (pulseThreshold < pllOnsetFloor) pulseThreshold = pllOnsetFloor;

    // Tempo-adaptive cooldown: shorter at faster tempos
    float bpmNorm = clampf((bpm_ - 60.0f) / 140.0f, 0.0f, 1.0f);
    float cooldownMs = 40.0f + 110.0f * (1.0f - bpmNorm);

    // Guard against ms wraparound
    if (nowMs < lastPulseMs_) lastPulseMs_ = nowMs;

    float pulseStrength = 0.0f;
    if (mic_.getLevel() > pulseMinLevel &&
        odf > pulseThreshold &&
        (nowMs - lastPulseMs_) > static_cast<uint32_t>(cooldownMs)) {
        pulseStrength = clampf(odf, 0.0f, 1.0f);
        lastPulseMs_ = nowMs;
    }
    lastPulseStrength_ = pulseStrength;
}

// ============================================================================
// Output Synthesis
// ============================================================================

void AudioTracker::synthesizeOutputs(float dt, uint32_t nowMs) {
    // --- Energy ---
    // Hybrid: mic level + bass mel energy + ODF peak-hold
    float micLevel = mic_.getLevel();
    float bassMelEnergy = 0.0f;
    const float* melBands = spectral_.getMelBands();
    if (melBands) {
        for (int i = 1; i <= 6; i++) {
            bassMelEnergy += melBands[i];
        }
        bassMelEnergy /= 6.0f;
    }

    float rawEnergy = energyMicWeight * micLevel + energyMelWeight * bassMelEnergy + energyOdfWeight * odfPeakHold_;

    // Beat-proximity boost
    float phaseDistance = pllPhase_ < 0.5f ? pllPhase_ : (1.0f - pllPhase_);
    float nearBeat = 1.0f - (phaseDistance / energyBoostWindow);
    if (nearBeat < 0.0f) nearBeat = 0.0f;
    rawEnergy *= (1.0f + nearBeat * energyBoostOnBeat * periodicityStrength_);

    control_.energy = clampf(rawEnergy, 0.0f, 1.0f);

    // --- Pulse ---
    float pulse = lastPulseStrength_;
    if (periodicityStrength_ > activationThreshold) {
        if (phaseDistance < pulseNearBeatThreshold) {
            pulse *= pulseBoostOnBeat;
        } else if (phaseDistance > pulseFarFromBeatThreshold) {
            pulse *= pulseSuppressOffBeat;
        }
    }
    control_.pulse = clampf(pulse, 0.0f, 1.0f);

    // --- Phase ---
    control_.phase = pllPhase_;

    // --- Rhythm Strength ---
    float combConf = combFilterBank_.getPeakConfidence();
    float strength = periodicityStrength_ * 0.6f + combConf * 0.4f;

    // Soft activation gate (quadratic falloff below threshold)
    if (strength < activationThreshold) {
        float ratio = strength / (activationThreshold + 0.001f);
        strength *= ratio;
    }

    // Decay during silence
    if (nowMs - lastSignificantAudioMs_ > 3000) {
        strength *= 0.95f;
    }

    control_.rhythmStrength = clampf(strength, 0.0f, 1.0f);

    // --- Onset Density ---
    // Count pulses per second (1-second window)
    if (lastPulseStrength_ > 0.0f) {
        onsetCountInWindow_++;
    }
    if (nowMs - onsetDensityWindowStart_ >= 1000) {
        float newDensity = static_cast<float>(onsetCountInWindow_);
        onsetDensity_ = onsetDensity_ * 0.7f + newDensity * 0.3f;
        onsetCountInWindow_ = 0;
        onsetDensityWindowStart_ = nowMs;
    }
    control_.onsetDensity = onsetDensity_;

    // --- Downbeat / BeatInMeasure ---
    // Not tracked. Current NN (Conv1D W16) is onset-only (single output channel).
    // A future multi-output model could provide downbeat activation.
    control_.downbeat = 0.0f;
    control_.beatInMeasure = 0;
}
