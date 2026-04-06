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
        // Uses pulseNNGate as floor so the peak timestamp stays fresh whenever
        // activation is in the gate-open range (Gemini/Copilot review feedback).
        if (odf > rawNNActivation_ && odf > pulseNNGate) {
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

    // 5. Pulse detection uses spectral flux for sharp transient edges, gated
    //    by NN onset activation to suppress non-onset spectral changes (harmonic
    //    shifts, hi-hat jitter, noise). Spectral flux provides precise timing
    //    (rising-edge detection), NN provides selectivity (is this a real onset?).
    float pulseOdf = newSpectralFrame ? spectral_.getSpectralFlux() : prevOdf_;
    updatePulseDetection(pulseOdf, dt, nowMs);

    // 6. Feed DSP components only on new spectral frames.
    if (newSpectralFrame) {
        // Bass-only flux for ACF period detection — isolates kick energy.
        // Hi-hats/snares in the high band create non-periodic ACF peaks that
        // overwhelm the kick pattern (especially in breakbeat/DnB).
        float bassFlux = spectral_.getBassFlux();
        float bassContrast;
        if (odfContrast == 2.0f) {
            bassContrast = bassFlux * bassFlux;
        } else if (odfContrast == 1.0f) {
            bassContrast = bassFlux;
        } else if (odfContrast == 0.5f) {
            bassContrast = sqrtf(bassFlux);
        } else {
            bassContrast = powf(bassFlux, odfContrast);
        }
        bassContrast = clampf(bassContrast, 0.0f, 1.0f);

        // NN-gated bass flux for epoch-fold pattern extraction.
        // Soft gate emphasizes kicks (NN F1=0.787) and suppresses fills,
        // biasing the epoch-fold toward the stable rhythmic skeleton.
        float nnGatedFlux = bassContrast * (0.3f + 0.7f * clampf(odf, 0.0f, 1.0f));
        addOssSample(bassContrast, nnGatedFlux);

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

    // 7. ACF period detection + PLP pattern analysis on timer (~4ms per run;
    // Warmup: 100 frames (~1.6s). Minimum is ACF_MAX_LAG (80) for valid correlation;
    // 100 gives 1.25x the max lag — tight but sufficient for first ACF.
    if (newSpectralFrame && (nowMs - lastAcfMs_) >= acfPeriodMs && ossCount_ >= 100) {
        uint32_t t0 = time_.millis();
        runAcf();
        uint32_t t1 = time_.millis();
        updatePlpAnalysis();
        uint32_t t2 = time_.millis();
        lastAcfMs_ = nowMs;
        lastAcfDurationMs_ = t1 - t0;  // Profile: ACF only
        lastPlpMs_ = t2 - t1;          // Profile: PLP analysis only
    }

    // 8. PLP phase update (free-running + pattern-based correction)
    updatePlpPhase();

    // 11. Decay during silence + reset stale state for clean warm-up
    if (nowMs - lastSignificantAudioMs_ > 2000) {
        periodicityStrength_ *= 0.998f;  // ~0.5s half-life at 62.5 Hz
    }
    // After 5s of silence, reset analysis state so new music starts clean.
    // Without this, old OSS/bass/NN buffers, pattern, and phase correction
    // state contaminate warm-up on the next track.
    if (nowMs - lastSignificantAudioMs_ > 5000 && ossCount_ > 0) {
        resetAnalysisState();
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
// State Reset
// ============================================================================

void AudioTracker::resetAnalysisState() {
    memset(ossBuffer_, 0, sizeof(ossBuffer_));
    memset(gatedFluxBuffer_, 0, sizeof(gatedFluxBuffer_));
    memset(bassBuffer_, 0, sizeof(bassBuffer_));
    memset(nnOnsetBuffer_, 0, sizeof(nnOnsetBuffer_));
    ossWriteIdx_ = 0; ossCount_ = 0;
    bassWriteIdx_ = 0; bassCount_ = 0;
    nnWriteIdx_ = 0; nnCount_ = 0;
    memset(plpPattern_, 0, sizeof(plpPattern_));
    memset(pulseBuf_, 0, sizeof(pulseBuf_));
    pulseHead_ = 0;
    olaPeakEma_ = 1.0f;
    odfBaseline_ = 0.0f;
    odfPeakHold_ = 0.0f;
    plpConfidence_ = 0.0f;
    periodicityStrength_ = 0.0f;
    plpPhase_ = 0.0f;
    beatCount_ = 0;
    resetSlots();
}

// ============================================================================
// OSS Buffer
// ============================================================================

void AudioTracker::addOssSample(float ungatedFlux, float gatedFlux) {
    ossBuffer_[ossWriteIdx_] = ungatedFlux;
    gatedFluxBuffer_[ossWriteIdx_] = gatedFlux;
    ossWriteIdx_ = (ossWriteIdx_ + 1) % OSS_BUFFER_SIZE;
    if (ossCount_ < OSS_BUFFER_SIZE) ossCount_++;
}

// ============================================================================
// Multi-Source ACF — Period Detection + Epoch-Fold Pattern Scoring
// ============================================================================
// Replaces the Fourier Tempogram (Goertzel DFT, 75ms) with autocorrelation
// (~4ms). ACF scans beat-level lags (20-80) on 3 sources, then generates
// bar-level candidates via integer multiples (2×/3×/4×). Epoch-fold variance
// scoring selects the period with the best pattern contrast, biasing toward
// full-bar patterns over single-beat periods.

void AudioTracker::runAcf() {
    // Linearize all source circular buffers into class members.
    int startIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    for (int i = 0; i < ossCount_; i++) {
        ossLinear_[i] = ossBuffer_[(startIdx + i) % OSS_BUFFER_SIZE];
        gatedFluxLinear_[i] = gatedFluxBuffer_[(startIdx + i) % OSS_BUFFER_SIZE];
    }
    int bStart = (bassWriteIdx_ - bassCount_ + BASS_BUFFER_SIZE) % BASS_BUFFER_SIZE;
    for (int i = 0; i < bassCount_; i++) bassLinear_[i] = bassBuffer_[(bStart + i) % BASS_BUFFER_SIZE];
    int nStart = (nnWriteIdx_ - nnCount_ + NN_BUFFER_SIZE) % NN_BUFFER_SIZE;
    for (int i = 0; i < nnCount_; i++) nnLinear_[i] = nnOnsetBuffer_[(nStart + i) % NN_BUFFER_SIZE];

    // Mean-subtract each source IN-PLACE (removes DC for clean ACF/epoch-fold).
    // NOTE: updatePlpAnalysis() reads these mean-subtracted buffers.
    float* sources[3] = { ossLinear_, bassLinear_, nnLinear_ };
    const int sourceCounts[3] = { ossCount_, bassCount_, nnCount_ };

    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < 20) continue;
        float mean = 0.0f;
        for (int i = 0; i < count; i++) mean += sources[src][i];
        mean /= count;
        for (int i = 0; i < count; i++) sources[src][i] -= mean;
    }
    // Also mean-subtract gatedFluxLinear_ for epoch-fold use
    if (ossCount_ >= 20) {
        float mean = 0.0f;
        for (int i = 0; i < ossCount_; i++) mean += gatedFluxLinear_[i];
        mean /= ossCount_;
        for (int i = 0; i < ossCount_; i++) gatedFluxLinear_[i] -= mean;
    }

    // --- Pass 1: Multi-source ACF at beat-level lags ---
    // Scan lags 20-80 (200-49 BPM beat rate) with step=2.
    // Bar-level candidates are generated from multiples, not scanned directly.
    // Cost: ~31 lags × 3 sources × ~700 overlap ≈ 65K MACs (~4ms on M4F).
    // Parabolic interpolation on peaks recovers sub-step resolution (odd lags).
    static constexpr int TOP_N = 5;
    static constexpr int ACF_MIN_LAG = 20;    // ~200 BPM beat rate
    static constexpr int ACF_MAX_LAG = 80;    // ~49 BPM beat rate
    static constexpr int ACF_STEP = 2;

    struct AcfPeak {
        float strength;   // Normalized ACF value at peak (0-1)
        int period;
        int source;
    };
    AcfPeak beatPeaks[TOP_N] = {};

    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < ACF_MAX_LAG * 2) continue;
        const float* buf = sources[src];

        // Zero-lag energy for normalization (unbiased ACF)
        float zeroLag = 0.0f;
        for (int i = 0; i < count; i++) zeroLag += buf[i] * buf[i];
        if (zeroLag < 1e-10f) continue;

        // Scan ACF with local-maximum peak detection.
        // Extend one step past ACF_MAX_LAG so a peak AT the boundary is detected
        // (the 3-point detector looks one step back).
        int scanEnd = min(ACF_MAX_LAG + ACF_STEP, count / 2 - 1);
        float prevAcf = 0.0f, prevPrevAcf = 0.0f;
        for (int lag = ACF_MIN_LAG; lag <= scanEnd; lag += ACF_STEP) {
            float sum = 0.0f;
            int overlap = count - lag;
            for (int i = 0; i < overlap; i++) {
                sum += buf[i] * buf[i + lag];
            }
            float acfVal = sum / zeroLag;

            // Peak: previous sample was a local maximum above threshold
            // Only record if the peak lag is within the valid scan range
            if (prevAcf > prevPrevAcf && prevAcf > acfVal && prevAcf > 0.05f
                && (lag - ACF_STEP) >= ACF_MIN_LAG && (lag - ACF_STEP) <= ACF_MAX_LAG) {
                int peakLag = lag - ACF_STEP;
                float peakStrength = prevAcf;

                // Parabolic interpolation: refine peak position and strength
                // from 3 equidistant ACF samples (step=2 → recovers odd lags)
                float denom = prevPrevAcf - 2.0f * prevAcf + acfVal;
                if (fabsf(denom) > 1e-10f) {
                    float delta = 0.5f * (prevPrevAcf - acfVal) / denom;
                    if (delta > -1.0f && delta < 1.0f) {
                        int refined = peakLag + static_cast<int>(roundf(delta * ACF_STEP));
                        if (refined >= ACF_MIN_LAG && refined <= ACF_MAX_LAG) {
                            peakLag = refined;
                        }
                        peakStrength = prevAcf - 0.25f * (prevPrevAcf - acfVal) * delta;
                    }
                }

                // Insert into top-N with 10% minimum period separation
                bool tooClose = false;
                for (int k = 0; k < TOP_N; k++) {
                    if (beatPeaks[k].strength > 0.01f &&
                        abs(peakLag - beatPeaks[k].period) <= beatPeaks[k].period / 10) {
                        if (peakStrength > beatPeaks[k].strength) {
                            beatPeaks[k] = { peakStrength, peakLag, src };
                        }
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) {
                    int minIdx = 0;
                    for (int k = 1; k < TOP_N; k++) {
                        if (beatPeaks[k].strength < beatPeaks[minIdx].strength) minIdx = k;
                    }
                    if (peakStrength > beatPeaks[minIdx].strength) {
                        beatPeaks[minIdx] = { peakStrength, peakLag, src };
                    }
                }
            }
            prevPrevAcf = prevAcf;
            prevAcf = acfVal;
        }
    }

    // --- Generate bar-level candidates from beat-peak multiples ---
    // Bar patterns are 2×/3×/4× the beat period. Epoch-fold variance scoring
    // selects bar-length periods over beat-length when the pattern is structured
    // (e.g., kick-snare-kick-snare has higher variance at 4× than at 1×).
    static constexpr int MULTIPLIERS[] = { 1, 2, 3, 4 };
    static constexpr int NUM_MULTIPLIERS = 4;
    static constexpr int MAX_CANDIDATES = TOP_N * NUM_MULTIPLIERS;

    struct ScoredCandidate {
        float acfStrength;
        int period;
        int source;
        int multiplier;   // 1=beat, 2/3/4=bar-level
    };
    static_assert(MAX_CANDIDATES == TOP_N * NUM_MULTIPLIERS, "MAX_CANDIDATES invariant");
    ScoredCandidate candidates[MAX_CANDIDATES] = {};
    int numCandidates = 0;

    for (int k = 0; k < TOP_N; k++) {
        if (beatPeaks[k].strength < 0.01f) continue;
        for (int m = 0; m < NUM_MULTIPLIERS; m++) {
            int cPeriod = beatPeaks[k].period * MULTIPLIERS[m];
            if (cPeriod > MAX_PATTERN_LEN) continue;
            if (cPeriod >= ossCount_ / 2) continue;  // Need 2+ epochs

            // Skip near-duplicates
            bool dup = false;
            for (int c = 0; c < numCandidates; c++) {
                if (abs(cPeriod - candidates[c].period) <= cPeriod / 10) { dup = true; break; }
            }
            if (dup) continue;

            if (numCandidates < MAX_CANDIDATES) {
                candidates[numCandidates++] = { beatPeaks[k].strength, cPeriod, beatPeaks[k].source, MULTIPLIERS[m] };
            }
        }
    }

    // --- Pass 2: Score candidates by epoch-fold pattern quality ---
    // Reuses tempogram Pass 2 logic: the period that produces the highest-contrast
    // pattern is the one that creates the best visual pulse. Each candidate is
    // scored using its own source buffer for fair cross-source comparison.
    float bestScore = -1.0f;
    float bestStrength = 0.0f;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    float bestPhase = 0.0f;
    int bestSource = 0;

    for (int c = 0; c < numCandidates; c++) {
        int cPeriod = candidates[c].period;
        int cSource = candidates[c].source;
        const float* cBuf = sources[cSource];
        int cCount = sourceCounts[cSource];

        if (cCount < cPeriod * 2) continue;

        // Recency-weighted epoch fold
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

        // Combined score: ACF periodicity × pattern contrast.
        // Multiplier penalty: higher multipliers (2×/3×/4×) must produce
        // proportionally more variance to beat the base period. Without this,
        // 4× bar periods always win because they capture more structure.
        // Penalty: divide by sqrt(multiplier) — 2× needs 1.41× more contrast,
        // 4× needs 2× more contrast to win over the beat-level period.
        float multiplierPenalty = sqrtf(static_cast<float>(candidates[c].multiplier));
        float score = candidates[c].acfStrength * sqrtf(variance) / multiplierPenalty;

        if (score > bestScore) {
            // Phase from epoch-fold peak position (where the main accent falls).
            // Parabolic interpolation on the 3 bins around the peak gives sub-frame
            // precision (~0.3% vs ~3% integer-bin at 120 BPM). Same technique used
            // in the ACF peak finder. Handles wrap-around for peaks at bin 0.
            int peakBin = 0;
            float peakVal = fold[0];
            for (int j = 1; j < cPeriod; j++) {
                if (fold[j] > peakVal) { peakVal = fold[j]; peakBin = j; }
            }
            // Parabolic interpolation for sub-frame phase
            float refinedBin = static_cast<float>(peakBin);
            if (cPeriod >= 3) {
                int prev = (peakBin - 1 + cPeriod) % cPeriod;  // wrap-around
                int next = (peakBin + 1) % cPeriod;
                float denom = fold[prev] - 2.0f * fold[peakBin] + fold[next];
                if (fabsf(denom) > 1e-6f) {
                    float delta = 0.5f * (fold[prev] - fold[next]) / denom;
                    delta = clampf(delta, -0.5f, 0.5f);
                    refinedBin += delta;
                    if (refinedBin < 0.0f) refinedBin += cPeriod;
                }
            }

            bestScore = score;
            bestStrength = candidates[c].acfStrength;
            bestPeriod = cPeriod;
            bestPhase = refinedBin / static_cast<float>(cPeriod);
            bestSource = cSource;
        }
    }

    acfPeakStrength_ = bestStrength;

    // Clear pulse buffer on significant period change (>10% shift)
    if (abs(bestPeriod - plpBestPeriod_) > plpBestPeriod_ / 10) {
        memset(pulseBuf_, 0, sizeof(pulseBuf_));
        pulseHead_ = 0;
        olaPeakEma_ = 1.0f;
        // Hard-reset free-running phase to match detected accent (OLA buffer is
        // empty so output is 100% cosine fallback — must align immediately)
        plpPhase_ = 1.0f - bestPhase;
        if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;
    }

    plpBestPeriod_ = bestPeriod;
    plpBestSource_ = static_cast<uint8_t>(bestSource);
    plpAccentPhase_ = bestPhase;

    // Correct free-running phase toward detected accent phase.
    // Target: plpPhase_ = 1 - accentPhase (so cosine fallback peaks when
    // the OLA kernel peak arrives, i.e. accentPhase * period frames from now).
    // Correction rate scales with confidence: reliable estimates get faster
    // correction, noisy estimates get ignored. At ~100ms ACF period and
    // rate 0.25, convergence takes ~400ms (4 updates) at full confidence.
    {
        float target = 1.0f - bestPhase;
        if (target >= 1.0f) target -= 1.0f;
        float error = target - plpPhase_;
        if (error > 0.5f) error -= 1.0f;
        if (error < -0.5f) error += 1.0f;
        float rate = phaseCorrectRate * clampf(plpConfidence_, 0.0f, 1.0f);
        plpPhase_ += error * rate;
        if (plpPhase_ < 0.0f) plpPhase_ += 1.0f;
        if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;
    }

    // Periodicity strength from ACF peak (already normalized 0-1)
    float newStrength = clampf(bestStrength, 0.0f, 1.0f);
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
    // --- 1. Use raw period from ACF (not BPM-smoothed) ---
    int patLen = plpBestPeriod_;
    if (patLen < 2) patLen = 2;
    if (patLen > MAX_PATTERN_LEN) patLen = MAX_PATTERN_LEN;
    plpPatternLen_ = patLen;

    // --- 2. Recency-weighted epoch fold for PATTERN DIGEST (slot cache only) ---
    // Epoch-fold is NOT used for pulse output (cosine OLA handles that).
    // The pattern digest captures the actual rhythmic shape for section detection
    // via the slot cache's cosine similarity matching.
    // Use NN-gated flux (gatedFluxLinear_) for source 0 to emphasize kicks/snares
    // in the pattern shape. The ungated ossLinear_ is reserved for ACF
    // period detection (NN-independent per architecture contract).
    const float* sourceBuf = gatedFluxLinear_;
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
    // where the ACF says periodicity is — anti-correlation is impossible because
    // cosine peaks are always positively correlated with the dominant periodic
    // component. (Grosche & Mueller 2011, Meier et al. 2024)
    //
    // Kernel: k(t) = hann(t) * cos(2*pi*(t * omega - phase))
    //   omega = 1/period (cycles per frame)
    //   phase = epoch-fold accent position (0-1 within period)
    //   Window length = 2 * period (covers 2 full beat cycles)
    {
        float omega = 1.0f / static_cast<float>(patLen);  // cycles per frame
        int halfWin = patLen;  // 1 period each side = 2-period window
        if (halfWin > PULSE_BUF_LEN / 2 - 1) halfWin = PULSE_BUF_LEN / 2 - 1;
        int winLen = halfWin * 2 + 1;

        // Kernel is centered at pulseHead_ (current frame in ring buffer).
        // Negative indices address past samples, positive address future predictions.
        // The Hann window tapers both ends smoothly.
        for (int i = -halfWin; i <= halfWin; i++) {
            float w = 0.5f + 0.5f * cosf(TWO_PI_F * static_cast<float>(i) / static_cast<float>(winLen - 1));
            float kernel = w * cosf(TWO_PI_F * (static_cast<float>(i) * omega - plpAccentPhase_));
            int idx = (pulseHead_ + i) % PULSE_BUF_LEN;
            if (idx < 0) idx += PULSE_BUF_LEN;
            pulseBuf_[idx] += kernel;
        }
    }

    // --- 4. PLP confidence from ACF strength + steep signal gate ---
    float acfConf = clampf(acfPeakStrength_, 0.0f, 1.0f);
    float micLevel = mic_.getLevel();
    float signalPresence = clampf((micLevel - plpSignalFloor * 0.5f) / (plpSignalFloor * 0.5f), 0.0f, 1.0f);
    float targetConf = acfConf * signalPresence;
    plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;
}

void AudioTracker::updatePlpPhase() {
    // --- Advance pulse ring buffer by 1 frame (Meier 2024 real-time PLP) ---
    // Read current value, then zero and advance head. Ring buffer avoids
    // the O(N) memmove (~3KB/frame at 62.5Hz) of a linear shift.
    // Old kernel contributions naturally fall off as head wraps around.

    // --- Read pulse from cosine OLA buffer at head (current frame) ---
    // Half-wave rectify: only positive lobes become pulse peaks (Grosche & Mueller 2011).
    float rawPulse = fmaxf(pulseBuf_[pulseHead_], 0.0f);

    // Zero the slot we just read and advance head
    pulseBuf_[pulseHead_] = 0.0f;
    pulseHead_ = (pulseHead_ + 1) % PULSE_BUF_LEN;

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

    // Pulse detection: fire on the RISING EDGE of NN activation crossing threshold.
    // This fires at the start of the transient (the leading edge), not at the peak.
    // Gives 16-48ms earlier detection compared to peak-based triggering.
    // (Brossier 2006: predictive onset detection fires on the rising slope)
    float pulseThreshold = odfBaseline_ * pulseThresholdMult;
    if (pulseThreshold < pulseOnsetFloor) pulseThreshold = pulseOnsetFloor;

    // Tempo-adaptive cooldown: shorter at faster tempos
    float bpmNorm = clampf((bpm_ - 60.0f) / 140.0f, 0.0f, 1.0f);
    float cooldownMs = 40.0f + 110.0f * (1.0f - bpmNorm);

    // Guard against ms wraparound
    if (nowMs < lastPulseMs_) lastPulseMs_ = nowMs;

    // Rising-edge detection: fire when ODF crosses threshold from below while rising.
    // prevOdf_ < threshold AND odf > threshold = the leading edge of a transient.
    bool risingEdge = (odf > pulseThreshold) && (prevOdf_ <= pulseThreshold);

    // Gate on spectral activity (not mic level — mic level normalization can
    // read 0 despite active audio, killing all pulse detection).
    float signalPresence = max(odfPeakHold_, cachedBassEnergy_);

    // NN gate: suppress spectral flux pulses that aren't corroborated by the
    // onset NN. Two conditions must be met:
    //   1. Activation above threshold (filters noise/silence)
    //   2. Activation peaked recently — was rising within ~100ms (filters
    //      sustained high activation from harmonic content, reverb tails)
    // When NN is not active (fallback mode), gate is always open.
    // Guard against millis() wrap (~49 days) — treat as stale peak
    uint32_t peakAge = (nowMs >= rawNNPeakMs_) ? (nowMs - rawNNPeakMs_) : 200;
    bool nnGateOpen = !nnActive_ || pulseNNGate <= 0.0f ||
        (rawNNActivation_ > pulseNNGate && peakAge < 100);

    float pulseStrength = 0.0f;
    if (signalPresence > pulseMinLevel &&
        risingEdge &&
        nnGateOpen &&
        (nowMs - lastPulseMs_) > static_cast<uint32_t>(cooldownMs)) {
        pulseStrength = clampf(odf, 0.0f, 1.0f);

        // Sub-frame interpolation: estimate the precise threshold crossing time
        // between the previous frame and this frame. Reduces quantization jitter
        // from ±16ms (frame rate) to ~±2ms. Standard technique in spectral peak
        // detection (McAulay-Quatieri), applied here to onset timing.
        float framePeriodMs = 1000.0f / OSS_FRAME_RATE;  // ~16ms
        float denom = odf - prevOdf_;
        float frac = (denom > 1e-6f) ? (pulseThreshold - prevOdf_) / denom : 0.5f;
        frac = clampf(frac, 0.0f, 1.0f);
        lastPulseMs_ = nowMs - static_cast<uint32_t>((1.0f - frac) * framePeriodMs);
    }
    prevOdf_ = odf;
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
    // ========================================================================
    // Clean 3-signal audio API for generators:
    //
    //   energy    (0-1)  Normalized mic amplitude. Auto-ranged to fill 0-1
    //                    regardless of absolute volume. THE primary signal.
    //
    //   pulse     (0-1)  Onset envelope. Spikes to ~1.0 on transients (kicks,
    //                    snares), decays exponentially. Triggers visual events.
    //
    //   plpPulse  (0-1)  Breathing curve. Repeating pattern shape at detected
    //                    tempo. Cosine fallback when no rhythm detected.
    //
    // Generators consume ONLY these signals. They don't know about mic levels,
    // FFT, BPM, or detection internals.
    // ========================================================================

    // --- Energy: weighted blend of mic, bass mel, and ODF peak-hold ---
    // Three complementary signals: broadband mic level (overall loudness),
    // bass mel energy (low-frequency punch), ODF peak-hold (transient accent).
    // Smolder floor: deterministic noise in silence for baseline ember activity.
    float micEnergy = mic_.getLevel();
    float bassEnergy = clampf(cachedBassEnergy_, 0.0f, 1.0f);
    float odfEnergy = clampf(odfPeakHold_, 0.0f, 1.0f);
    float weightedEnergy = micEnergy * energyMicWeight
                         + bassEnergy * energyMelWeight
                         + odfEnergy * energyOdfWeight;
    // Smolder: hash-based deterministic noise (0.12-0.20) for baseline activity
    uint32_t h = nowMs;
    h ^= h >> 16; h *= 0x45d9f3bU; h ^= h >> 16;
    float smolder = 0.12f + 0.08f * (h & 0xFF) * (1.0f / 255.0f);
    control_.energy = clampf(max(weightedEnergy, smolder), 0.0f, 1.0f);

    // --- Pulse: onset envelope from spectral flux ---
    // Auto-normalize flux by its own recent peak, then trigger a decaying
    // envelope. No hardcoded thresholds — adapts to any mic sensitivity.
    float flux = spectral_.getSpectralFlux();
    // Track running peak of flux (frame-rate-independent ~10s time constant)
    if (flux > fluxPeak_) fluxPeak_ = flux;
    fluxPeak_ *= expf(-dt / 10.0f);  // ~7s half-life (tau=10s)
    if (fluxPeak_ < 0.001f) fluxPeak_ = 0.001f;

    // Normalized flux: 0-1 relative to recent peak
    float normFlux = clampf(flux / fluxPeak_, 0.0f, 1.0f);

    // Trigger envelope: any normalized flux above 0.4 pushes the envelope up
    if (normFlux > 0.4f) {
        float trigger = (normFlux - 0.4f) / 0.6f;  // 0-1 above threshold
        if (trigger > pulseEnvelope_) {
            pulseEnvelope_ = trigger;
        }
    }
    // Frame-rate-independent decay (~165ms half-life, tau=0.24s)
    pulseEnvelope_ *= expf(-dt / 0.24f);
    if (pulseEnvelope_ < 0.01f) pulseEnvelope_ = 0.0f;

    control_.pulse = clampf(pulseEnvelope_, 0.0f, 1.0f);

    // --- Phase + PLP Pulse: breathing curve ---
    // Free-running phase at detected or default tempo.
    // plpPulseValue_ is set by updatePlpPhase() — cosine fallback when
    // no rhythm pattern is available.
    control_.phase = plpPhase_;
    control_.plpPulse = plpPulseValue_;

    // --- Rhythm Strength ---
    // Driven by ACF periodicity strength. Enables music-reactive mode in
    // Water/Lightning generators when periodic rhythm is detected.
    control_.rhythmStrength = periodicityStrength_;

    // --- Onset Density ---
    if (control_.pulse > 0.1f && prevPulseOutput_ <= 0.1f) {
        onsetCountInWindow_++;
    }
    prevPulseOutput_ = control_.pulse;
    if (nowMs - onsetDensityWindowStart_ >= 1000) {
        float newDensity = static_cast<float>(onsetCountInWindow_);
        onsetDensity_ = onsetDensity_ * 0.7f + newDensity * 0.3f;
        onsetCountInWindow_ = 0;
        onsetDensityWindowStart_ = nowMs;
    }
    control_.onsetDensity = onsetDensity_;
}
