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
static_assert(SLOT_BINS == SEED_TEMPLATE_BINS, "SLOT_BINS must match SEED_TEMPLATE_BINS for cosine similarity");
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

    // Initialize all time-based state to current time to avoid
    // instant triggers on first update (nowMs >> 0 would fire immediately)
    uint32_t now = time_.millis();
    onsetDensityWindowStart_ = now;
    lastSignificantAudioMs_ = now;   // Prevent premature silence decay on boot
    lastAcfMs_ = now;                // Prevent immediate ACF fire
    lastPulseMs_ = now;
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

    // 9. Periodic ACF for tempo estimation
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

    // 11. Decay during silence + reset stale state for clean warm-up
    if (nowMs - lastSignificantAudioMs_ > 2000) {
        periodicityStrength_ *= 0.998f;  // ~0.5s half-life at 62.5 Hz
    }
    // After 5s of silence, reset analysis state so new music starts clean.
    // Without this, old OSS/bass/NN buffers, pattern, and phase correction
    // state contaminate warm-up on the next track.
    if (nowMs - lastSignificantAudioMs_ > 5000 && ossCount_ > 0) {
        memset(ossBuffer_, 0, sizeof(ossBuffer_));
        memset(bassBuffer_, 0, sizeof(bassBuffer_));
        memset(nnOnsetBuffer_, 0, sizeof(nnOnsetBuffer_));
        ossWriteIdx_ = 0; ossCount_ = 0;
        bassWriteIdx_ = 0; bassCount_ = 0;
        nnWriteIdx_ = 0; nnCount_ = 0;
        memset(plpPattern_, 0, sizeof(plpPattern_));
        odfBaseline_ = 0.0f;
        odfPeakHold_ = 0.0f;
        phaseErrEma_ = 0.0f;
        phaseErrVar_ = 0.25f;  // Start fast convergence
        antiCorrRunCount_ = 0;
        plpConfidence_ = 0.0f;
        periodicityStrength_ = 0.0f;
        resetSlots();
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

    // --- Pass 1: Goertzel DFT scan to find top-N candidates ---
    // Collect top candidates by DFT magnitude across all sources.
    static constexpr int TOP_N = 5;
    struct Candidate {
        float mag;
        float phase;
        int period;
        int source;
    };
    Candidate topCandidates[TOP_N] = {};

    // Scan candidate periods from bpmMin to bpmMax
    int minLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMax);
    int maxLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMin);
    if (minLag < 10) minLag = 10;
    if (maxLag > MAX_PATTERN_LEN) maxLag = MAX_PATTERN_LEN;
    if (maxLag >= ossCount_ / 2) maxLag = ossCount_ / 2 - 1;

    for (int lag = minLag; lag <= maxLag; lag++) {
        float omega = TWO_PI_F / static_cast<float>(lag);
        float cosOmega = cosf(omega);
        float sinOmega = sinf(omega);
        float coeff = 2.0f * cosOmega;

        for (int src = 0; src < 3; src++) {
            int count = sourceCounts[src];
            if (count < lag * 2) continue;
            const float* buf = sources[src];

            float s1 = 0.0f, s2 = 0.0f;
            for (int i = 0; i < count; i++) {
                float s0 = buf[i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            float dftReal = s1 - s2 * cosOmega;
            float dftImag = s2 * sinOmega;
            float mag = sqrtf(dftReal * dftReal + dftImag * dftImag) / sqrtf(static_cast<float>(count));

            // Insert into top-N if large enough.
            // Enforce minimum 10% period separation to prevent near-duplicate
            // candidates (e.g., lag 32/33/34 from different sources) from
            // crowding out genuinely different tempo hypotheses.
            bool tooClose = false;
            for (int k = 0; k < TOP_N; k++) {
                if (topCandidates[k].mag > 0.01f &&
                    abs(lag - topCandidates[k].period) <= topCandidates[k].period / 10) {
                    // Near-duplicate: keep the stronger one
                    if (mag > topCandidates[k].mag) {
                        float rawPhase = -atan2f(dftImag, dftReal) / TWO_PI_F;
                        float phaseAdvance = static_cast<float>(count - 1) / static_cast<float>(lag);
                        float phase = rawPhase + phaseAdvance;
                        phase -= floorf(phase);
                        topCandidates[k] = { mag, phase, lag, src };
                    }
                    tooClose = true;
                    break;
                }
            }
            if (!tooClose) {
                int minIdx = 0;
                for (int k = 1; k < TOP_N; k++) {
                    if (topCandidates[k].mag < topCandidates[minIdx].mag) minIdx = k;
                }
                if (mag > topCandidates[minIdx].mag) {
                    float rawPhase = -atan2f(dftImag, dftReal) / TWO_PI_F;
                    float phaseAdvance = static_cast<float>(count - 1) / static_cast<float>(lag);
                    float phase = rawPhase + phaseAdvance;
                    phase -= floorf(phase);
                    topCandidates[minIdx] = { mag, phase, lag, src };
                }
            }
        }
    }

    // --- Pass 2: Score candidates by epoch-fold pattern quality ---
    // The period that produces the highest-contrast pattern is the one that
    // creates the best visual pulse, regardless of which has higher DFT magnitude.
    // Combined score: DFT magnitude (periodicity) × pattern variance (visual quality).
    float bestScore = -1.0f;
    float bestMag = 0.0f;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    float bestPhase = 0.0f;
    int bestSource = 0;

    for (int k = 0; k < TOP_N; k++) {
        if (topCandidates[k].mag < 0.01f) continue;  // Skip empty slots

        int cPeriod = topCandidates[k].period;
        int cSource = topCandidates[k].source;
        const float* cBuf = sources[cSource];
        int cCount = sourceCounts[cSource];

        if (cCount < cPeriod * 2) continue;

        // Epoch-fold at this candidate period with recency weighting
        float fold[MAX_PATTERN_LEN] = {0};
        float totalWeight = 0.0f;
        int epochs = 0;
        for (int offset = cCount - cPeriod; offset >= 0; offset -= cPeriod) {
            float weight = expf(-0.3f * epochs);  // ~3-epoch half-life
            for (int j = 0; j < cPeriod; j++) {
                fold[j] += cBuf[offset + j] * weight;
            }
            totalWeight += weight;
            epochs++;
        }
        if (epochs < 2) continue;
        for (int j = 0; j < cPeriod; j++) fold[j] /= totalWeight;

        // Pattern variance (contrast) — higher = more structured
        float mean = 0.0f;
        for (int j = 0; j < cPeriod; j++) mean += fold[j];
        mean /= cPeriod;
        float variance = 0.0f;
        for (int j = 0; j < cPeriod; j++) {
            float d = fold[j] - mean;
            variance += d * d;
        }
        variance /= cPeriod;

        // Combined score: DFT magnitude × pattern variance
        // Both are important: DFT confirms periodicity, variance confirms visual quality
        float score = topCandidates[k].mag * sqrtf(variance);

        if (score > bestScore) {
            bestScore = score;
            bestMag = topCandidates[k].mag;
            bestPeriod = topCandidates[k].period;
            bestPhase = topCandidates[k].phase;
            bestSource = topCandidates[k].source;
        }
    }

    plpDftMag_ = bestMag;

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
    // --- 1. Use raw period from Fourier tempogram (not BPM-smoothed) ---
    int patLen = plpBestPeriod_;
    if (patLen < 2) patLen = 2;
    if (patLen > MAX_PATTERN_LEN) patLen = MAX_PATTERN_LEN;
    plpPatternLen_ = patLen;

    // --- 2. Recency-weighted epoch fold (fixed grid from buffer end) ---
    // The fold grid is anchored to the buffer end (most recent data). This is
    // stable across ACF cycles — the same absolute time positions always map to
    // the same pattern bins. Phase alignment is handled separately by the
    // pattern-peak correction in step 4 (adjusts plpPhase_, not the fold grid).
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

    float patternAccum[MAX_PATTERN_LEN] = {0};
    float totalWeight = 0.0f;
    int epochs = 0;
    for (int offset = sourceCount - patLen; offset >= 0; offset -= patLen) {
        float weight = expf(-0.3f * epochs);  // ~3-epoch half-life
        for (int j = 0; j < patLen; j++) {
            patternAccum[j] += sourceBuf[offset + j] * weight;
        }
        totalWeight += weight;
        epochs++;
    }
    if (epochs < 2) return;

    // Normalize to [0, 1] using min-max
    float minVal = patternAccum[0], maxVal = patternAccum[0];
    for (int j = 0; j < patLen; j++) {
        patternAccum[j] /= totalWeight;
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

    // --- 3. Anti-correlation detection + half-period correction ---
    // Cross-correlate the pattern against the most recent raw epoch at lag 0
    // and lag patLen/2. Only rotate if: (a) corrAtZero is clearly negative AND
    // (b) corrAtHalf is clearly positive. This avoids false triggers when both
    // correlations are near zero (noise) or both negative (genuinely flat pattern).
    //
    // If anti-correlation persists for 4+ consecutive ACF cycles (~600ms),
    // force a half-period phase shift to break out of wrong phase locks.
    {
        int recentStart = sourceCount - patLen;
        float corrAtZero = 0.0f, corrAtHalf = 0.0f;
        int halfPat = patLen / 2;
        for (int j = 0; j < patLen; j++) {
            float raw = sourceBuf[recentStart + j];
            corrAtZero += plpPattern_[j] * raw;
            corrAtHalf += plpPattern_[(j + halfPat) % patLen] * raw;
        }

        // Normalize by pattern energy for scale-independent comparison
        float patEnergy = 0.0f;
        for (int j = 0; j < patLen; j++) patEnergy += plpPattern_[j] * plpPattern_[j];
        if (patEnergy > 1e-10f) {
            float invE = 1.0f / sqrtf(patEnergy);
            corrAtZero *= invE;
            corrAtHalf *= invE;
        }

        // Only rotate when corrAtZero is significantly negative AND
        // corrAtHalf is significantly positive — clear phase inversion.
        static constexpr float ANTI_CORR_THRESH = 0.05f;
        if (corrAtZero < -ANTI_CORR_THRESH && corrAtHalf > ANTI_CORR_THRESH) {
            // Clear phase inversion: rotate pattern and shift phase
            float tmp[MAX_PATTERN_LEN];
            memcpy(tmp, plpPattern_, patLen * sizeof(float));
            for (int j = 0; j < patLen; j++) {
                plpPattern_[j] = tmp[(j + halfPat) % patLen];
            }
            plpPhase_ += 0.5f;
            if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;
            antiCorrRunCount_ = 0;  // Reset: we just corrected, don't double-shift
        } else if (corrAtZero < -ANTI_CORR_THRESH) {
            // Negative but no clear half-period improvement: count toward persistent reset
            antiCorrRunCount_++;
            if (antiCorrRunCount_ >= 4) {
                plpPhase_ += 0.5f;
                if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;
                phaseErrVar_ = 0.25f;  // Force fast re-convergence
                antiCorrRunCount_ = 0;
            }
        } else {
            antiCorrRunCount_ = 0;
        }
    }

    // --- 4. Phase alignment: pattern-peak with DFT fallback ---
    float phaseError = 0.0f;
    {
        int peakIdx = 0;
        float peakVal = plpPattern_[0];
        for (int j = 1; j < patLen; j++) {
            if (plpPattern_[j] > peakVal) { peakVal = plpPattern_[j]; peakIdx = j; }
        }
        if (peakVal > 0.5f) {
            float peakPhase = static_cast<float>(peakIdx) / static_cast<float>(patLen);
            phaseError = peakPhase - plpPhase_;
        } else {
            phaseError = plpDftPhase_ - plpPhase_;
        }
    }

    // Wrap error to [-0.5, 0.5]
    if (phaseError > 0.5f) phaseError -= 1.0f;
    if (phaseError < -0.5f) phaseError += 1.0f;

    // Adaptive correction rate from phase error variance (EMA)
    phaseErrEma_ += PHASE_ALPHA_ERR * (phaseError - phaseErrEma_);
    float deviation = phaseError - phaseErrEma_;
    phaseErrVar_ += PHASE_ALPHA_ERR * (deviation * deviation - phaseErrVar_);

    float sigma = sqrtf(phaseErrVar_ > 0.0f ? phaseErrVar_ : 0.0f);
    float lambda = sigma / (sigma + PHASE_SIGMA_REF);
    float alpha = PHASE_ALPHA_MIN + (PHASE_ALPHA_MAX - PHASE_ALPHA_MIN) * lambda;

    plpPhase_ += alpha * phaseError;
    if (plpPhase_ < 0.0f) plpPhase_ += 1.0f;
    if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;

    // --- 5. PLP confidence from DFT magnitude + steep signal gate ---
    float dftConf = clampf(plpDftMag_, 0.0f, 1.0f);
    float micLevel = mic_.getLevel();
    float signalPresence = clampf((micLevel - plpSignalFloor * 0.5f) / (plpSignalFloor * 0.5f), 0.0f, 1.0f);
    float targetConf = dftConf * signalPresence;
    plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;
}

void AudioTracker::updatePlpPhase() {
    // Free-running phase advance at detected period (not BPM-smoothed)
    int period = (plpBestPeriod_ > 0) ? plpBestPeriod_ : 33;
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
    if (plpPhase_ < 0.05f || plpPhase_ > 0.95f) {
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
    // Floor-tracking baseline:
    // Slow rise (peaks don't inflate baseline), fast drop (floor drops caught quickly).
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

    } else if (bestSlot == activeSlot_ && bestSim > slotNewThreshold && plpConfidence_ > slotSaveMinConf) {
        // REINFORCE active slot with current PLP pattern
        for (int i = 0; i < SLOT_BINS; i++) {
            slots_[activeSlot_].bins[i] =
                slots_[activeSlot_].bins[i] * (1.0f - slotUpdateRate) +
                currentDigest_[i] * slotUpdateRate;
        }
        slots_[activeSlot_].totalBars++;
        slots_[activeSlot_].confidence = plpConfidence_;
        slots_[activeSlot_].age = 0;  // Keep active slot fresh for LRU

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
            // Blend template into slot (use slotSeedBlend for consistency with recall path)
            float seedBlend = slotSeedBlend;
            for (int i = 0; i < SLOT_BINS; i++) {
                slots_[activeSlot_].bins[i] =
                    (1.0f - seedBlend) * slots_[activeSlot_].bins[i] + seedBlend * SEED_TEMPLATES[bestTemplate][i];
            }
            // Also seed PLP pattern
            float tempPattern[MAX_PATTERN_LEN];
            resamplePattern(slots_[activeSlot_].bins, SLOT_BINS, tempPattern, plpPatternLen_);
            for (int j = 0; j < plpPatternLen_; j++) {
                plpPattern_[j] = tempPattern[j] * seedBlend + plpPattern_[j] * (1.0f - seedBlend);
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
