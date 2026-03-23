#include "AudioTracker.h"
#include <math.h>
#include <string.h>

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PULSE_ONSET_FLOOR=0.1

static constexpr float TWO_PI_F = 6.28318530718f;

// Adaptive phase correction constants (from parameter sweep data)
static constexpr float PHASE_ALPHA_ERR = 0.15f;   // Error EMA rate (~6-7 sample window at 150ms cadence)
static constexpr float PHASE_SIGMA_REF = 0.08f;   // Expected RMS phase error when locked
static constexpr float PHASE_ALPHA_MIN = 0.10f;    // Correction rate when locked (sweep: stability=0.87)
static constexpr float PHASE_ALPHA_MAX = 0.50f;    // Correction rate when converging (sweep: atTransient=0.50)

// Cold-start template bank: 8 common rhythmic patterns (16 bins = 16th-note resolution).
// Used ONLY for 1-bar seeding during cold start — not for runtime matching or caching.
// ~512 bytes flash. Each pattern normalized to sum ~1.0.
static constexpr int NUM_SEED_TEMPLATES = 8;
static constexpr int SEED_TEMPLATE_BINS = 16;
static const float SEED_TEMPLATES[NUM_SEED_TEMPLATES][SEED_TEMPLATE_BINS] = {
    {0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f},  // 4otf
    {0.20f,0.0f,0.0f,0.0f, 0.30f,0.0f,0.0f,0.0f, 0.20f,0.0f,0.0f,0.0f, 0.30f,0.0f,0.0f,0.0f},  // backbeat
    {0.35f,0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f,0.0f,  0.35f,0.0f,0.0f,0.0f, 0.15f,0.0f,0.0f,0.15f}, // halftime
    {0.20f,0.0f,0.0f,0.15f,0.0f,0.0f,0.20f,0.0f,  0.10f,0.0f,0.0f,0.0f, 0.15f,0.0f,0.20f,0.0f}, // breakbeat
    {0.15f,0.0f,0.10f,0.0f,0.15f,0.0f,0.10f,0.0f,  0.15f,0.0f,0.10f,0.0f,0.15f,0.0f,0.10f,0.0f}, // 8th note
    {0.25f,0.0f,0.10f,0.0f,0.0f,0.0f,0.0f,0.0f,   0.30f,0.0f,0.0f,0.10f,0.0f,0.0f,0.15f,0.10f}, // dnb
    {0.20f,0.0f,0.0f,0.15f,0.0f,0.0f,0.20f,0.0f,  0.0f,0.0f,0.0f,0.15f, 0.20f,0.0f,0.0f,0.10f}, // dembow
    {0.50f,0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f,0.0f,  0.10f,0.0f,0.0f,0.0f, 0.20f,0.0f,0.0f,0.20f},  // sparse
};

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

    bool nnOk = frameOnsetNN_.begin();
    nnActive_ = nnOk && frameOnsetNN_.isReady();

    // Initialize time-based state to current time to avoid
    // instant flush on first update (nowMs >> 0 would fire immediately)
    uint32_t now = time_.millis();
    onsetDensityWindowStart_ = now;
    resetSlots();

    return true;
}

void AudioTracker::end() {
    mic_.end();
}

int AudioTracker::getHwGain() const {
    return mic_.getHwGain();
}

void AudioTracker::resetSlots() {
    for (int i = 0; i < SLOT_COUNT; i++) {
        memset(slots_[i].bins, 0, sizeof(slots_[i].bins));
        slots_[i].confidence = 0.0f;
        slots_[i].totalBars = 0;
        slots_[i].age = 0;
        slots_[i].valid = false;
        slots_[i].seeded = false;
    }
    memset(currentDigest_, 0, sizeof(currentDigest_));
    activeSlot_ = -1;
    lastSlotCheckBeat_ = 0;
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

    // 4. NN inference → onset activation for pulse detection.
    //    Only run NN when a new spectral frame is ready. Between frames,
    //    use last activation. Note: NN output is NOT used for BPM estimation
    //    (spectral flux handles that) — it drives visual pulse only.
    float odf = 0.0f;
    uint32_t currentFrameCount = spectral_.getFrameCount();
    if (nnActive_ && currentFrameCount > lastSpectralFrameCount_) {
        lastSpectralFrameCount_ = currentFrameCount;
        frameOnsetNN_.setProfileEnabled(nnProfile);
        odf = frameOnsetNN_.infer(spectral_.getRawMelBands());
        odf = clampf(odf, 0.0f, 1.0f);
        newSpectralFrame = true;
    } else if (!nnActive_) {
        // Fallback: use mic level as simple energy ODF
        odf = mic_.getLevel();
        newSpectralFrame = true;  // mic level updates every frame
    } else {
        // NN active but no new spectral frame — skip OSS update
        // to avoid duplicate samples in the ACF buffer.
        // Still run PLP phase advance and output synthesis.
        odf = frameOnsetNN_.getLastOnset();
    }

    // Track significant audio for silence detection
    if (mic_.getLevel() > 0.05f) {
        lastSignificantAudioMs_ = nowMs;
    }

    // 5. Pulse detection runs every frame (uses raw ODF, before gating)
    updatePulseDetection(odf, dt, nowMs);

    // 6. Feed DSP components only on new spectral frames.
    if (newSpectralFrame) {
        float flux = spectral_.getSpectralFlux();
        // Contrast sharpening (power-law) before buffering — sharpens peaks for ACF.
        float fluxContrast;
        if (odfContrast == 2.0f) {
            fluxContrast = flux * flux;
        } else if (odfContrast == 1.0f) {
            fluxContrast = flux;
        } else if (odfContrast == 0.5f) {
            fluxContrast = sqrtf(flux);
        } else {
            fluxContrast = powf(flux, odfContrast);
        }
        fluxContrast = clampf(fluxContrast, 0.0f, 1.0f);
        addOssSample(fluxContrast);

        // Cache bass energy (used by PLP dual-source AND energy synthesis)
        cachedBassEnergy_ = 0.0f;
        const float* mel = spectral_.getMelBands();
        if (mel) {
            for (int i = 1; i <= 6; i++) cachedBassEnergy_ += mel[i];
            cachedBassEnergy_ /= 6.0f;
        }
        addBassSample(cachedBassEnergy_);

        // Buffer raw NN onset activation for PLP source comparison
        nnOnsetBuffer_[nnWriteIdx_] = odf;
        nnWriteIdx_ = (nnWriteIdx_ + 1) % NN_BUFFER_SIZE;
        if (nnCount_ < NN_BUFFER_SIZE) nnCount_++;
    }

    // 9. Periodic ACF for tempo estimation + IOI analysis
    //    First ACF fires after ~1s (ossCount_ >= 60 at ~66 Hz), then every
    //    acfPeriodMs (~150ms = ~9 frames). The 60-sample minimum ensures
    //    enough OSS data for meaningful autocorrelation.
    if (ossCount_ >= 60 && (nowMs - lastAcfMs_ >= acfPeriodMs)) {
        lastAcfMs_ = nowMs;
        runFourierTempogram();
        updatePlpAnalysis();  // PLP epoch-fold + bass ACF + cross-correlate
    }

    // 10. PLP phase update (free-running + pattern-based correction)
    updatePlpPhase();

    // 11. Decay periodicity during silence (between ACF runs)
    if (nowMs - lastSignificantAudioMs_ > 2000) {
        periodicityStrength_ *= 0.998f;  // ~0.5s half-life at 62.5 Hz
    }

    // 12. Pattern slot cache: check every bar (4 beats)
    if ((beatCount_ & 3) == 0 && beatCount_ != lastSlotCheckBeat_) {
        lastSlotCheckBeat_ = beatCount_;
        if (plpConfidence_ > 0.1f) {
            checkPatternSlots();
        }
    }

    // 13. Output synthesis
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
// Fourier Tempogram — Period + Phase Selection
// ============================================================================

void AudioTracker::runFourierTempogram() {
    // Linearize all 3 source circular buffers into class members
    int startIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    for (int i = 0; i < ossCount_; i++) {
        ossLinear_[i] = ossBuffer_[(startIdx + i) % OSS_BUFFER_SIZE];
    }
    int bStart = (bassWriteIdx_ - bassCount_ + BASS_BUFFER_SIZE) % BASS_BUFFER_SIZE;
    for (int i = 0; i < bassCount_; i++) bassLinear_[i] = bassBuffer_[(bStart + i) % BASS_BUFFER_SIZE];
    int nStart = (nnWriteIdx_ - nnCount_ + NN_BUFFER_SIZE) % NN_BUFFER_SIZE;
    for (int i = 0; i < nnCount_; i++) nnLinear_[i] = nnOnsetBuffer_[(nStart + i) % NN_BUFFER_SIZE];

    // --- Fourier Tempogram: DFT at candidate tempo frequencies ---
    // For each candidate BPM, compute the DFT coefficient (Goertzel-style).
    // DFT magnitude selects the period. DFT phase gives beat alignment for free.
    // Unlike epoch-fold PMR, the Fourier tempogram inherently suppresses
    // sub-harmonics (half-time) because a sub-harmonic sinusoid anti-correlates
    // with half the onset peaks. (Grosche & Mueller 2011)

    float* sources[3] = { ossLinear_, bassLinear_, nnLinear_ };
    const int sourceCounts[3] = { ossCount_, bassCount_, nnCount_ };

    // Mean-subtract each source IN-PLACE (critical for DFT — DC leakage otherwise
    // dominates all frequency bins, making periodic components invisible).
    // NOTE: this mutates ossLinear_/bassLinear_/nnLinear_ — updatePlpAnalysis()
    // reads these mean-subtracted values; its min-max normalization handles
    // the zero-centered epoch-fold output. Do not reorder these calls.
    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < 20) continue;
        float mean = 0.0f;
        for (int i = 0; i < count; i++) mean += sources[src][i];
        mean /= count;
        for (int i = 0; i < count; i++) sources[src][i] -= mean;
    }

    float bestMag = 0.0f;
    float dftMagSum = 0.0f;   // Sum of all DFT magnitudes (for Fisher's g-statistic)
    int dftMagCount = 0;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    float bestPhase = 0.0f;
    int bestSource = 0;

    // Scan candidate periods from bpmMin to bpmMax
    int minLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMax);
    int maxLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMin);
    if (minLag < 10) minLag = 10;
    if (maxLag > MAX_PATTERN_LEN) maxLag = MAX_PATTERN_LEN;  // Keep period ≤ pattern buffer
    if (maxLag >= ossCount_ / 2) maxLag = ossCount_ / 2 - 1; // Avoid scanning past buffer at startup

    for (int lag = minLag; lag <= maxLag; lag++) {
        // Goertzel recurrence: computes one DFT bin with 2 multiply-adds per sample.
        float omega = TWO_PI_F / static_cast<float>(lag);
        float cosOmega = cosf(omega);
        float sinOmega = sinf(omega);
        float coeff = 2.0f * cosOmega;

        for (int src = 0; src < 3; src++) {
            int count = sourceCounts[src];
            if (count < lag * 2) continue;
            const float* buf = sources[src];

            // Goertzel recurrence: s[n] = x[n] + coeff*s[n-1] - s[n-2]
            float s1 = 0.0f, s2 = 0.0f;
            for (int i = 0; i < count; i++) {
                float s0 = buf[i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            // Extract real + imag from Goertzel final state
            float dftReal = s1 - s2 * cosOmega;
            float dftImag = s2 * sinOmega;

            // Magnitude (normalize by sqrt(count) for fair comparison across sources)
            float mag = sqrtf(dftReal * dftReal + dftImag * dftImag) / sqrtf(static_cast<float>(count));
            dftMagSum += mag;
            dftMagCount++;

            if (mag > bestMag) {
                bestMag = mag;
                bestPeriod = lag;
                bestSource = src;
                // Phase at the END of the buffer (most recent sample).
                // Goertzel DFT phase is relative to index 0 (oldest sample).
                // Advance by (count-1) periods to get phase at the newest sample.
                float rawPhase = -atan2f(dftImag, dftReal) / TWO_PI_F;
                float phaseAdvance = static_cast<float>(count - 1) / static_cast<float>(lag);
                bestPhase = rawPhase + phaseAdvance;
                bestPhase -= floorf(bestPhase);  // Wrap to [0, 1)
            }
        }
    }

    plpDftMag_ = bestMag;
    // Fisher's g-statistic: max magnitude / sum of all magnitudes.
    // Measures how concentrated the periodic energy is at one frequency.
    // g > 0.3 → highly significant periodicity (p < 0.01)
    // g ~ 0.05 → no significant periodicity (uniform spectrum)
    plpFisherG_ = (dftMagSum > 1e-10f) ? bestMag / dftMagSum : 0.0f;

    // Reset adaptive phase correction state on significant period change
    // (>10% shift). Prevents old low-variance state from suppressing
    // the fast correction needed to re-converge at the new period.
    if (abs(bestPeriod - plpBestPeriod_) > plpBestPeriod_ / 10) {
        phaseErrVar_ = 0.25f;  // Force fast convergence
    }

    plpBestPeriod_ = bestPeriod;
    plpBestSource_ = static_cast<uint8_t>(bestSource);
    plpDftPhase_ = bestPhase;

    // --- Periodicity strength from DFT magnitude ---
    // With sqrt(N) normalization, magnitude ~1.0 for a clearly periodic signal.
    // This is not a normalized cross-correlation [-1,1] but a spectral magnitude
    // where ~1.0 indicates strong periodicity. Clamp to [0,1] for rhythmStrength.
    float newStrength = clampf(bestMag, 0.0f, 1.0f);
    periodicityStrength_ = periodicityStrength_ * 0.5f + newStrength * 0.5f;

    // BPM from best period (informational — PLP uses plpBestPeriod_ directly)
    float newBpm = OSS_FRAMES_PER_MIN / static_cast<float>(bestPeriod);
    newBpm = clampf(newBpm, bpmMin, bpmMax);
    float bpmChange = fabsf(newBpm - bpm_) / bpm_;
    if (bpmChange > 0.05f) {
        bpm_ = bpm_ * tempoSmoothing + newBpm * (1.0f - tempoSmoothing);
    }
    beatPeriodFrames_ = OSS_FRAME_RATE * 60.0f / bpm_;

    // Invalidate pattern slots on significant BPM change (>15%)
    if (prevBpm_ > 0.0f && fabsf(bpm_ - prevBpm_) / prevBpm_ > 0.15f) {
        resetSlots();
    }
    prevBpm_ = bpm_;
}

// ============================================================================
// PLP (Predominant Local Pulse) — Dual-Source Pattern Extraction
// ============================================================================

void AudioTracker::addBassSample(float bassEnergy) {
    bassBuffer_[bassWriteIdx_] = bassEnergy;
    bassWriteIdx_ = (bassWriteIdx_ + 1) % BASS_BUFFER_SIZE;
    if (bassCount_ < BASS_BUFFER_SIZE) bassCount_++;
}

void AudioTracker::updatePlpAnalysis() {
    // --- 1. Use raw period from grid search (not BPM-smoothed) ---
    // The grid search found the exact period with best PMR. Using the BPM-smoothed
    // beatPeriodFrames_ introduces a round-trip error (period→BPM→EMA→period)
    // that can shift the fold by ±1 frame and degrade coherence.
    int patLen = plpBestPeriod_;
    if (patLen < 2) patLen = 2;
    if (patLen > MAX_PATTERN_LEN) patLen = MAX_PATTERN_LEN;
    plpPatternLen_ = patLen;

    // --- 2. Epoch-fold the WINNING source at the winning period ---
    // All linearized buffers populated by runFourierTempogram() (class members).
    // NOTE: buffers are mean-subtracted by runFourierTempogram() — epoch-fold
    // produces zero-centered values, hence min-max normalization below.
    const float* sourceBuf = ossLinear_;
    int sourceCount = ossCount_;
    if (plpBestSource_ == 1 && bassCount_ >= patLen * 2) {
        sourceBuf = bassLinear_;
        sourceCount = bassCount_;
    } else if (plpBestSource_ == 2 && nnCount_ >= patLen * 2) {
        sourceBuf = nnLinear_;
        sourceCount = nnCount_;
    }

    if (sourceCount < patLen * 2) return;

    // Fold and average
    float patternAccum[MAX_PATTERN_LEN] = {0};
    int epochs = 0;
    for (int offset = sourceCount - patLen; offset >= 0; offset -= patLen) {
        for (int j = 0; j < patLen; j++) {
            patternAccum[j] += sourceBuf[offset + j];
        }
        epochs++;
    }
    if (epochs < 2) return;

    // Normalize to [0, 1] using min-max (signal is mean-subtracted, may have negatives)
    float minVal = patternAccum[0], maxVal = patternAccum[0];
    for (int j = 0; j < patLen; j++) {
        patternAccum[j] /= epochs;
        if (patternAccum[j] < minVal) minVal = patternAccum[j];
        if (patternAccum[j] > maxVal) maxVal = patternAccum[j];
    }
    float range = maxVal - minVal;
    if (range > 1e-10f) {
        for (int j = 0; j < patLen; j++) {
            float normalized = (patternAccum[j] - minVal) / range;
            plpPattern_[j] = (plpNovGain != 1.0f) ? powf(normalized, plpNovGain) : normalized;
        }
    } else {
        for (int j = 0; j < patLen; j++) plpPattern_[j] = 0.0f;
    }

    // --- 3. Phase alignment: adaptive correction rate ---
    // Combines DFT coarse phase and pattern-peak fine phase into a single error,
    // then applies an adaptive correction rate that's fast during convergence
    // and slow once locked. Based on EMA variance of recent phase errors.

    // Compute phase error from pattern peak (most precise alignment)
    float phaseError = 0.0f;
    bool hasPatternPeak = false;
    {
        int peakIdx = 0;
        float peakVal = plpPattern_[0];
        for (int j = 1; j < patLen; j++) {
            if (plpPattern_[j] > peakVal) { peakVal = plpPattern_[j]; peakIdx = j; }
        }
        // After min-max normalization, max=1.0 and min=0.0 for any non-flat pattern.
        // peakVal > 0.5 always true for non-flat signals. Falls through to DFT phase
        // fallback only when pattern is flat (range < 1e-10, all values set to 0.0).
        if (peakVal > 0.5f) {
            float peakPhase = static_cast<float>(peakIdx) / static_cast<float>(patLen);
            phaseError = peakPhase - plpPhase_;
            hasPatternPeak = true;
        }
    }

    // Fall back to DFT phase if pattern peak is weak
    if (!hasPatternPeak) {
        phaseError = plpDftPhase_ - plpPhase_;
    }

    // Wrap error to [-0.5, 0.5]
    if (phaseError > 0.5f) phaseError -= 1.0f;
    if (phaseError < -0.5f) phaseError += 1.0f;

    // Adaptive correction rate from phase error variance (EMA)
    // High variance → still converging → aggressive correction
    // Low variance → locked → gentle correction (prevents jitter)
    phaseErrEma_ += PHASE_ALPHA_ERR * (phaseError - phaseErrEma_);
    float deviation = phaseError - phaseErrEma_;
    phaseErrVar_ += PHASE_ALPHA_ERR * (deviation * deviation - phaseErrVar_);

    float sigma = sqrtf(phaseErrVar_ > 0.0f ? phaseErrVar_ : 0.0f);
    float lambda = sigma / (sigma + PHASE_SIGMA_REF);
    float alpha = PHASE_ALPHA_MIN + (PHASE_ALPHA_MAX - PHASE_ALPHA_MIN) * lambda;

    plpPhase_ += alpha * phaseError;
    if (plpPhase_ < 0.0f) plpPhase_ += 1.0f;
    if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;

    // --- 4. PLP confidence from DFT magnitude + steep signal gate ---
    // Use DFT magnitude directly (not Fisher's g — g penalizes tracks with multiple
    // similarly-strong periods, common in syncopated music). Soft blend handles the
    // confidence-to-output mapping without needing a principled [0,1] scale.
    float dftConf = clampf(plpDftMag_, 0.0f, 1.0f);

    // Steep signal gate: transition from 0→1 over a narrow range near noise floor.
    // Once there's clearly audio (>2x noise floor), presence is 1.0 and doesn't
    // attenuate DFT confidence. Only suppresses during true silence.
    float micLevel = mic_.getLevel();
    float signalPresence = clampf((micLevel - plpSignalFloor * 0.5f) / (plpSignalFloor * 0.5f), 0.0f, 1.0f);
    float targetConf = dftConf * signalPresence;
    plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;
}

void AudioTracker::updatePlpPhase() {
    // Free-running phase advance at the PMR-winning period (not BPM-smoothed)
    int period = (plpBestPeriod_ > 0) ? plpBestPeriod_ : 33;  // Guard against zero
    float phaseIncrement = 1.0f / static_cast<float>(period);
    plpPhase_ += phaseIncrement;

    // Beat wrap
    if (plpPhase_ >= 1.0f) {
        plpPhase_ -= 1.0f;
        beatCount_++;
    }

    // Read PLP pattern and cosine fallback, blend by confidence.
    // No hard threshold — published PLP systems run continuously (Meier 2024, librosa).
    // Confidence naturally approaches 0 during silence/ambient, making the output
    // smoothly degrade to the cosine fallback without a discontinuous switch.
    float patternPulse = 0.5f;
    if (plpPatternLen_ > 0) {
        float patPos = plpPhase_ * plpPatternLen_;
        int idx0 = static_cast<int>(patPos) % plpPatternLen_;
        float frac = patPos - floorf(patPos);
        int idx1 = (idx0 + 1) % plpPatternLen_;
        patternPulse = plpPattern_[idx0] * (1.0f - frac) + plpPattern_[idx1] * frac;
    }
    float cosinePulse = 0.5f + 0.5f * cosf(plpPhase_ * TWO_PI_F);
    float blend = clampf(plpConfidence_, 0.0f, 1.0f);
    plpPulseValue_ = cosinePulse * (1.0f - blend) + patternPulse * blend;

    // --- Beat stability tracking ---
    // Track PLP peak amplitude via EMA. Stability = current peak / EMA.
    // High stability (>0.7) = pattern is locked and repeating.
    // Low stability (<0.3) = disrupted (fill, breakdown, section change).
    // Used to gate bar histogram learning rate (fill/breakdown immunity).
    if (plpPhase_ < 0.05f || plpPhase_ > 0.95f) {
        // Near phase=0 (beat position): measure current PLP peak amplitude
        float peakAmp = plpPulseValue_;
        static constexpr float STABILITY_ALPHA = 0.1f;
        plpPeakEma_ += STABILITY_ALPHA * (peakAmp - plpPeakEma_);
        beatStability_ = (plpPeakEma_ > 0.01f) ? clampf(peakAmp / plpPeakEma_, 0.0f, 1.5f) : 0.0f;
    }

    // Decay confidence during extended silence
    if (plpConfidence_ > 0.0f && (time_.millis() - lastSignificantAudioMs_ > 2000)) {
        plpConfidence_ *= 0.998f;
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
    if (pulseThreshold < pulseOnsetFloor) pulseThreshold = pulseOnsetFloor;

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
// Pattern Slot Cache (v82): PLP pattern caching for instant section recall
// ============================================================================

void AudioTracker::resamplePattern(const float* src, int srcLen, float* dst, int dstLen) {
    if (srcLen <= 0) { memset(dst, 0, dstLen * sizeof(float)); return; }
    for (int i = 0; i < dstLen; i++) {
        float srcPos = static_cast<float>(i) / dstLen * srcLen;
        int idx0 = static_cast<int>(srcPos) % srcLen;
        float frac = srcPos - floorf(srcPos);
        int idx1 = (idx0 + 1) % srcLen;
        dst[i] = src[idx0] * (1.0f - frac) + src[idx1] * frac;
    }
}

float AudioTracker::cosineSimilarity(const float* a, const float* b, int len) {
    float dot = 0.0f, normA = 0.0f, normB = 0.0f;
    for (int i = 0; i < len; i++) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    float denom = sqrtf(normA) * sqrtf(normB);
    return (denom > 1e-10f) ? dot / denom : 0.0f;
}

int AudioTracker::allocateSlot() {
    // Find first invalid slot
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!slots_[i].valid) return i;
    }
    // All valid — evict oldest (highest age)
    int oldest = 0;
    for (int i = 1; i < SLOT_COUNT; i++) {
        if (slots_[i].age > slots_[oldest].age) oldest = i;
    }
    // Clear the evicted slot
    memset(slots_[oldest].bins, 0, sizeof(slots_[oldest].bins));
    slots_[oldest].valid = false;
    slots_[oldest].totalBars = 0;
    slots_[oldest].confidence = 0.0f;
    slots_[oldest].seeded = false;
    return oldest;
}

void AudioTracker::checkPatternSlots() {
    if (plpPatternLen_ < 2) return;

    // 1. Resample current PLP pattern to 16-bin digest
    resamplePattern(plpPattern_, plpPatternLen_, currentDigest_, SLOT_BINS);

    // 2. Compare against all valid slots
    int bestSlot = -1;
    float bestSim = 0.0f;
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (!slots_[i].valid) continue;
        float sim = cosineSimilarity(currentDigest_, slots_[i].bins, SLOT_BINS);
        if (sim > bestSim) {
            bestSim = sim;
            bestSlot = i;
        }
    }

    // 3. Decision logic
    if (bestSlot >= 0 && bestSlot != activeSlot_ && bestSim > slotSwitchThreshold) {
        // INSTANT RECALL: previously-seen section returned
        activeSlot_ = bestSlot;
        // Refresh LRU ages
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (slots_[i].valid && i != activeSlot_) slots_[i].age++;
        }
        slots_[activeSlot_].age = 0;
        // Blend cached pattern into PLP (slotSeedBlend controls mix ratio)
        float tempPattern[MAX_PATTERN_LEN];
        resamplePattern(slots_[activeSlot_].bins, SLOT_BINS, tempPattern, plpPatternLen_);
        for (int j = 0; j < plpPatternLen_; j++) {
            plpPattern_[j] = tempPattern[j] * slotSeedBlend + plpPattern_[j] * (1.0f - slotSeedBlend);
        }
        // Confidence boost
        plpConfidence_ = fmaxf(plpConfidence_, slots_[activeSlot_].confidence * 0.8f);
        // Reset phase correction for fast re-convergence at new section
        phaseErrVar_ = 0.25f;

    } else if (bestSlot == activeSlot_ && bestSim > 0.50f && plpConfidence_ > slotSaveMinConf) {
        // REINFORCE active slot with current PLP pattern
        for (int i = 0; i < SLOT_BINS; i++) {
            slots_[activeSlot_].bins[i] =
                slots_[activeSlot_].bins[i] * (1.0f - slotUpdateRate) +
                currentDigest_[i] * slotUpdateRate;
        }
        slots_[activeSlot_].totalBars++;
        slots_[activeSlot_].confidence = plpConfidence_;

    } else if (bestSim < slotNewThreshold && plpConfidence_ > slotSaveMinConf) {
        // NEW SECTION: allocate a new slot
        int newSlot = allocateSlot();
        memcpy(slots_[newSlot].bins, currentDigest_, sizeof(currentDigest_));
        slots_[newSlot].confidence = plpConfidence_;
        slots_[newSlot].totalBars = 1;
        slots_[newSlot].valid = true;
        slots_[newSlot].age = 0;
        slots_[newSlot].seeded = false;
        // Age all other slots
        for (int i = 0; i < SLOT_COUNT; i++) {
            if (i != newSlot && slots_[i].valid) slots_[i].age++;
        }
        activeSlot_ = newSlot;

    } else if (activeSlot_ < 0 && plpConfidence_ > slotSaveMinConf) {
        // FIRST SLOT: no active slot yet, create one
        activeSlot_ = 0;
        memcpy(slots_[0].bins, currentDigest_, sizeof(currentDigest_));
        slots_[0].confidence = plpConfidence_;
        slots_[0].totalBars = 1;
        slots_[0].valid = true;
        slots_[0].age = 0;
        slots_[0].seeded = false;
    }

    // 4. Template seeding for new/young slots (one-shot, reuses SEED_TEMPLATES)
    if (activeSlot_ >= 0 && slots_[activeSlot_].valid && !slots_[activeSlot_].seeded &&
        slots_[activeSlot_].totalBars >= 1 && slots_[activeSlot_].totalBars <= 3) {
        float bestTemplateSim = 0.0f;
        int bestTemplate = -1;
        for (int t = 0; t < NUM_SEED_TEMPLATES; t++) {
            float sim = cosineSimilarity(slots_[activeSlot_].bins, SEED_TEMPLATES[t], SLOT_BINS);
            if (sim > bestTemplateSim) {
                bestTemplateSim = sim;
                bestTemplate = t;
            }
        }
        if (bestTemplate >= 0 && bestTemplateSim > 0.50f) {
            // Blend template into slot
            for (int i = 0; i < SLOT_BINS; i++) {
                slots_[activeSlot_].bins[i] =
                    0.5f * slots_[activeSlot_].bins[i] + 0.5f * SEED_TEMPLATES[bestTemplate][i];
            }
            // Also seed PLP pattern
            float tempPattern[MAX_PATTERN_LEN];
            resamplePattern(slots_[activeSlot_].bins, SLOT_BINS, tempPattern, plpPatternLen_);
            for (int j = 0; j < plpPatternLen_; j++) {
                plpPattern_[j] = tempPattern[j] * 0.5f + plpPattern_[j] * 0.5f;
            }
        }
        slots_[activeSlot_].seeded = true;  // One-shot: don't re-seed this slot
    }
}

// ============================================================================
// Output Synthesis
// ============================================================================

void AudioTracker::synthesizeOutputs(float dt, uint32_t nowMs) {
    // --- Energy ---
    // Hybrid: mic level + bass mel energy (cached) + ODF peak-hold
    float micLevel = mic_.getLevel();
    float rawEnergy = energyMicWeight * micLevel + energyMelWeight * cachedBassEnergy_ + energyOdfWeight * odfPeakHold_;

    // Beat-proximity energy boost via PLP pulse (replaces subdivision-aware proximity)
    rawEnergy *= (1.0f + plpPulseValue_ * 0.3f * plpConfidence_);

    // Pattern prediction via PLP pattern (replaces v77 bar histogram prediction)
    float patternPrediction = plpPulseValue_ * plpConfidence_;

    // Anticipatory energy: read PLP pattern at lookahead phase
    static constexpr float ANTICIPATION_GAIN = 0.1f;
    static constexpr float ANTICIPATION_LOOKAHEAD = 0.05f;
    if (ANTICIPATION_GAIN > 0.0f && plpPatternLen_ > 0 && plpConfidence_ > 0.2f) {
        float lookaheadPhase = plpPhase_ + ANTICIPATION_LOOKAHEAD;
        if (lookaheadPhase >= 1.0f) lookaheadPhase -= 1.0f;
        float patPos = lookaheadPhase * plpPatternLen_;
        int idx0 = static_cast<int>(patPos) % plpPatternLen_;
        float frac = patPos - floorf(patPos);
        int idx1 = (idx0 + 1) % plpPatternLen_;
        float lookaheadPulse = plpPattern_[idx0] * (1.0f - frac) + plpPattern_[idx1] * frac;
        rawEnergy += lookaheadPulse * ANTICIPATION_GAIN * plpConfidence_;
    }

    control_.energy = clampf(rawEnergy, 0.0f, 1.0f);

    // --- Pulse ---
    // Raw onset strength with pattern prediction boost.
    // PLP pulse (via phaseToPulse()) provides beat-synced breathing separately.
    float pulse = lastPulseStrength_;

    static constexpr float PATTERN_GAIN = 0.3f;
    if (pulse > 0.0f && patternPrediction > 0.3f) {
        pulse *= (1.0f + patternPrediction * PATTERN_GAIN);
    }

    control_.pulse = clampf(pulse, 0.0f, 1.0f);

    // --- Phase + PLP Pulse ---
    control_.phase = plpPhase_;
    control_.plpPulse = plpPulseValue_;

    // --- Rhythm Strength ---
    // PLP confidence can only boost, never drag down ACF periodicity.
    float strength = (plpConfidence_ > periodicityStrength_) ? plpConfidence_ : periodicityStrength_;

    // Active slot confidence can only boost
    if (activeSlot_ >= 0 && slots_[activeSlot_].valid) {
        float slotConf = slots_[activeSlot_].confidence;
        if (slotConf > strength) strength = slotConf;
    }

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
}
