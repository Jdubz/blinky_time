#include "AudioTracker.h"
#include <math.h>
#include <string.h>

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PULSE_ONSET_FLOOR=0.1

static constexpr float TWO_PI_F = 6.28318530718f;

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

        // Track raw NN activation peaks (before pulse detection threshold/cooldown).
        // Record timestamp when activation exceeds previous value (rising edge).
        if (odf > rawNNActivation_ && odf > 0.3f) {
            rawNNPeakMs_ = nowMs;
        }
        rawNNActivation_ = odf;
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

        // NN-weighted flux: emphasize consistent rhythmic elements (kicks/snares)
        // and suppress variable ornamental elements (hi-hats, fills). The NN fires
        // primarily on kicks and snares (F1=0.716), naturally biasing the epoch-fold
        // toward the stable rhythmic skeleton. Soft gate preserves some non-NN energy.
        float nnGatedFlux = fluxContrast * (0.3f + 0.7f * clampf(odf, 0.0f, 1.0f));
        addOssSample(nnGatedFlux);

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
        memset(pulseBuf_, 0, sizeof(pulseBuf_));
        olaPeakEma_ = 1.0f;
        odfBaseline_ = 0.0f;
        odfPeakHold_ = 0.0f;
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

    // Step size: 1 frame for short periods (beat-level), 2 for longer periods
    // (bar-level). Halves Goertzel scan cost for the extended range without
    // losing resolution where it matters. At lag=66, 1-frame step = 0.9 BPM
    // resolution; at lag=132, 2-frame step = 0.4 BPM resolution (still fine).
    for (int lag = minLag; lag <= maxLag; lag += (lag < 66 ? 1 : 2)) {
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

    // Clear pulse buffer on significant period change (>10% shift)
    // so old kernels at the wrong frequency don't contaminate the new period.
    if (abs(bestPeriod - plpBestPeriod_) > plpBestPeriod_ / 10) {
        memset(pulseBuf_, 0, sizeof(pulseBuf_));
        olaPeakEma_ = 1.0f;
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

    // --- 2. Recency-weighted epoch fold for PATTERN DIGEST (slot cache only) ---
    // Epoch-fold is NOT used for pulse output (cosine OLA handles that).
    // The pattern digest captures the actual rhythmic shape for section detection
    // via the slot cache's cosine similarity matching.
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

    // Variance-gated epoch fold: track both sum and sum-of-squares per bin.
    // Bins with low cross-epoch variance are consistent (kick on beat 1 always
    // lands here). Bins with high variance are unreliable (hi-hat pattern changes
    // bar-to-bar). Weight each bin inversely by its variance.
    // (Stellingwerf 1978 Phase Dispersion Minimization, adapted for music)
    float foldSum[MAX_PATTERN_LEN] = {0};
    float foldSumSq[MAX_PATTERN_LEN] = {0};
    float totalWeight = 0.0f;
    int epochs = 0;
    for (int offset = sourceCount - patLen; offset >= 0; offset -= patLen) {
        float weight = expf(-0.3f * epochs);
        for (int j = 0; j < patLen; j++) {
            float val = sourceBuf[offset + j];
            foldSum[j] += val * weight;
            foldSumSq[j] += val * val * weight;
        }
        totalWeight += weight;
        epochs++;
    }
    if (epochs < 2) return;

    // Compute per-bin mean, variance, and reliability-weighted pattern
    static constexpr float VARIANCE_SENSITIVITY = 10.0f;  // Higher = more aggressive suppression
    float minVal = 1e30f, maxVal = -1e30f;
    float patternRaw[MAX_PATTERN_LEN];
    for (int j = 0; j < patLen; j++) {
        float mean = foldSum[j] / totalWeight;
        float meanSq = foldSumSq[j] / totalWeight;
        float variance = meanSq - mean * mean;
        if (variance < 0.0f) variance = 0.0f;  // Numerical safety

        // Reliability: consistent bins (low variance) get full weight,
        // variable bins (high variance) get suppressed
        float reliability = 1.0f / (1.0f + variance * VARIANCE_SENSITIVITY);
        patternRaw[j] = mean * reliability;

        if (patternRaw[j] < minVal) minVal = patternRaw[j];
        if (patternRaw[j] > maxVal) maxVal = patternRaw[j];
    }

    // Normalize to [0, 1] using min-max
    float range = maxVal - minVal;
    if (range > 1e-10f) {
        for (int j = 0; j < patLen; j++) {
            float normalized = (patternRaw[j] - minVal) / range;
            plpPattern_[j] = (plpNovGain != 1.0f) ? powf(normalized, plpNovGain) : normalized;
        }
    } else {
        for (int j = 0; j < patLen; j++) plpPattern_[j] = 0.0f;
    }

    // --- 3. Canonical PLP: add Hann-windowed cosine kernel to pulse buffer ---
    // Each ACF update contributes one kernel. The buffer rolls forward 1 position
    // per frame in updatePlpPhase(). Overlap-add of many kernels produces peaks
    // where the DFT says periodicity is — anti-correlation is impossible because
    // cosine peaks are always positively correlated with the dominant periodic
    // component. (Grosche & Mueller 2011, Meier et al. 2024)
    //
    // Kernel: k(t) = hann(t) * cos(2*pi*(t * omega - phase))
    //   omega = 1/period (cycles per frame)
    //   phase = DFT phase at winning frequency (Meier 2024 eq. 3)
    //   Window length = 2 * period (covers 2 full beat cycles)
    {
        float omega = 1.0f / static_cast<float>(patLen);  // cycles per frame
        int halfWin = patLen;  // 1 period each side = 2-period window
        if (halfWin > PULSE_BUF_LEN / 2 - 1) halfWin = PULSE_BUF_LEN / 2 - 1;
        int winLen = halfWin * 2 + 1;

        // Kernel is centered at position 0 (current frame = buffer position 0).
        // Negative indices address past samples, positive address future predictions.
        // The Hann window tapers both ends smoothly.
        for (int i = -halfWin; i <= halfWin; i++) {
            float w = 0.5f + 0.5f * cosf(TWO_PI_F * static_cast<float>(i) / static_cast<float>(winLen - 1));
            float kernel = w * cosf(TWO_PI_F * (static_cast<float>(i) * omega - plpDftPhase_));
            int idx = i;  // Relative to current position (0 = now)
            if (idx < 0) idx += PULSE_BUF_LEN;
            if (idx >= PULSE_BUF_LEN) idx -= PULSE_BUF_LEN;
            pulseBuf_[idx] += kernel;
        }
    }

    // --- 4. PLP confidence from DFT magnitude + steep signal gate ---
    float dftConf = clampf(plpDftMag_, 0.0f, 1.0f);
    float micLevel = mic_.getLevel();
    float signalPresence = clampf((micLevel - plpSignalFloor * 0.5f) / (plpSignalFloor * 0.5f), 0.0f, 1.0f);
    float targetConf = dftConf * signalPresence;
    plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;
}

void AudioTracker::updatePlpPhase() {
    // --- Roll pulse buffer forward by 1 frame (Meier 2024 real-time PLP) ---
    // Shift all values left by 1; zero the tail. This advances the "now" cursor
    // so position 0 always represents the current frame. Old kernel contributions
    // naturally fall off the left end — no explicit decay needed.
    memmove(pulseBuf_, pulseBuf_ + 1, (PULSE_BUF_LEN - 1) * sizeof(float));
    pulseBuf_[PULSE_BUF_LEN - 1] = 0.0f;

    // --- Read pulse from cosine OLA buffer at position 0 (current frame) ---
    // Half-wave rectify: only positive lobes become pulse peaks (Grosche & Mueller 2011).
    float rawPulse = fmaxf(pulseBuf_[0], 0.0f);

    // Normalize with slow-tracking peak EMA to keep output in [0, 1].
    // EMA avoids hard peak-hold artifacts. Floor prevents division issues during silence.
    static constexpr float PEAK_EMA_ALPHA = 0.01f;   // ~100-frame (~1.5s) time constant
    static constexpr float PEAK_FLOOR = 0.1f;
    olaPeakEma_ += PEAK_EMA_ALPHA * (rawPulse - olaPeakEma_);
    if (olaPeakEma_ < PEAK_FLOOR) olaPeakEma_ = PEAK_FLOOR;
    float normalizedPulse = clampf(rawPulse / olaPeakEma_, 0.0f, 1.0f);

    // Blend OLA pulse with cosine fallback when confidence is low.
    // During silence, confidence→0 and the output degrades to a smooth cosine.
    float cosinePulse = 0.5f + 0.5f * cosf(plpPhase_ * TWO_PI_F);
    float blend = clampf(plpConfidence_, 0.0f, 1.0f);
    plpPulseValue_ = cosinePulse * (1.0f - blend) + normalizedPulse * blend;

    // --- Free-running phase advance (for beat counting + cosine fallback) ---
    int period = (plpBestPeriod_ > 0) ? plpBestPeriod_ : 33;
    float phaseIncrement = 1.0f / static_cast<float>(period);
    plpPhase_ += phaseIncrement;
    if (plpPhase_ >= 1.0f) {
        plpPhase_ -= 1.0f;
        beatCount_++;
    }

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
