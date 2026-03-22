#include "AudioTracker.h"
#include <math.h>
#include <string.h>

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PULSE_ONSET_FLOOR=0.1

static constexpr float TWO_PI_F = 6.28318530718f;

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
    lastBarBoundaryMs_ = now;

    return true;
}

void AudioTracker::end() {
    mic_.end();
}

int AudioTracker::getHwGain() const {
    return mic_.getHwGain();
}

void AudioTracker::resetPatternMemory() {
    memset(ioiBins_, 0, sizeof(ioiBins_));
    memset(barBins_, 0, sizeof(barBins_));
    memset(onsetTimes_, 0, sizeof(onsetTimes_));
    onsetBufCount_ = 0;
    onsetWriteIdx_ = 0;
    ioiPeakMs_ = 500.0f;
    ioiPeakBpm_ = 120.0f;
    ioiPeakStrength_ = 0.0f;
    ioiConfidence_ = 0.0f;
    barEntropy_ = 1.0f;
    patternConfidence_ = 0.0f;
    patternBarsAccumulated_ = 0;
    lastBarBoundaryMs_ = time_.millis();
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

    // Mean-subtract each source (critical for DFT — DC leakage otherwise
    // dominates all frequency bins, making periodic components invisible)
    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < 20) continue;
        float mean = 0.0f;
        for (int i = 0; i < count; i++) mean += sources[src][i];
        mean /= count;
        for (int i = 0; i < count; i++) sources[src][i] -= mean;
    }

    float bestMag = 0.0f;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    float bestPhase = 0.0f;
    int bestSource = 0;

    // Scan candidate periods from bpmMin to bpmMax
    int minLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMax);
    int maxLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMin);
    if (minLag < 10) minLag = 10;
    if (maxLag > MAX_PATTERN_LEN) maxLag = MAX_PATTERN_LEN;  // Keep period ≤ pattern buffer

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

            if (mag > bestMag) {
                bestMag = mag;
                bestPeriod = lag;
                bestSource = src;
                // Phase: position of the peak within the cycle
                bestPhase = -atan2f(dftImag, dftReal) / TWO_PI;
                if (bestPhase < 0.0f) bestPhase += 1.0f;
            }
        }
    }

    plpDftMag_ = bestMag;
    plpBestPeriod_ = bestPeriod;
    plpBestSource_ = static_cast<uint8_t>(bestSource);
    plpDftPhase_ = bestPhase;  // Store for use in updatePlpAnalysis

    // --- Periodicity strength from DFT magnitude ---
    // With sqrt(N) normalization, magnitude ~1.0 for a clearly periodic signal
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
    static constexpr float ALPHA_ERR = 0.15f;    // Error EMA rate (~6-7 sample window)
    static constexpr float SIGMA_REF = 0.08f;    // Expected RMS error when locked
    static constexpr float ALPHA_MIN = 0.10f;    // Correction rate when locked (sweep: stability=0.87)
    static constexpr float ALPHA_MAX = 0.50f;    // Correction rate when converging (sweep: atTransient=0.50)

    phaseErrEma_ += ALPHA_ERR * (phaseError - phaseErrEma_);
    float deviation = phaseError - phaseErrEma_;
    phaseErrVar_ += ALPHA_ERR * (deviation * deviation - phaseErrVar_);

    float sigma = sqrtf(phaseErrVar_ > 0.0f ? phaseErrVar_ : 0.0f);
    float lambda = sigma / (sigma + SIGMA_REF);
    float alpha = ALPHA_MIN + (ALPHA_MAX - ALPHA_MIN) * lambda;

    plpPhase_ += alpha * phaseError;
    if (plpPhase_ < 0.0f) plpPhase_ += 1.0f;
    if (plpPhase_ >= 1.0f) plpPhase_ -= 1.0f;

    // --- 4. PLP confidence from DFT magnitude + signal presence ---
    float dftConf = clampf(plpDftMag_, 0.0f, 1.0f);
    float signalPresence = clampf(mic_.getLevel() / plpSignalFloor, 0.0f, 1.0f);
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

    // Read extracted pattern at current phase position (linear interpolation)
    if (plpPatternLen_ > 0 && plpConfidence_ > plpActivation) {
        float patPos = plpPhase_ * plpPatternLen_;
        int idx0 = static_cast<int>(patPos) % plpPatternLen_;
        float frac = patPos - floorf(patPos);
        int idx1 = (idx0 + 1) % plpPatternLen_;
        plpPulseValue_ = plpPattern_[idx0] * (1.0f - frac) + plpPattern_[idx1] * frac;
    } else {
        // Fallback: cosine pulse (same shape as old phaseToPulse)
        plpPulseValue_ = 0.5f + 0.5f * cosf(plpPhase_ * TWO_PI_F);
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

    // Shannon entropy of bar histogram (normalized to [0, 1]).
    // Retained for serial diagnostics (getBarEntropy() is streamed via SerialConsole).
    float sum = 0;
    for (int i = 0; i < BAR_BINS; i++) sum += barBins_[i];
    if (sum < 1e-6f) {
        barEntropy_ = 1.0f;
        patternConfidence_ *= 0.9f;
        return;
    }

    float H = 0;
    for (int i = 0; i < BAR_BINS; i++) {
        float p = barBins_[i] / sum;
        if (p > 1e-6f) H -= p * log2f(p);
    }
    barEntropy_ = H / 4.0f;  // log2(16) = 4.0

    // Confidence update — peak-to-mean ratio measures pattern structure.
    // 1.0 = flat (no pattern), 4.0+ = strong peaks (kick/snare).
    float maxBin = 0.0f;
    float meanBin = sum / BAR_BINS;
    for (int i = 0; i < BAR_BINS; i++) {
        if (barBins_[i] > maxBin) maxBin = barBins_[i];
    }
    float peakToMean = (meanBin > 1e-6f) ? (maxBin / meanBin) : 0.0f;
    // Map peak-to-mean [1, 4] → target [0, 1]. At 1x (uniform) target=0,
    // at 4x+ (strong beats) target=1. Typical 4otf pattern peaks at 2-3x.
    float target = clampf((peakToMean - 1.0f) / 3.0f, 0.0f, 1.0f);

    float alpha = (target > patternConfidence_) ? confidenceRise : confidenceDecay;
    patternConfidence_ += (target - patternConfidence_) * alpha;
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
// Output Synthesis
// ============================================================================

void AudioTracker::synthesizeOutputs(float dt, uint32_t nowMs) {
    // --- Energy ---
    // Hybrid: mic level + bass mel energy (cached) + ODF peak-hold
    float micLevel = mic_.getLevel();
    float rawEnergy = energyMicWeight * micLevel + energyMelWeight * cachedBassEnergy_ + energyOdfWeight * odfPeakHold_;

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
    // PLP confidence can only boost, never drag down ACF periodicity.
    float strength = (plpConfidence_ > periodicityStrength_) ? plpConfidence_ : periodicityStrength_;

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
