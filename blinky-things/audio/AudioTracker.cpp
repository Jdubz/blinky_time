#include "AudioTracker.h"
#include <math.h>
#include <string.h>

// Max ACF lag array size: 4 * (OSS_FRAMES_PER_MIN / bpmMin) = 4 * 66 = 264
// Subtract minLag (~20) + 1 = 245. Round up for safety.
static constexpr int MAX_ACF_SIZE = 280;

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PULSE_ONSET_FLOOR=0.1

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Static constexpr member definitions (required in C++14; remove if upgrading to C++17)
constexpr float AudioTracker::CACHE_BLEND_RATES[4];

// ============================================================================
// Pattern Template Bank (8 templates, ~544 bytes flash)
// ============================================================================
// Each template has 16 bins (16th-note positions in a 4/4 bar).
// fillMask: bit i set = position i is fill-prone (ignored in core matching).
// Bins represent relative onset density (normalized to sum ~1.0).

const PatternTemplate AudioTracker::templates_[AudioTracker::NUM_TEMPLATES] = {
    // 0: "4otf" — four-on-the-floor (kick on every quarter note: 0,4,8,12)
    {{0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f,
      0.25f,0.0f,0.0f,0.0f, 0.25f,0.0f,0.0f,0.0f},
     0x8888},  // fills at positions 3,7,11,15 (upbeat 16ths before kicks)

    // 1: "backbeat" — kick on 1,3 + snare on 2,4 (bins 0,4,8,12)
    // but emphasis on 0,8 (kick) and 4,12 (snare)
    {{0.20f,0.0f,0.0f,0.0f, 0.30f,0.0f,0.0f,0.0f,
      0.20f,0.0f,0.0f,0.0f, 0.30f,0.0f,0.0f,0.0f},
     0x8888},

    // 2: "halftime" — kick on 1, snare on 3 (bins 0, 8)
    {{0.35f,0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f,0.0f,
      0.35f,0.0f,0.0f,0.0f, 0.15f,0.0f,0.0f,0.15f},
     0x6060},  // fills at positions 5,6,13,14

    // 3: "breakbeat" — syncopated kick + snare (funk/hip-hop)
    {{0.20f,0.0f,0.0f,0.15f, 0.0f,0.0f,0.20f,0.0f,
      0.10f,0.0f,0.0f,0.0f, 0.15f,0.0f,0.20f,0.0f},
     0x0808},  // fills at positions 3,11

    // 4: "8thnote" — straight 8th notes (hi-hat pulse on every 8th)
    {{0.15f,0.0f,0.10f,0.0f, 0.15f,0.0f,0.10f,0.0f,
      0.15f,0.0f,0.10f,0.0f, 0.15f,0.0f,0.10f,0.0f},
     0x0000},  // no typical fills

    // 5: "dnb" — drum and bass (fast kick + snare on 2, 160-180 BPM)
    {{0.25f,0.0f,0.10f,0.0f, 0.0f,0.0f,0.0f,0.0f,
      0.30f,0.0f,0.0f,0.10f, 0.0f,0.0f,0.15f,0.10f},
     0x0808},

    // 6: "dembow" — syncopated dancehall/reggaeton
    {{0.20f,0.0f,0.0f,0.15f, 0.0f,0.0f,0.20f,0.0f,
      0.0f,0.0f,0.0f,0.15f, 0.20f,0.0f,0.0f,0.10f},
     0x0000},

    // 7: "sparse" — ambient/minimal (kick on 1, sparse ghost notes)
    {{0.50f,0.0f,0.0f,0.0f, 0.0f,0.0f,0.0f,0.0f,
      0.10f,0.0f,0.0f,0.0f, 0.20f,0.0f,0.0f,0.20f},
     0x6666},  // fills at positions 1,2,5,6,9,10,13,14
};

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

    bool nnOk = frameOnsetNN_.begin();
    nnActive_ = nnOk && frameOnsetNN_.isReady();

    // Initialize time-based state to current time to avoid
    // instant flush on first update (nowMs >> 0 would fire immediately)
    uint32_t now = time_.millis();
    onsetDensityWindowStart_ = now;
    lastBarBoundaryMs_ = now;

    return true;
}

void AudioTracker::end() {
    mic_.end();
}

int AudioTracker::getHwGain() const {
    return mic_.getHwGain();
}

int AudioTracker::getCacheEntryCount() const {
    int count = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache_[i].valid) count++;
    }
    return count;
}

void AudioTracker::resetPatternMemory() {
    memset(ioiBins_, 0, sizeof(ioiBins_));
    memset(barBins_, 0, sizeof(barBins_));
    memset(prevGoodBins_, 0, sizeof(prevGoodBins_));
    memset(onsetTimes_, 0, sizeof(onsetTimes_));
    onsetBufCount_ = 0;
    onsetWriteIdx_ = 0;
    ioiPeakMs_ = 500.0f;
    ioiPeakBpm_ = 120.0f;
    ioiPeakStrength_ = 0.0f;
    ioiConfidence_ = 0.0f;
    barEntropy_ = 1.0f;
    patternConfidence_ = 0.0f;
    prevPatternConfidence_ = 0.0f;
    patternBarsAccumulated_ = 0;
    lastBarBoundaryMs_ = time_.millis();
    bestTemplateIdx_ = -1;
    bestTemplateSim_ = 0.0f;
    for (int i = 0; i < CACHE_SIZE; i++) {
        cache_[i].valid = false;
    }
    cacheRestoreActive_ = false;
    cacheRestoreBarsLeft_ = 0;
    cacheRestoreIdx_ = -1;
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
        // NN active but no new spectral frame — skip OSS/comb update
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

    // 6. ODF information gate — removed (v76).

    // 7-8. Feed DSP components only on new spectral frames.
    //      BPM estimation uses spectral flux (NN-independent broadband transient
    //      signal) — not NN onset activation. NN onset drives visual pulse only.
    if (newSpectralFrame) {
        float flux = spectral_.getSpectralFlux();
        // Apply contrast sharpening (power-law) before buffering.
        // Squaring sharpens peaks relative to baseline, improving ACF.
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

        // Clamp contrast-sharpened flux to [0, 1] before feeding DSP components.
        // Spectral flux can exceed 1.0 if band weights are set high; squaring
        // amplifies the overshoot and degrades ACF normalization.
        fluxContrast = clampf(fluxContrast, 0.0f, 1.0f);

        combFilterBank_.feedbackGain = combFeedback;
        combFilterBank_.process(fluxContrast);
        addOssSample(fluxContrast);

        // Bass energy for PLP dual-source analysis
        float bassEnergy = 0.0f;
        const float* mel = spectral_.getMelBands();
        if (mel) {
            for (int i = 1; i <= 6; i++) bassEnergy += mel[i];
            bassEnergy /= 6.0f;
        }
        addBassSample(bassEnergy);
    }

    // 9. Periodic ACF for tempo estimation + IOI analysis
    //    First ACF fires after ~1s (ossCount_ >= 60 at ~66 Hz), then every
    //    acfPeriodMs (~150ms = ~9 frames). The 60-sample minimum ensures
    //    enough OSS data for meaningful autocorrelation.
    if (ossCount_ >= 60 && (nowMs - lastAcfMs_ >= acfPeriodMs)) {
        lastAcfMs_ = nowMs;
        runAutocorrelation();
        updatePlpAnalysis();  // PLP epoch-fold + bass ACF + cross-correlate

        // IOI analysis runs once per ACF cycle (same cadence)
        if (patternEnabled) {
            updateIoiAnalysis();
        }
    }

    // 10. PLP phase update (free-running + pattern-based correction)
    updatePlpPhase();

    // 11. Decay periodicity during silence (between ACF runs)
    if (nowMs - lastSignificantAudioMs_ > 2000) {
        periodicityStrength_ *= 0.998f;  // ~0.5s half-life at 62.5 Hz
    }

    // 12. Pattern memory: decay bins + bar stats
    //    (IOI analysis runs in the ACF block above, once per cycle)
    if (patternEnabled) {
        decayPatternBins();

        // Bar-boundary stats: phase-accumulating boundary (avoids drift)
        uint32_t currentBarMs = (uint32_t)(ioiPeakMs_ * 4.0f);
        if (currentBarMs > 0 && nowMs - lastBarBoundaryMs_ >= currentBarMs) {
            lastBarBoundaryMs_ += currentBarMs;
            // Re-sync if accumulator drifted too far (tempo change or startup).
            // Behind: more than half a bar late. Ahead: BPM was overestimated.
            if (nowMs - lastBarBoundaryMs_ > currentBarMs / 2
                || lastBarBoundaryMs_ > nowMs + currentBarMs / 2) {
                lastBarBoundaryMs_ = nowMs;
            }
            computePatternStats();
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

        // Compute Rayleigh weight directly per lag (no comb bin lookup).
        // Early-exit when weight contribution is negligible (expf(-20) ≈ 2e-9);
        // avoids unnecessary expf() calls for lags far outside the Rayleigh peak.
        float expArg = -lagF * lagF / (2.0f * rayleighSigma2);
        if (expArg < -20.0f) continue;
        float rayleighW = (lagF / rayleighSigma2) * expf(expArg);

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

    // IOI advisory nudge: when the IOI histogram has a confident peak that
    // disagrees with ACF, blend toward IOI (max 30% influence). This helps
    // break the ~135 BPM gravity well by providing onset-interval evidence
    // independent of spectral flux periodicity.
    //
    // Octave disambiguation: fold IOI BPM to nearest octave of ACF BPM before
    // nudging. Prevents subdivisions (8th/16th notes) from pulling BPM to 2x/4x.
    if (patternEnabled && ioiConfidence_ > 0.65f) {
        float foldedIoiBpm = ioiPeakBpm_;
        // Fold down: if IOI is at 4x, quarter it; if 2x, halve it
        while (foldedIoiBpm > newBpm * 1.5f && foldedIoiBpm > bpmMin) {
            foldedIoiBpm *= 0.5f;
        }
        // Fold up: if IOI is at 0.5x (half-note), double it
        while (foldedIoiBpm < newBpm * 0.667f && foldedIoiBpm * 2.0f < bpmMax) {
            foldedIoiBpm *= 2.0f;
        }

        float ioiDiff = fabsf(foldedIoiBpm - newBpm) / newBpm;
        if (ioiDiff > 0.10f) {
            // Cap nudge at 15% unless very high confidence (>0.8 → up to 24%)
            float maxWeight = (ioiConfidence_ > 0.8f) ? 0.3f : 0.15f;
            float ioiWeight = fminf(ioiConfidence_ * 0.3f, maxWeight);
            newBpm = newBpm * (1.0f - ioiWeight) + foldedIoiBpm * ioiWeight;
        }
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
// PLP (Predominant Local Pulse) — Dual-Source Pattern Extraction
// ============================================================================

void AudioTracker::addBassSample(float bassEnergy) {
    bassBuffer_[bassWriteIdx_] = bassEnergy;
    bassWriteIdx_ = (bassWriteIdx_ + 1) % BASS_BUFFER_SIZE;
    if (bassCount_ < BASS_BUFFER_SIZE) bassCount_++;
}

void AudioTracker::updatePlpAnalysis() {
    // --- 1. Compute pattern length from current BPM ---
    int patLen = static_cast<int>(beatPeriodFrames_ + 0.5f);
    if (patLen < 2) patLen = 2;
    if (patLen > MAX_PATTERN_LEN) patLen = MAX_PATTERN_LEN;
    plpPatternLen_ = patLen;

    // Need at least 2 full periods in OSS buffer for meaningful epoch folding
    if (ossCount_ < patLen * 2) return;

    // --- 2. Epoch-fold OSS buffer to extract average pattern ---
    // Linearize circular buffer (reuse static from runAutocorrelation)
    static float ossLinear[OSS_BUFFER_SIZE];
    int startIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    for (int i = 0; i < ossCount_; i++) {
        ossLinear[i] = ossBuffer_[(startIdx + i) % OSS_BUFFER_SIZE];
    }

    // Fold and average
    float patternAccum[MAX_PATTERN_LEN] = {0};
    int epochs = 0;
    for (int offset = ossCount_ - patLen; offset >= 0; offset -= patLen) {
        for (int j = 0; j < patLen; j++) {
            patternAccum[j] += ossLinear[offset + j];
        }
        epochs++;
    }

    if (epochs < 2) return;

    // Normalize to [0, 1], then apply contrast via power-law
    float maxVal = 0.0f;
    for (int j = 0; j < patLen; j++) {
        patternAccum[j] /= epochs;
        if (patternAccum[j] > maxVal) maxVal = patternAccum[j];
    }
    if (maxVal > 0.0f) {
        for (int j = 0; j < patLen; j++) {
            float normalized = patternAccum[j] / maxVal;
            // plpNovGain as contrast exponent: >1 sharpens peaks, <1 flattens
            plpPattern_[j] = (plpNovGain != 1.0f) ? powf(normalized, plpNovGain) : normalized;
        }
    }

    // --- 3. Bass ACF for dual-source period agreement ---
    if (bassCount_ >= patLen * 2) {
        // Linearize bass buffer
        static float bassLinear[BASS_BUFFER_SIZE];
        int bStart = (bassWriteIdx_ - bassCount_ + BASS_BUFFER_SIZE) % BASS_BUFFER_SIZE;
        for (int i = 0; i < bassCount_; i++) {
            bassLinear[i] = bassBuffer_[(bStart + i) % BASS_BUFFER_SIZE];
        }

        // Bass mean subtraction
        float bassMean = 0.0f;
        for (int i = 0; i < bassCount_; i++) bassMean += bassLinear[i];
        bassMean /= bassCount_;

        // Bass ACF — find dominant lag in [minLag, maxLag]
        // Normalized by zero-lag autocorrelation for fair comparison across lags
        int minLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMax);
        int maxLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMin);
        if (maxLag >= bassCount_ / 2) maxLag = bassCount_ / 2 - 1;
        if (minLag < 1) minLag = 1;

        // Zero-lag autocorrelation (= variance * count) for normalization
        float bassZeroLag = 0.0f;
        for (int i = 0; i < bassCount_; i++) {
            float d = bassLinear[i] - bassMean;
            bassZeroLag += d * d;
        }

        float bestBassCorr = -1e30f;
        int bestBassLag = minLag;
        if (bassZeroLag > 1e-10f) {
            for (int lag = minLag; lag <= maxLag && lag < bassCount_; lag++) {
                float sum = 0.0f;
                int n = bassCount_ - lag;
                for (int i = 0; i < n; i++) {
                    sum += (bassLinear[i] - bassMean) * (bassLinear[i + lag] - bassMean);
                }
                // Normalize by zero-lag to get [-1, 1] range
                float normalized = sum / bassZeroLag;
                if (normalized > bestBassCorr) {
                    bestBassCorr = normalized;
                    bestBassLag = lag;
                }
            }
        }
        plpBassPeriod_ = static_cast<float>(bestBassLag);
        float bassStrength = clampf(bestBassCorr, 0.0f, 1.0f);

        // --- 4. Dual-source agreement → PLP confidence ---
        float fluxBpm = bpm_;
        float bassBpm = OSS_FRAMES_PER_MIN / plpBassPeriod_;

        // Check agreement: within 10%, or at octave (2x / 0.5x)
        float ratio = bassBpm / fluxBpm;
        float agreement = 0.0f;
        if (ratio > 0.9f && ratio < 1.1f) {
            agreement = 1.0f;  // Direct agreement
        } else if (ratio > 1.8f && ratio < 2.2f) {
            agreement = 0.7f;  // Octave above
        } else if (ratio > 0.45f && ratio < 0.55f) {
            agreement = 0.7f;  // Octave below
        }

        // Factor in both ACF strengths: flux periodicity AND bass periodicity
        float fluxStrength = clampf(periodicityStrength_, 0.0f, 1.0f);
        float targetConf = agreement * fluxStrength * bassStrength;

        // EMA smooth confidence
        plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;
    }

    // --- 5. Cross-correlate last period with extracted pattern for phase alignment ---
    if (ossCount_ >= patLen) {
        float bestCorr = -1e30f;
        int bestOffset = 0;
        for (int offset = 0; offset < patLen; offset++) {
            float sum = 0.0f;
            for (int j = 0; j < patLen; j++) {
                int sigIdx = ossCount_ - patLen + j;
                int patIdx = (j + offset) % patLen;
                sum += ossLinear[sigIdx] * plpPattern_[patIdx];
            }
            if (sum > bestCorr) {
                bestCorr = sum;
                bestOffset = offset;
            }
        }

        // Convert offset to target phase
        float measuredPhase = static_cast<float>(bestOffset) / static_cast<float>(patLen);

        // Gentle proportional phase correction (10% per update, ~150ms cadence)
        float phaseError = measuredPhase - plpPhase_;
        if (phaseError > 0.5f) phaseError -= 1.0f;
        if (phaseError < -0.5f) phaseError += 1.0f;
        plpPhase_ += 0.1f * phaseError;
        if (plpPhase_ < 0.0f) plpPhase_ += 1.0f;
        if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;
    }
}

void AudioTracker::updatePlpPhase() {
    // Free-running phase advance at current BPM
    float phaseIncrement = 1.0f / beatPeriodFrames_;
    plpPhase_ += phaseIncrement;

    // Beat wrap
    if (plpPhase_ >= 1.0f) {
        plpPhase_ -= 1.0f;
        beatCount_++;
    }

    // Read extracted pattern at current phase position (linear interpolation)
    if (plpPatternLen_ > 0 && plpConfidence_ > plpActivation) {
        float patPos = plpPhase_ * plpPatternLen_;
        int idx0 = static_cast<int>(patPos) % plpPatternLen_;
        float frac = patPos - floorf(patPos);
        int idx1 = (idx0 + 1) % plpPatternLen_;
        plpPulseValue_ = plpPattern_[idx0] * (1.0f - frac) + plpPattern_[idx1] * frac;
    } else {
        // Fallback: cosine pulse (same shape as old phaseToPulse)
        plpPulseValue_ = 0.5f + 0.5f * cosf(plpPhase_ * 6.28318530718f);
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

        // Record onset for pattern memory (Phase A: IOI, Phase B: bar histogram)
        recordOnsetForPattern(pulseStrength, nowMs);
    }
    lastPulseStrength_ = pulseStrength;
}

// ============================================================================
// Pattern Memory (v77): IOI histogram + bar-position prediction
// ============================================================================

void AudioTracker::recordOnsetForPattern(float strength, uint32_t nowMs) {
    if (!patternEnabled) return;

    // Store timestamp in circular buffer
    onsetTimes_[onsetWriteIdx_] = nowMs;
    onsetWriteIdx_ = (onsetWriteIdx_ + 1) % ONSET_BUF_SIZE;
    if (onsetBufCount_ < ONSET_BUF_SIZE) onsetBufCount_++;

    // Phase A: compute IOIs against recent onsets (not just previous one)
    // This captures intervals at multiple subdivision levels
    int maxBack = (onsetBufCount_ < 8) ? onsetBufCount_ - 1 : 8;
    for (int back = 1; back <= maxBack; back++) {
        // onsetWriteIdx_ was just incremented, so -1 is the slot just written,
        // -1-back is 'back' slots before it in the circular buffer.
        int prevIdx = (onsetWriteIdx_ - 1 - back + ONSET_BUF_SIZE) % ONSET_BUF_SIZE;
        float ioiMs = (float)(nowMs - onsetTimes_[prevIdx]);
        if (ioiMs >= IOI_MIN_MS && ioiMs <= IOI_MAX_MS) {
            int bin = (int)((ioiMs - IOI_MIN_MS) * IOI_BINS / (IOI_MAX_MS - IOI_MIN_MS));
            bin = (bin < 0) ? 0 : (bin >= IOI_BINS ? IOI_BINS - 1 : bin);
            ioiBins_[bin] += strength * patternLearnRate;
        }
    }

    // Phase B: update bar histogram (only when IOI is confident)
    updateBarHistogram(strength, nowMs);
}

void AudioTracker::updateIoiAnalysis() {
    // Triangular smoothing: 3-bin kernel (0.25, 0.5, 0.25) on stack-local copy.
    // 512 bytes stack, runs every 150ms (same path as ACF's 1.1KB — they don't overlap).
    float smoothed[IOI_BINS];
    smoothed[0] = ioiBins_[0] * 0.75f + ioiBins_[1] * 0.25f;
    for (int i = 1; i < IOI_BINS - 1; i++) {
        smoothed[i] = ioiBins_[i - 1] * 0.25f + ioiBins_[i] * 0.5f + ioiBins_[i + 1] * 0.25f;
    }
    smoothed[IOI_BINS - 1] = ioiBins_[IOI_BINS - 2] * 0.25f + ioiBins_[IOI_BINS - 1] * 0.75f;

    // Find strongest IOI peak in the 250-1000ms range (60-240 BPM)
    float bestVal = 0;
    int bestBin = -1;
    for (int i = IOI_BEAT_LOW; i <= IOI_BEAT_HIGH; i++) {
        if (smoothed[i] > bestVal) {
            bestVal = smoothed[i];
            bestBin = i;
        }
    }

    if (bestBin >= 0 && bestVal > 0.01f) {
        // Parabolic interpolation: refine peak with sub-bin precision (~1-2ms vs ~12ms bin width)
        float binWidth = (IOI_MAX_MS - IOI_MIN_MS) / IOI_BINS;
        float peakBinMs = IOI_MIN_MS + bestBin * binWidth;

        if (bestBin > IOI_BEAT_LOW && bestBin < IOI_BEAT_HIGH) {
            float left = smoothed[bestBin - 1];
            float center = smoothed[bestBin];
            float right = smoothed[bestBin + 1];
            float denom = left - 2.0f * center + right;
            if (fabsf(denom) > 1e-6f) {
                float offset = 0.5f * (left - right) / denom;
                offset = clampf(offset, -0.5f, 0.5f);
                peakBinMs += offset * binWidth;
            }
        }

        // EMA smoothing: prevents bar histogram smearing from IOI jumps between bins.
        // 30% new, 70% old — histogram accumulation already provides smoothing.
        ioiPeakMs_ = ioiPeakMs_ * 0.7f + peakBinMs * 0.3f;
        ioiPeakBpm_ = 60000.0f / ioiPeakMs_;
        ioiPeakStrength_ = bestVal;

        // Check agreement with ACF BPM (within 10% or at octave)
        float acfBeatPeriodMs = 60000.0f / bpm_;
        float ratio = ioiPeakMs_ / acfBeatPeriodMs;
        bool agrees = (fabsf(ratio - 1.0f) < 0.10f) ||
                      (fabsf(ratio - 0.5f) < 0.10f) ||
                      (fabsf(ratio - 2.0f) < 0.10f);

        if (agrees) {
            ioiConfidence_ = fminf(ioiConfidence_ + 0.05f, 1.0f);
        } else {
            ioiConfidence_ *= 0.93f;  // Symmetric with ramp: ~1.5s to decay from 0.5 to 0.25
        }
    } else {
        ioiConfidence_ *= 0.96f;  // Gentle decay when no peak found
    }
}

void AudioTracker::updateBarHistogram(float strength, uint32_t nowMs) {
    if (ioiConfidence_ < 0.5f) return;  // Phase B inactive until Phase A confident

    // Only accumulate strong onsets (kicks/snares) into the bar histogram.
    // Hi-hats/cymbals fire at every 8th/16th note position, making the
    // histogram uniform and destroying pattern structure. Kicks and snares
    // are the events that define phrasing (4otf, backbeat, halftime, etc.).
    if (strength < histogramMinStrength) return;

    // Project onset onto bar grid using IOI peak as beat period.
    // NOTE: This is a time-based approximation, not beat-synchronized.
    // Under tempo drift, the histogram smears across bins until the new
    // tempo stabilizes and the old bins decay.
    float barPeriodMs = ioiPeakMs_ * 4.0f;
    float elapsed = (float)(nowMs - lastBarBoundaryMs_);
    float barPhase = fmodf(elapsed / barPeriodMs, 1.0f);
    int bin = (int)(barPhase * BAR_BINS) % BAR_BINS;

    barBins_[bin] = barBins_[bin] * (1.0f - patternLearnRate) + strength * patternLearnRate;
}

void AudioTracker::computePatternStats() {
    if (patternBarsAccumulated_ < 0xFFFF) patternBarsAccumulated_++;  // Saturate to avoid wrap

    // Step 1: Progressive cache blend (if restore in progress)
    cacheProgressiveBlend();

    // Step 2: Shannon entropy of bar histogram (normalized to [0, 1]).
    // Note: barEntropy_ is no longer used in the confidence pipeline (replaced by
    // peak-to-mean ratio in Step 4), but is retained for serial diagnostics
    // (getBarEntropy() is streamed via SerialConsole).
    float sum = 0;
    for (int i = 0; i < BAR_BINS; i++) sum += barBins_[i];
    if (sum < 1e-6f) {
        barEntropy_ = 1.0f;
        patternConfidence_ *= 0.9f;
        prevPatternConfidence_ = patternConfidence_;
        return;
    }

    float H = 0;
    for (int i = 0; i < BAR_BINS; i++) {
        float p = barBins_[i] / sum;
        if (p > 1e-6f) H -= p * log2f(p);
    }
    barEntropy_ = H / 4.0f;  // log2(16) = 4.0

    // Step 3: Template matching
    matchTemplates();

    // Step 4: Confidence update — peak-to-mean ratio measures pattern structure.
    // Entropy was too sensitive: even 5x variation between bins gave entropy ~0.97,
    // killing confidence. Peak-to-mean directly measures how peaked the histogram
    // is vs uniform noise: 1.0 = flat (no pattern), 4.0+ = strong peaks (kick/snare).
    float maxBin = 0.0f;
    float meanBin = sum / BAR_BINS;
    for (int i = 0; i < BAR_BINS; i++) {
        if (barBins_[i] > maxBin) maxBin = barBins_[i];
    }
    float peakToMean = (meanBin > 1e-6f) ? (maxBin / meanBin) : 0.0f;
    // Map peak-to-mean [1, 4] → target [0, 1]. At 1x (uniform) target=0,
    // at 4x+ (strong beats) target=1. Typical 4otf pattern peaks at 2-3x.
    float target = clampf((peakToMean - 1.0f) / 3.0f, 0.0f, 1.0f);
    float rise = confidenceRise;
    float decay = confidenceDecay;

    // Fill tolerance: when template core is intact (>0.75 similarity),
    // halve decay rate. Fill onsets raised entropy but the core pattern is intact.
    if (bestTemplateSim_ > 0.75f) {
        decay *= 0.5f;
    }

    float alpha = (target > patternConfidence_) ? rise : decay;
    patternConfidence_ += (target - patternConfidence_) * alpha;

    // Step 5: Cold start boost (first 4 bars, template match >0.70)
    if (patternBarsAccumulated_ <= 4 && bestTemplateSim_ > 0.70f) {
        float boost = bestTemplateSim_ * 0.15f;  // proportional to similarity
        patternConfidence_ = fminf(patternConfidence_ + boost, 1.0f);
    }

    // Step 6: Save snapshot of bins while confidence is healthy.
    // Used by cachePatternRestore() — at the downward crossing through 0.3,
    // current barBins_ are contaminated by the new section. prevGoodBins_
    // preserves the last high-confidence state for meaningful cache matching.
    if (patternConfidence_ > 0.5f) {
        memcpy(prevGoodBins_, barBins_, sizeof(float) * BAR_BINS);
    }

    // Step 7: Cache save (upward confidence crossing through 0.6)
    // Guard: require > 8 bars of data to avoid saving undercooked patterns
    // from cold start boost (which can push confidence above 0.6 in 4 bars).
    if (patternConfidence_ >= 0.6f && prevPatternConfidence_ < 0.6f
        && patternBarsAccumulated_ > 8) {
        cachePatternSave();
    }

    // Step 8: Cache restore trigger (downward confidence crossing through 0.3)
    if (patternConfidence_ < 0.3f && prevPatternConfidence_ >= 0.3f) {
        cachePatternRestore();
    }

    // Step 9: Update prevPatternConfidence_
    prevPatternConfidence_ = patternConfidence_;
}

float AudioTracker::predictOnsetStrength(uint32_t nowMs) {
    if (!patternEnabled || patternConfidence_ < 0.3f || ioiConfidence_ < 0.5f) return 0.0f;

    // barPeriodMs is always >= 400ms (ioiPeakMs_ >= IOI_MIN_MS=100, * 4 = 400)
    float barPeriodMs = ioiPeakMs_ * 4.0f;
    float elapsed = (float)(nowMs - lastBarBoundaryMs_);
    float barPhase = fmodf(elapsed / barPeriodMs, 1.0f);
    int bin = (int)(barPhase * BAR_BINS) % BAR_BINS;
    float frac = barPhase * BAR_BINS - floorf(barPhase * BAR_BINS);
    int nextBin = (bin + 1) % BAR_BINS;
    return (barBins_[bin] * (1.0f - frac) + barBins_[nextBin] * frac) * patternConfidence_;
}

void AudioTracker::decayPatternBins() {
    // IOI bins: decay per frame (0.999^62.5 ≈ 0.94/sec, half-life ~11s).
    // Runs every frame at 62.5 Hz — intentionally faster than bar bins
    // because tempo can change quickly (breakdowns, DJ transitions).
    for (int i = 0; i < IOI_BINS; i++) ioiBins_[i] *= ioiDecayRate;
    // Bar bins: decay slower (patternDecayRate=0.9995, half-life ~22s at 62.5 Hz).
    // Pattern persists through breakdowns.
    for (int i = 0; i < BAR_BINS; i++) barBins_[i] *= patternDecayRate;
}

// ============================================================================
// Template Matching + LRU Cache
// ============================================================================

float AudioTracker::templateCosineSimilarity(const float* observed,
                                              const PatternTemplate& tmpl) const {
    // Cosine similarity with fill-masked bins zeroed out.
    // Fill mask positions are excluded so that extra fill onsets don't
    // reduce the match score against the core pattern.
    float dotProduct = 0.0f;
    float normObs = 0.0f;
    float normTmpl = 0.0f;
    for (int i = 0; i < BAR_BINS; i++) {
        if (tmpl.fillMask & (1 << i)) continue;  // Skip fill-prone positions
        float o = observed[i];
        float t = tmpl.bins[i];
        dotProduct += o * t;
        normObs += o * o;
        normTmpl += t * t;
    }
    float denom = sqrtf(normObs) * sqrtf(normTmpl);
    if (denom < 1e-8f) return 0.0f;
    return dotProduct / denom;
}

void AudioTracker::matchTemplates() {
    bestTemplateIdx_ = -1;
    bestTemplateSim_ = 0.0f;
    for (int t = 0; t < NUM_TEMPLATES; t++) {
        float sim = templateCosineSimilarity(barBins_, templates_[t]);
        if (sim > bestTemplateSim_) {
            bestTemplateSim_ = sim;
            bestTemplateIdx_ = t;
        }
    }
}

void AudioTracker::cachePatternSave() {
    // Check if current pattern already matches an existing cache entry (cosine > 0.85)
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!cache_[i].valid) continue;
        // Simple cosine similarity between two bar histograms (no fill mask)
        float dot = 0, na = 0, nb = 0;
        for (int b = 0; b < BAR_BINS; b++) {
            dot += barBins_[b] * cache_[i].bins[b];
            na += barBins_[b] * barBins_[b];
            nb += cache_[i].bins[b] * cache_[i].bins[b];
        }
        float denom = sqrtf(na) * sqrtf(nb);
        if (denom > 1e-8f && dot / denom > 0.85f) {
            // Already cached — just refresh LRU age
            uint8_t oldAge = cache_[i].age;
            for (int j = 0; j < CACHE_SIZE; j++) {
                if (cache_[j].valid && cache_[j].age < oldAge) {
                    cache_[j].age++;
                }
            }
            cache_[i].age = 0;
            cache_[i].bpm = bpm_;
            return;
        }
    }

    // Find slot: first invalid, or LRU (highest age)
    int slot = -1;
    uint8_t maxAge = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!cache_[i].valid) { slot = i; break; }
        if (cache_[i].age >= maxAge) { maxAge = cache_[i].age; slot = i; }
    }
    if (slot < 0) slot = 0;  // Safety fallback

    // Age all other entries
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache_[i].valid) cache_[i].age++;
    }

    // Save current pattern
    memcpy(cache_[slot].bins, barBins_, sizeof(float) * BAR_BINS);
    cache_[slot].bpm = bpm_;
    cache_[slot].age = 0;
    cache_[slot].valid = true;
}

void AudioTracker::cachePatternRestore() {
    // Search cache for best cosine match > 0.7 against prevGoodBins_
    // (not current barBins_, which are contaminated by the section change
    // that triggered the confidence drop)
    int bestIdx = -1;
    float bestSim = 0.7f;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (!cache_[i].valid) continue;
        float dot = 0, na = 0, nb = 0;
        for (int b = 0; b < BAR_BINS; b++) {
            dot += prevGoodBins_[b] * cache_[i].bins[b];
            na += prevGoodBins_[b] * prevGoodBins_[b];
            nb += cache_[i].bins[b] * cache_[i].bins[b];
        }
        float denom = sqrtf(na) * sqrtf(nb);
        float sim = (denom > 1e-8f) ? dot / denom : 0.0f;
        if (sim > bestSim) {
            bestSim = sim;
            bestIdx = i;
        }
    }

    if (bestIdx >= 0) {
        cacheRestoreActive_ = true;
        cacheRestoreBarsLeft_ = 4;
        cacheRestoreIdx_ = bestIdx;
    }
}

void AudioTracker::cacheProgressiveBlend() {
    if (!cacheRestoreActive_ || cacheRestoreIdx_ < 0 ||
        cacheRestoreIdx_ >= CACHE_SIZE || !cache_[cacheRestoreIdx_].valid) {
        cacheRestoreActive_ = false;
        return;
    }

    // Blend rates: 40%, 20%, 10%, 5% over 4 bar boundaries
    int step = 4 - cacheRestoreBarsLeft_;  // 0,1,2,3
    if (step < 0 || step > 3) { cacheRestoreActive_ = false; return; }

    float rate = CACHE_BLEND_RATES[step];
    const float* cached = cache_[cacheRestoreIdx_].bins;
    for (int i = 0; i < BAR_BINS; i++) {
        barBins_[i] = barBins_[i] * (1.0f - rate) + cached[i] * rate;
    }

    cacheRestoreBarsLeft_--;
    if (cacheRestoreBarsLeft_ <= 0) {
        cacheRestoreActive_ = false;
    }
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

    // Beat-proximity energy boost via PLP pulse (replaces subdivision-aware proximity)
    rawEnergy *= (1.0f + plpPulseValue_ * 0.3f * plpConfidence_);

    // Pattern prediction (computed once, used for both anticipatory energy and pulse boost)
    float patternPrediction = 0.0f;
    if (patternEnabled) {
        patternPrediction = predictOnsetStrength(nowMs);
        // Anticipatory energy: subtle pre-glow before predicted onsets.
        // patternLookahead=0.05 = 5% of beat period (~25ms at 120 BPM).
        // Intentionally subtle — this is a "lights know what's coming" effect,
        // not a full pre-flash. Increase to 0.12-0.15 for more visible anticipation.
        if (anticipationGain > 0.0f) {
            uint32_t lookaheadMs = (uint32_t)(patternLookahead * ioiPeakMs_);
            float lookaheadPrediction = predictOnsetStrength(nowMs + lookaheadMs);
            rawEnergy += lookaheadPrediction * anticipationGain;
        }
    }

    control_.energy = clampf(rawEnergy, 0.0f, 1.0f);

    // --- Pulse ---
    // Raw onset strength with pattern prediction boost.
    // PLP pulse (via phaseToPulse()) provides beat-synced breathing separately.
    float pulse = lastPulseStrength_;

    if (pulse > 0.0f && patternPrediction > 0.0f) {
        pulse *= (1.0f + patternPrediction * patternGain);
    }

    control_.pulse = clampf(pulse, 0.0f, 1.0f);

    // --- Phase + PLP Pulse ---
    control_.phase = plpPhase_;
    control_.plpPulse = plpPulseValue_;

    // --- Rhythm Strength ---
    // Base: ACF periodicity + comb bank confidence (proven formula).
    // PLP confidence can only boost, never drag down.
    float combConf = combFilterBank_.getPeakConfidence();
    float baseStrength = periodicityStrength_ * 0.6f + combConf * 0.4f;
    float strength = (plpConfidence_ > baseStrength) ? plpConfidence_ : baseStrength;

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
