#include "AudioTracker.h"
#include <math.h>
#include <string.h>

// Max ACF lag array size: need 4x maxLag for Percival harmonic folding.
// At 60 BPM: maxLag=66. Percival reads acf[4*lagIdx] so needs 4*46+66 = 250.
// Round up for safety.
static constexpr int MAX_ACF_SIZE = 280;

// Constants moved to AudioTracker public members (v74) for tuning via serial console.
// Previous hardcoded values: ODF_CONTRAST=2.0, PULSE_THRESHOLD_MULT=2.0,
// PULSE_MIN_LEVEL=0.03, PULSE_ONSET_FLOOR=0.1

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
    // Linearize circular OSS buffer into class member (shared with updatePlpAnalysis)
    int startIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    for (int i = 0; i < ossCount_; i++) {
        ossLinear_[i] = ossBuffer_[(startIdx + i) % OSS_BUFFER_SIZE];
    }

    // --- Grid search: find period with best PMR across 3 sources ---
    // Instead of ACF peak → period, directly try every candidate period and
    // epoch-fold each source. The period × source with highest peak-to-mean
    // ratio (PMR) wins. This optimizes for pattern quality, not BPM accuracy.
    int minLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMax);
    int maxLag = static_cast<int>(OSS_FRAMES_PER_MIN / bpmMin);
    if (maxLag >= ossCount_ / 2) maxLag = ossCount_ / 2 - 1;
    if (minLag < 2) minLag = 2;

    // Linearize bass and NN onset buffers
    static float bassLinear[BASS_BUFFER_SIZE];
    static float nnLinear[NN_BUFFER_SIZE];
    int bStart = (bassWriteIdx_ - bassCount_ + BASS_BUFFER_SIZE) % BASS_BUFFER_SIZE;
    for (int i = 0; i < bassCount_; i++) bassLinear[i] = bassBuffer_[(bStart + i) % BASS_BUFFER_SIZE];
    int nStart = (nnWriteIdx_ - nnCount_ + NN_BUFFER_SIZE) % NN_BUFFER_SIZE;
    for (int i = 0; i < nnCount_; i++) nnLinear[i] = nnOnsetBuffer_[(nStart + i) % NN_BUFFER_SIZE];

    // Source buffers and counts
    const float* sources[3] = { ossLinear_, bassLinear, nnLinear };
    const int sourceCounts[3] = { ossCount_, bassCount_, nnCount_ };
    // source 0=flux, 1=bass, 2=nn

    float bestPmr = 0.0f;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    int bestSource = 0;

    for (int lag = minLag; lag <= maxLag; lag++) {
        for (int src = 0; src < 3; src++) {
            int count = sourceCounts[src];
            if (count < lag * 2) continue;  // Need at least 2 periods
            const float* buf = sources[src];

            // Epoch-fold: sum values at each position within the period
            float patSum[MAX_PATTERN_LEN] = {0};
            int epochs = 0;
            for (int off = count - lag; off >= 0; off -= lag) {
                for (int j = 0; j < lag; j++) patSum[j] += buf[off + j];
                epochs++;
            }
            if (epochs < 2) continue;

            // Compute PMR = max / mean of folded pattern
            float maxVal = 0.0f, sumVal = 0.0f;
            for (int j = 0; j < lag; j++) {
                float v = patSum[j] / epochs;
                if (v > maxVal) maxVal = v;
                sumVal += v;
            }
            float mean = sumVal / lag;
            float pmr = (mean > 1e-10f) ? maxVal / mean : 0.0f;

            if (pmr > bestPmr) {
                bestPmr = pmr;
                bestPeriod = lag;
                bestSource = src;
            }
        }
    }

    plpBestPmr_ = bestPmr;
    plpBestSource_ = bestSource;

    // --- Also compute ACF periodicity strength for rhythmStrength ---
    // Quick ACF at the winning period (just one lag, not full spectrum)
    float ossMean = 0.0f;
    for (int i = 0; i < ossCount_; i++) ossMean += ossLinear_[i];
    ossMean /= ossCount_;

    float signalEnergy = 0.0f;
    float lagCorr = 0.0f;
    int n = ossCount_ - bestPeriod;
    for (int i = 0; i < ossCount_; i++) {
        float v = ossLinear_[i] - ossMean;
        signalEnergy += v * v;
        if (i < n) lagCorr += v * (ossLinear_[i + bestPeriod] - ossMean);
    }
    if (signalEnergy > 1e-10f) {
        float normCorr = lagCorr / signalEnergy;
        float newStrength = clampf(normCorr * 1.5f, 0.0f, 1.0f);
        periodicityStrength_ = periodicityStrength_ * 0.5f + newStrength * 0.5f;
    }

    // BPM from best period
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
    // --- 1. Use period from grid search (set by runAutocorrelation) ---
    int patLen = static_cast<int>(beatPeriodFrames_ + 0.5f);
    if (patLen < 2) patLen = 2;
    if (patLen > MAX_PATTERN_LEN) patLen = MAX_PATTERN_LEN;
    plpPatternLen_ = patLen;

    // --- 2. Epoch-fold the WINNING source at the winning period ---
    // runAutocorrelation() already found the best source (flux/bass/nn) and period.
    // Now extract the pattern from that source for visual output.
    // ossLinear_ already populated. Bass/NN linearized in runAutocorrelation statics.

    // Use the winning source buffer (ossLinear_ for flux, or re-linearize for bass/nn)
    const float* sourceBuf = ossLinear_;
    int sourceCount = ossCount_;
    if (plpBestSource_ == 1 && bassCount_ >= patLen * 2) {
        // Bass won — linearize bass buffer
        static float bassLin[BASS_BUFFER_SIZE];
        int bS = (bassWriteIdx_ - bassCount_ + BASS_BUFFER_SIZE) % BASS_BUFFER_SIZE;
        for (int i = 0; i < bassCount_; i++) bassLin[i] = bassBuffer_[(bS + i) % BASS_BUFFER_SIZE];
        sourceBuf = bassLin;
        sourceCount = bassCount_;
    } else if (plpBestSource_ == 2 && nnCount_ >= patLen * 2) {
        // NN onset won — linearize NN buffer
        static float nnLin[NN_BUFFER_SIZE];
        int nS = (nnWriteIdx_ - nnCount_ + NN_BUFFER_SIZE) % NN_BUFFER_SIZE;
        for (int i = 0; i < nnCount_; i++) nnLin[i] = nnOnsetBuffer_[(nS + i) % NN_BUFFER_SIZE];
        sourceBuf = nnLin;
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

    // Normalize to [0, 1], then apply contrast via power-law
    float maxVal = 0.0f;
    for (int j = 0; j < patLen; j++) {
        patternAccum[j] /= epochs;
        if (patternAccum[j] > maxVal) maxVal = patternAccum[j];
    }
    if (maxVal > 0.0f) {
        for (int j = 0; j < patLen; j++) {
            float normalized = patternAccum[j] / maxVal;
            plpPattern_[j] = (plpNovGain != 1.0f) ? powf(normalized, plpNovGain) : normalized;
        }
    }

    // --- 3. PLP confidence from PMR quality ---
    // High PMR (>2.5) = strong pattern = high confidence
    // Low PMR (<1.5) = weak/flat pattern = low confidence
    float targetConf = clampf((plpBestPmr_ - 1.5f) / 2.0f, 0.0f, 1.0f);
    // Also gate by periodicity strength (weak ACF = unreliable period)
    targetConf *= clampf(periodicityStrength_ * 2.0f, 0.0f, 1.0f);
    plpConfidence_ += (targetConf - plpConfidence_) * plpConfAlpha;

    // --- 4. Cross-correlate last period of winning source with pattern ---
    if (sourceCount >= patLen) {
        float bestCorr = -1e30f;
        int bestOffset = 0;
        for (int offset = 0; offset < patLen; offset++) {
            float sum = 0.0f;
            for (int j = 0; j < patLen; j++) {
                int sigIdx = sourceCount - patLen + j;
                int patIdx = (j + offset) % patLen;
                sum += sourceBuf[sigIdx] * plpPattern_[patIdx];
            }
            if (sum > bestCorr) {
                bestCorr = sum;
                bestOffset = offset;
            }
        }

        float measuredPhase = static_cast<float>(bestOffset) / static_cast<float>(patLen);
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
