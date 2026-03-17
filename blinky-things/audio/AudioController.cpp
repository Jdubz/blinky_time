#include "AudioController.h"
#include "../inputs/SerialConsole.h"
#include "../types/BlinkyAssert.h"
#include <math.h>

// ============================================================================
// Named Constants for rhythm-based pulse modulation
// ============================================================================

// Beat proximity thresholds for pulse modulation
// Beat proximity thresholds moved to AudioController class as tunable parameters
// See pulseNearBeatThreshold and pulseFarFromBeatThreshold in AudioController.h

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

    // Initialize spectral analysis
    spectral_.begin();

    // Reset OSS buffer and timestamps
    for (int i = 0; i < OSS_BUFFER_SIZE; i++) {
        ossBuffer_[i] = 0.0f;
        ossTimestamps_[i] = 0;
    }
    ossWriteIdx_ = 0;
    ossCount_ = 0;

    // Reset tempo estimation
    bpm_ = 120.0f;
    beatPeriodMs_ = 500.0f;
    periodicityStrength_ = 0.0f;

    // Reset phase tracking
    phase_ = 0.0f;

    // Reset beat stability tracking
    for (int i = 0; i < STABILITY_BUFFER_SIZE; i++) {
        interBeatIntervals_[i] = 0.0f;
    }
    ibiWriteIdx_ = 0;
    ibiCount_ = 0;
    lastBeatMs_ = 0;
    beatStability_ = 0.0f;

    // Reset continuous tempo estimation
    tempoVelocity_ = 0.0f;
    prevBpm_ = 120.0f;
    nextBeatMs_ = 0;

    // Reset timing
    lastAutocorrMs_ = 0;
    lastSignificantAudioMs_ = 0;

    // Reset onset density tracking
    onsetDensity_ = 0.0f;
    onsetCountInWindow_ = 0;
    onsetDensityWindowStart_ = time_.millis();

    // Initialize and reset comb filter bank
    // Uses 60 Hz frame rate assumption (same as OSS buffer)
    combFilterBank_.init(static_cast<float>(OSS_FRAME_RATE));

    // Initialize Bayesian tempo state (after comb bank, which sets up BPM/lag arrays)
    initTempoState();

    // Reset CBSS state
    for (int i = 0; i < OSS_BUFFER_SIZE; i++) {
        cbssBuffer_[i] = 0.0f;
    }
    lastBeatSample_ = 0;
    beatPeriodSamples_ = 30;  // ~120 BPM at 60Hz
    sampleCounter_ = 0;
    beatCount_ = 0;
    cbssConfidence_ = 0.0f;
    lastSmoothedOnset_ = 0.0f;
    lastBeatWasPredicted_ = false;
    lastFiredBeatPredicted_ = false;
    lastTransientSample_ = -1;

    // Reset ODF smoothing
    for (int i = 0; i < ODF_SMOOTH_MAX; i++) odfSmoothBuffer_[i] = 0.0f;
    odfSmoothIdx_ = 0;

    // Reset prediction state
    timeToNextBeat_ = 15;  // ~250ms at 60Hz
    timeToNextPrediction_ = 10;
    pendingBeatPeriod_ = -1;  // No pending tempo change
    beatsSinceOctaveCheck_ = 0;
    // (beatsSinceMetricalCheck_/Template/Subbeat init removed v64 — checks removed, no benefit)
    // (multi-agent state init removed v64 — multi-agent beat tracking removed, no benefit)
    // (pfInitialized_/fwdInitialized_/pfRngState_/pfCooldown_ init removed v64 — PF and forward filter removed)
    pllPhaseIntegral_ = 0.0f;  // Reset PLL integral accumulator (v45)
    lastSnapOffset_ = 0;       // Reset onset snap hysteresis (v65)
    snapConsistencyCount_ = 0; // Reset snap consistency counter (v65)
    downbeatSmoothed_ = 0.0f;  // Reset downbeat EMA (v65)
    beatInMeasure_ = 0;        // Reset measure counter (v65)
    effectiveTightness_ = cbssTightness;  // Initialize adaptive tightness (v45)
    logGaussianLastT_ = 0;
    logGaussianLastTight_ = 0.0f;
    logGaussianWeightsSize_ = 0;
    beatExpectationLastT_ = 0;
    beatExpectationSize_ = 0;

    // Initialize NN beat activation (fails gracefully if model not compiled in)
    frameBeatNN_.begin();
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::INFO) {
        frameBeatNN_.printDiagnostics();
    }

    // Reset output
    control_ = AudioControl();
    lastPulseStrength_ = 0.0f;
    lastPulseMs_ = 0;

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

    // 2. Feed samples to spectral analysis from mic's ring buffer
    static int16_t sampleBuffer[256];  // Matches FFT_SIZE
    int samplesRead = mic_.getSamplesForExternal(sampleBuffer, 256);
    if (samplesRead > 0) {
        spectral_.addSamples(sampleBuffer, samplesRead);
    }

    // 3. Process spectral data
    if (spectral_.hasSamples()) {
        spectral_.process();
    }

    // 4. Get onset strength for rhythm analysis
    //    NN model discriminates beat onsets from sustained notes/hi-hats/noise.
    //    Use pure NN ODF when model is ready — mixing with mic_.getLevel()
    //    degrades beat tracking because mic level fires on all loud audio.
    //    Falls back to mic level only when no model is loaded.
    float onsetStrength = 0.0f;

    if (frameBeatNN_.isReady()) {
        if (spectral_.isFrameReady() || spectral_.hasPreviousFrame()) {
            onsetStrength = frameBeatNN_.infer(spectral_.getRawMelBands());
            spectral_.resetFrameReady();
        } else {
            onsetStrength = mic_.getLevel();
        }
    } else {
        onsetStrength = mic_.getLevel();
    }

    // 4b. Simple pulse detection from ODF signal
    //     Pulse drives visual spark effects only; ODF drives beat tracking separately.
    {
        float pulseStrength = 0.0f;
        if (mic_.getLevel() > PULSE_MIN_LEVEL) {
            float odfVal = onsetStrength;
            // Detect pulse when ODF exceeds smoothed ODF × threshold and cooldown elapsed.
            // lastSmoothedOnset_ is the EMA-smoothed ODF; use as baseline for spike detection.
            float odfMean = (lastSmoothedOnset_ > 0.001f) ? lastSmoothedOnset_ : 0.1f;
            uint16_t cooldown = effectivePulseCooldownMs();
            // Wraparound guard: if nowMs < lastPulseMs_ (uint32 wrap at ~49 days),
            // reset lastPulseMs_ to allow the next pulse through cleanly.
            if (nowMs < lastPulseMs_) {
                lastPulseMs_ = nowMs;
            }
            if (odfVal > odfMean * PULSE_THRESHOLD_MULT &&
                (nowMs - lastPulseMs_) > static_cast<uint32_t>(cooldown)) {
                pulseStrength = clampf(odfVal, 0.0f, 1.0f);
                lastPulseMs_ = nowMs;
            }
        }
        lastPulseStrength_ = pulseStrength;
    }

    // 4c. Count onsets for density tracking
    if (lastPulseStrength_ > 0.0f) {
        onsetCountInWindow_++;
    }

    // 4d. Phase correction: when a pulse occurs near a predicted beat
    //     boundary, nudge lastBeatSample_ to align phase with the pulse.
    //     This corrects cumulative drift from small BPM errors.
    if (lastPulseStrength_ > 0.0f) {
        lastTransientSample_ = sampleCounter_;

        if (phaseCorrectionStrength > 0.0f && beatCount_ > 2 && beatPeriodSamples_ >= 10) {
            int T = beatPeriodSamples_;
            int elapsed = sampleCounter_ - lastBeatSample_;
            int phaseError = elapsed % T;
            if (phaseError > T / 2) phaseError -= T;  // Center: -T/2 to +T/2

            int window = T / 4;  // Correction window: +/-25% of beat period
            if (phaseError != 0 && phaseError > -window && phaseError < window) {
                int correction = static_cast<int>(phaseError * phaseCorrectionStrength);
                if (correction != 0) {
                    lastBeatSample_ += correction;
                }
            }
        }
    }

    // Cache NN-active state once per update — used by ODF smoothing, contrast,
    // ACF mean subtraction, and CBSS alpha adaptation.
    nnActive_ = frameBeatNN_.isReady();
    frameBeatNN_.setProfileEnabled(nnProfile);

    // Apply ODF smoothing before all consumers (OSS buffer, comb bank, CBSS).
    // Bypass when NN is active — FrameBeatNN's sliding window already provides
    // temporal context. Additional smoothing blurs activation peaks that the CBSS
    // needs sharp. (madmom/BeatNet don't smooth NN output.)
    // cppcheck-suppress knownConditionTrueFalse -- nnActive_ is always false until NN model is loaded
    if (nnActive_) {
        lastSmoothedOnset_ = onsetStrength;
    } else {
        onsetStrength = smoothOnsetStrength(onsetStrength);
        lastSmoothedOnset_ = onsetStrength;
    }

    // Track when we last had significant audio
    // Use a threshold above typical noise floor (~0.02) to avoid tracking silence
    const float SIGNIFICANT_AUDIO_THRESHOLD = 0.05f;
    bool hasSignificantAudio = (onsetStrength > SIGNIFICANT_AUDIO_THRESHOLD ||
                                 mic_.getLevel() > SIGNIFICANT_AUDIO_THRESHOLD);

    // 5. Add sample to onset strength buffer with timestamp
    // Only add significant audio to avoid filling buffer with noise patterns
    // (onsetTrainOdf/odfDiffMode removed v67 — BandFlux pipeline removed)
    float ossValue = hasSignificantAudio ? onsetStrength : 0.0f;
    if (hasSignificantAudio) {
        lastSignificantAudioMs_ = nowMs;
    }
    addOssSample(ossValue, nowMs);

    // 6. Run autocorrelation periodically (tunable period, default 500ms)
    if (nowMs - lastAutocorrMs_ >= autocorrPeriodMs) {
        runAutocorrelation(nowMs);
        lastAutocorrMs_ = nowMs;
    }

    // 6b. Update comb filter bank (independent tempo validation)
    //     Provides tempo validation without depending on autocorrelation
    if (combBankEnabled) {
        combFilterBank_.feedbackGain = combBankFeedback;
        combFilterBank_.process(onsetStrength);
    }

    // 7. CBSS input: apply contrast to onset strength (shared across all beat tracking modes)
    //    NN ODF is smooth [0,1] with non-zero baseline (~0.2-0.4). Power-law contrast
    //    (squaring) sharpens beat peaks vs baseline: 0.8→0.64, 0.3→0.09 (ratio 7:1 vs 2.7:1).
    //    A/B tested: cbssContrast=2.0 wins 10-6 vs 1.0 (BandFlux ODF, 3 devices × 18 tracks).
    float cbssInput = onsetStrength;
    if (cbssContrast != 1.0f && cbssInput > 0.0f) {
        cbssInput = powf(cbssInput, cbssContrast);
    }

    // 8. Update beat tracking
    //    (Forward filter, particle filter, HMM, multi-agent modes removed v64 — A/B tested, no benefit)
    //    Default: Bayesian fusion (ACF + comb filter bank) + CBSS predict+countdown
    updateCBSS(cbssInput);
    detectBeat();

    // 9. Synthesize output
    synthesizeEnergy();
    synthesizePulse();
    synthesizePhase();
    updateOnsetDensity(nowMs);       // Update density before rhythmStrength uses it
    synthesizeRhythmStrength();

    return control_;
}

// ===== CONFIGURATION =====
// (setDetectorEnabled/Weight/Threshold removed v67 — BandFlux pipeline removed)

// lockHwGain/unlockHwGain/isHwGainLocked removed v72 — gain is fixed at platform default

int AudioController::getHwGain() const {
    return mic_.getHwGain();
}

// ===== RHYTHM TRACKING =====

void AudioController::addOssSample(float onsetStrength, uint32_t timestampMs) {
    ossBuffer_[ossWriteIdx_] = onsetStrength;
    ossTimestamps_[ossWriteIdx_] = timestampMs;
    ossWriteIdx_ = (ossWriteIdx_ + 1) % OSS_BUFFER_SIZE;
    if (ossCount_ < OSS_BUFFER_SIZE) {
        ossCount_++;
    }
}

void AudioController::runAutocorrelation(uint32_t nowMs) {
    // Progressive startup: start autocorrelation after 1 second (60 samples @ 60Hz)
    // instead of waiting 3 seconds. Early estimates have limited tempo range
    // (maxLag = ossCount_/2 restricts minimum detectable BPM) but periodicityStrength_
    // smoothing handles the lower reliability. At 60 samples: minimum detectable BPM
    // is ~120 (maxLag=30); upper bound is bpmMax (200). Full 60-200 BPM range
    // available after ~2 seconds (120 samples). Note: brief wrong-tempo estimates
    // are possible during warmup but rhythmStrength blending limits visual impact.
    if (ossCount_ < 60) {
        return;
    }

    // Convert BPM range to time-based lag range using actual timestamps
    // Formula: lagMs = 60000 / bpm (milliseconds per beat)
    // 200 BPM = 300ms, 60 BPM = 1000ms
    float minLagMs = 60000.0f / bpmMax;  // Minimum period in milliseconds
    float maxLagMs = 60000.0f / bpmMin;  // Maximum period in milliseconds

    // Convert to sample indices using actual elapsed time in buffer
    int mostRecentIdx = (ossWriteIdx_ - 1 + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
    int oldestIdx = (ossWriteIdx_ - ossCount_ + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;

    // Handle potential timestamp wraparound (after ~49 days)
    int32_t bufferDurationMs = (int32_t)(ossTimestamps_[mostRecentIdx] - ossTimestamps_[oldestIdx]);
    if (bufferDurationMs < 0 || bufferDurationMs > 10000) {
        // Wraparound detected or invalid duration - use expected value for full buffer
        bufferDurationMs = 6000;  // 6 seconds @ 60 Hz
    }

    // Estimate samples per millisecond from buffer
    BLINKY_ASSERT(bufferDurationMs > 0, "AudioController: bufferDurationMs <= 0 in autocorrelation");
    float samplesPerMs = bufferDurationMs > 0 ? (float)ossCount_ / (float)bufferDurationMs : 0.06f;

    int minLag = static_cast<int>(minLagMs * samplesPerMs);
    int maxLag = static_cast<int>(maxLagMs * samplesPerMs);

    if (minLag < 10) minLag = 10;
    if (maxLag > ossCount_ / 2) maxLag = ossCount_ / 2;
    if (minLag >= maxLag) return;

    // === LINEARIZE OSS + ADAPTIVE ODF THRESHOLD ===
    // Copy circular OSS buffer to a linear working buffer.
    // When adaptiveOdfThresh is on, apply BTrack-style local-mean subtraction
    // with half-wave rectification to remove arrangement-level dynamics.
    static float ossLinear[OSS_BUFFER_SIZE];  // static to avoid 1.4KB stack allocation; single-threaded, not reentrant
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - ossCount_ + i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        ossLinear[i] = ossBuffer_[idx];
    }

    if (adaptiveOdfThresh) {
        int halfWin = odfThreshWindow;  // Tunable half-window (each side), default 15 samples (~250ms at 60Hz)

        for (int i = 0; i < ossCount_; i++) {
            // Compute local mean in centered window
            float localSum = 0.0f;
            int count = 0;
            int wStart = i - halfWin;
            int wEnd = i + halfWin;
            if (wStart < 0) wStart = 0;
            if (wEnd >= ossCount_) wEnd = ossCount_ - 1;
            for (int j = wStart; j <= wEnd; j++) {
                localSum += ossLinear[j];
                count++;
            }
            float localMean = localSum / static_cast<float>(count);

            // Subtract local mean + half-wave rectification (keep only positive residuals)
            float val = ossLinear[i] - localMean;
            ossLinear[i] = (val > 0.0f) ? val : 0.0f;
        }
    }

    // Compute signal energy for normalization (from linearized/thresholded buffer)
    float signalEnergy = 0.0f;
    float maxOss = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        float val = ossLinear[i];
        signalEnergy += val * val;
        if (val > maxOss) maxOss = val;
    }

    // DEBUG: Print autocorrelation diagnostics (only when rhythm debug channel enabled)
    // Use: "debug rhythm on" to enable
    static uint32_t lastDebugMs = 0;
    bool shouldPrintDebug = SerialConsole::isDebugChannelEnabled(DebugChannel::RHYTHM) &&
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

    // Threshold for detecting meaningful signal vs noise
    // With noise floor ~0.02 and 360 samples: 360 * 0.02^2 = 0.144
    // With silence (zeros): signalEnergy approaches 0
    // Require at least 10% of buffer to have significant values (signalEnergy > 0.01)
    if (signalEnergy < 0.01f || maxOss < 0.05f) {
        // No meaningful signal - decay periodicity faster
        periodicityStrength_ *= 0.8f;
        return;
    }

    // Autocorrelation: compute correlation for all lags
    // We'll store correlations to find multiple peaks
    // NOTE: Static buffer assumes single-threaded execution (only one AudioController instance)
    // FIX: Initialize to zero to prevent garbage data from previous frames
    // Extended to 256 to accommodate 4-harmonic comb lookups (BTrack-style)
    static float correlationAtLag[256] = {0};
    // Extend range for harmonic lookups: need ACF at 2T, 3T, 4T for comb filter
    int harmonicMaxLag = 4 * maxLag;
    if (harmonicMaxLag > ossCount_ / 2) harmonicMaxLag = ossCount_ / 2;
    int harmonicCorrelationSize = harmonicMaxLag - minLag + 1;
    if (harmonicCorrelationSize > 256) harmonicCorrelationSize = 256;
    int correlationSize = maxLag - minLag + 1;
    // Clamp fundamental range to harmonic range (fires when OSS buffer < one full period)
    if (correlationSize > harmonicCorrelationSize) correlationSize = harmonicCorrelationSize;

    // Clear entire buffer to prevent any stale data from previous calls
    memset(correlationAtLag, 0, sizeof(correlationAtLag));

    float maxCorrelation = 0.0f;
    int bestLag = minLag;  // FIX: Initialize to valid lag, not 0

    // ODF mean subtraction (BTrack-style detrending)
    // Removes DC bias from autocorrelation — without this, all lags appear
    // somewhat correlated due to the non-zero mean of the OSS buffer.
    // Disabled for BandFlux (v32: raw ODF preserves ACF structure, +70% F1).
    // Enabled for NN ODF: smooth [0,1] output has non-zero baseline (~0.2-0.4)
    // that elevates all ACF lags equally, masking the true tempo peak.
    float ossMean = 0.0f;
    if (odfMeanSubEnabled || nnActive_) {
        for (int i = 0; i < ossCount_; i++) {
            ossMean += ossLinear[i];
        }
        ossMean /= static_cast<float>(ossCount_);
        // Adjust signalEnergy to variance: sum((x-mean)^2) = sum(x^2) - N*mean^2
        // This keeps normalization consistent with the mean-subtracted autocorrelation.
        signalEnergy -= static_cast<float>(ossCount_) * ossMean * ossMean;
        if (signalEnergy < 0.001f) signalEnergy = 0.001f;  // Guard against floating-point undershoot
    }

    for (int lag = minLag; lag <= harmonicMaxLag && (lag - minLag) < 256; lag++) {
        float correlation = 0.0f;
        int count = ossCount_ - lag;

        // FIX: Skip if count <= 0 to prevent division by zero
        if (count <= 0) continue;

        // Use linearized ossLinear buffer (may be adaptively thresholded)
        // ossLinear[0] = oldest sample, ossLinear[ossCount_-1] = newest
        for (int i = 0; i < count; i++) {
            int idx1 = ossCount_ - 1 - i;
            int idx2 = ossCount_ - 1 - i - lag;
            correlation += (ossLinear[idx1] - ossMean) * (ossLinear[idx2] - ossMean);
        }

        correlation /= static_cast<float>(count);
        correlationAtLag[lag - minLag] = correlation;

        // Only track max within fundamental range (not extended harmonic range)
        // to avoid sub-harmonics inflating periodicityStrength_
        if (lag <= maxLag && correlation > maxCorrelation) {
            maxCorrelation = correlation;
            bestLag = lag;
        }
    }

    // Compute periodicity strength (normalized correlation) from raw ACF
    float avgEnergy = signalEnergy / static_cast<float>(ossCount_);
    float normCorrelation = maxCorrelation / (avgEnergy + 0.001f);

    // Smooth periodicity strength updates
    float newStrength = clampf(normCorrelation * 1.5f, 0.0f, 1.0f);
    periodicityStrength_ = periodicityStrength_ * periodicityBlend + newStrength * (1.0f - periodicityBlend);

    // NOTE: Inverse-lag normalization REMOVED (v43).
    // The balanced ACF (correlation /= count, where count = ossCount_ - lag)
    // already corrects for the sample-count bias at longer lags.
    // Applying an additional /lag on top created a DOUBLE normalization that
    // gave lag=20 (~198 BPM) a 1.65x boost over lag=33 (120 BPM), causing
    // systematic upward tempo lock to ~195 BPM on all tracks.
    // BTrack applies one or the other, not both.
    // The Rayleigh prior + comb filter now provide octave discrimination
    // without this artificial upward bias.

    // === BAYESIAN TEMPO FUSION ===
    // Replaces sequential override chain (HPS → pulse train → harmonic disambiguation
    // → comb bank → IOI → FT) with unified multi-signal posterior estimation.
    // Each signal provides a per-bin observation likelihood; the MAP estimate
    // of the posterior becomes the tempo.
    runBayesianTempoFusion(correlationAtLag, correlationSize, minLag, maxLag,
                           avgEnergy, samplesPerMs, shouldPrintDebug,
                           harmonicCorrelationSize);
}

// ===== ODF SMOOTHING =====

float AudioController::smoothOnsetStrength(float raw) {
    int width = odfSmoothWidth;
    if (width < 3) width = 3;
    if (width > ODF_SMOOTH_MAX) width = ODF_SMOOTH_MAX;

    // Reset buffer if width changed (prevents stale data from old width)
    if (odfSmoothIdx_ >= width) {
        for (int i = 0; i < ODF_SMOOTH_MAX; i++) odfSmoothBuffer_[i] = raw;
        odfSmoothIdx_ = 0;
    }

    odfSmoothBuffer_[odfSmoothIdx_] = raw;
    odfSmoothIdx_ = (odfSmoothIdx_ + 1) % width;
    float sum = 0.0f;
    for (int i = 0; i < width; i++) sum += odfSmoothBuffer_[i];
    return sum / width;
}

// ===== BAYESIAN TEMPO STATE =====

void AudioController::buildTransitionMatrix() {
    // Lag-space Gaussian transition matrix (v43: Fix #4).
    // Previous BPM-space Gaussian on lag-uniform grid created asymmetric
    // bandwidth: at low BPM (dense bins in BPM), the Gaussian covered more
    // bins → more probability mass → systematic drift toward slow tempos.
    // Fixed sigma in lag units ensures symmetric bandwidth on the uniform-lag
    // grid. Reference lag = midpoint of range (43 at 66 Hz).
    float lagSigma = bayesLambda * static_cast<float>(CombFilterBank::MAX_LAG + CombFilterBank::MIN_LAG) * 0.5f;
    if (lagSigma < 1.0f) lagSigma = 1.0f;

    for (int i = 0; i < TEMPO_BINS; i++) {
        for (int j = 0; j < TEMPO_BINS; j++) {
            float lagDiff = static_cast<float>(tempoBinLags_[i] - tempoBinLags_[j]);
            float narrow = expf(-0.5f * (lagDiff * lagDiff) / (lagSigma * lagSigma));

            float harmonicBonus = 0.0f;

            float htw = harmonicTransWeight;
            if (htw > 0.0f) {
                float ratio = tempoBinBpms_[i] / tempoBinBpms_[j];

                // 2:1/1:2 octave shortcuts: only for multiplicative path.
                // BTrack pipeline handles 2:1 via comb-on-ACF octave folding.
                if (!btrkPipeline) {
                    // 2:1 (octave up, e.g., 68→136)
                    float diff2x = fabsf(ratio - 2.0f);
                    if (diff2x < 0.15f) {
                        float w = htw * expf(-diff2x * diff2x * 100.0f);
                        if (w > harmonicBonus) harmonicBonus = w;
                    }
                    // 1:2 (octave down, e.g., 136→68)
                    float diffHalf = fabsf(ratio - 0.5f);
                    if (diffHalf < 0.15f) {
                        float w = htw * expf(-diffHalf * diffHalf * 100.0f);
                        if (w > harmonicBonus) harmonicBonus = w;
                    }
                }

                // (harmonicSesqui 3:2/2:3 shortcuts removed v44 — catastrophic regression on fast tracks)
            }

            transMatrix_[i][j] = narrow + harmonicBonus;
        }
    }

    // Column-normalize: each column j must sum to 1 (proper stochastic matrix).
    // Without this, the BPM-space Gaussian on a lag-uniform grid creates more
    // probability mass at low BPM (densely-spaced bins) than high BPM, causing
    // systematic drift toward slow tempos.
    for (int j = 0; j < TEMPO_BINS; j++) {
        float colSum = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            colSum += transMatrix_[i][j];
        }
        if (colSum > 1e-9f) {
            float invSum = 1.0f / colSum;
            for (int i = 0; i < TEMPO_BINS; i++) {
                transMatrix_[i][j] *= invSum;
            }
        }
    }
}

void AudioController::initTempoState() {
    // Copy bin BPMs and lags from CombFilterBank
    for (int i = 0; i < TEMPO_BINS; i++) {
        tempoBinBpms_[i] = combFilterBank_.getFilterBPM(i);
        // Compute lag from BPM: lag = frameRate / (bpm / 60) = 60 * frameRate / bpm
        // At 66 Hz: lag = 3960 / bpm
        tempoBinLags_[i] = static_cast<int>(OSS_FRAMES_PER_MIN / tempoBinBpms_[i] + 0.5f);
    }

    // Initialize prior as Gaussian centered on bayesPriorCenter
    float sum = 0.0f;
    for (int i = 0; i < TEMPO_BINS; i++) {
        float diff = tempoBinBpms_[i] - bayesPriorCenter;
        float sigma = tempoPriorWidth;
        tempoStatePrior_[i] = expf(-0.5f * (diff * diff) / (sigma * sigma));
        sum += tempoStatePrior_[i];
    }
    // Normalize to sum=1
    if (sum > 1e-9f) {
        for (int i = 0; i < TEMPO_BINS; i++) {
            tempoStatePrior_[i] /= sum;
        }
    }

    // Pre-compute static prior (ongoing Gaussian pull toward bayesPriorCenter)
    // This is multiplied into the posterior at every step to prevent sub-harmonic drift.
    // Uses the same Gaussian shape as the initial prior but stored separately.
    for (int i = 0; i < TEMPO_BINS; i++) {
        float diff = tempoBinBpms_[i] - bayesPriorCenter;
        float sigma = tempoPriorWidth;
        tempoStaticPrior_[i] = expf(-0.5f * (diff * diff) / (sigma * sigma));
        if (tempoStaticPrior_[i] < 0.01f) tempoStaticPrior_[i] = 0.01f;  // Floor
    }

    // Pre-compute Gaussian transition matrix (only depends on bin BPMs, bayesLambda, btrkPipeline)
    // Avoids 1600 expf() calls per autocorrelation cycle at runtime.
    buildTransitionMatrix();
    transMatrixLambda_ = bayesLambda;
    transMatrixHarmonic_ = harmonicTransWeight;
    transMatrixBtrkPipeline_ = btrkPipeline;

    // Rayleigh prior peaked at rayleighBpm (BTrack-style perceptual weighting)
    // For candidate period T (lag), Rayleigh(T; sigma) = T/sigma^2 * exp(-T^2 / (2*sigma^2))
    // Peaked at sigma = lag corresponding to rayleighBpm (default 120 BPM = lag 33 at 66 Hz)
    {
        float rayleighSigma = OSS_FRAMES_PER_MIN / rayleighBpm;  // Lag for peak BPM at 66 Hz (default 120 BPM = lag 33)
        float maxR = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            float lag = static_cast<float>(tempoBinLags_[i]);
            rayleighWeight_[i] = (lag / (rayleighSigma * rayleighSigma))
                                * expf(-lag * lag / (2.0f * rayleighSigma * rayleighSigma));
            if (rayleighWeight_[i] > maxR) maxR = rayleighWeight_[i];
        }
        // Normalize to max=1.0
        if (maxR > 0.0f) {
            for (int i = 0; i < TEMPO_BINS; i++) rayleighWeight_[i] /= maxR;
        }
        rayleighBpm_ = rayleighBpm;
    }

    // Clear posterior and debug arrays
    for (int i = 0; i < TEMPO_BINS; i++) {
        tempoStatePost_[i] = tempoStatePrior_[i];
        lastCombObs_[i] = 0.0f;
    }

    bayesBestBin_ = TEMPO_BINS / 2;
    tempoStateInitialized_ = true;
}

int AudioController::findClosestTempoBin(float targetBpm) const {
    int closest = -1;
    float closestDist = 999.0f;
    for (int i = 0; i < TEMPO_BINS; i++) {
        float dist = fabsf(tempoBinBpms_[i] - targetBpm);
        if (dist < closestDist) {
            closestDist = dist;
            closest = i;
        }
    }
    return closest;
}

// Bayesian debug getters
float AudioController::getBayesBestConf() const {
    if (bayesBestBin_ >= 0 && bayesBestBin_ < TEMPO_BINS)
        return tempoStatePost_[bayesBestBin_];
    return 0.0f;
}
float AudioController::getBayesCombObs() const {
    if (bayesBestBin_ >= 0 && bayesBestBin_ < TEMPO_BINS)
        return lastCombObs_[bayesBestBin_];
    return 0.0f;
}

void AudioController::runBayesianTempoFusion(float* correlationAtLag, int correlationSize,
                                              int minLag, int maxLag, float avgEnergy,
                                              float samplesPerMs, bool debugPrint,
                                              int harmonicCorrelationSize) {
    if (!tempoStateInitialized_) return;

    // === 1. PREDICTION STEP ===
    // Rebuild transition matrix if any governing parameter changed.
    if (bayesLambda != transMatrixLambda_ ||
        harmonicTransWeight != transMatrixHarmonic_ ||
        btrkPipeline != transMatrixBtrkPipeline_) {
        buildTransitionMatrix();
        transMatrixLambda_ = bayesLambda;
        transMatrixHarmonic_ = harmonicTransWeight;
        transMatrixBtrkPipeline_ = btrkPipeline;
    }

    // Recompute Rayleigh weights if rayleighBpm changed at runtime
    if (rayleighBpm != rayleighBpm_) {
        float rayleighSigma = OSS_FRAMES_PER_MIN / rayleighBpm;
        float maxR = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            float lag = static_cast<float>(tempoBinLags_[i]);
            rayleighWeight_[i] = (lag / (rayleighSigma * rayleighSigma))
                                * expf(-lag * lag / (2.0f * rayleighSigma * rayleighSigma));
            if (rayleighWeight_[i] > maxR) maxR = rayleighWeight_[i];
        }
        if (maxR > 0.0f) {
            for (int i = 0; i < TEMPO_BINS; i++) rayleighWeight_[i] /= maxR;
        }
        rayleighBpm_ = rayleighBpm;
    }

    // When btrkPipeline is active, prediction is deferred to the Viterbi step (Change 2b).
    // When inactive, use the original Bayesian prediction (sum-product).
    float prediction[TEMPO_BINS];
    if (!btrkPipeline) {
        float predSum = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            prediction[i] = 0.0f;
            for (int j = 0; j < TEMPO_BINS; j++) {
                prediction[i] += tempoStatePrior_[j] * transMatrix_[i][j];
            }
            predSum += prediction[i];
        }
        // Normalize prediction
        if (predSum > 1e-9f) {
            for (int i = 0; i < TEMPO_BINS; i++) prediction[i] /= predSum;
        }
    }

    // === 1b. PERCIVAL HARMONIC PRE-ENHANCEMENT (v45) ===
    // Fold 2nd and 4th harmonic ACF values into fundamental lag BEFORE comb-on-ACF.
    // For each lag L: ACF[L] += w2*ACF[2L] + w4*ACF[4L]
    // Forward iteration is safe: we always read from higher lags (2L > L)
    // that haven't been modified yet when iterating L from minLag upward.
    // This gives fundamental a unique advantage over its double-time harmonic.
    // Source: Percival & Tzanetakis 2014, Essentia percivalenhanceharmonics.cpp
    if (percivalEnhance) {
        for (int li = 0; li < correlationSize; li++) {
            int lag = minLag + li;
            // 2nd harmonic: ACF at 2*lag
            int harm2Idx = 2 * lag - minLag;
            if (harm2Idx >= 0 && harm2Idx < harmonicCorrelationSize) {
                correlationAtLag[li] += percivalWeight2 * correlationAtLag[harm2Idx];
            }
            // 4th harmonic: ACF at 4*lag
            int harm4Idx = 4 * lag - minLag;
            if (harm4Idx >= 0 && harm4Idx < harmonicCorrelationSize) {
                correlationAtLag[li] += percivalWeight4 * correlationAtLag[harm4Idx];
            }
            // 3rd harmonic: SUBTRACT to suppress 3:2 ratio confusion (v48 anti-harmonic)
            // Speech F0 lit: negative weight on odd harmonics distinguishes fundamental from sub-harmonic
            if (percivalWeight3 > 0.0f) {
                int harm3Idx = 3 * lag - minLag;
                if (harm3Idx >= 0 && harm3Idx < harmonicCorrelationSize) {
                    correlationAtLag[li] = fmaxf(0.0f, correlationAtLag[li] - percivalWeight3 * correlationAtLag[harm3Idx]);
                }
            }
        }
    }

    // === 2. FULL-RESOLUTION COMB-ON-ACF (v43: Fix #2) ===
    // Evaluate 4-harmonic comb at EVERY lag in the fundamental range,
    // not just the 20 bin centers. With 20 bins spanning 47 lags (~2.4 lag/bin),
    // the old approach missed ~50% of true peaks due to quantization.
    // Full-resolution evaluation ensures the comb catches peaks regardless
    // of where they fall relative to bin centers.
    // Size must accommodate the full correlationSize range (maxLag - minLag + 1),
    // which can exceed CombFilterBank::MAX_LAG - MIN_LAG + 1 when bpmMin < 60.
    // Use 256 to match correlationAtLag[] capacity.
    static float fullCombAcf[256];
    for (int li = 0; li < correlationSize; li++) {
        int lag = minLag + li;
        float combAcf = 0.0f;
        int harmonicsUsed = 0;
        for (int a = 1; a <= 4; a++) {
            int harmLag = a * lag;
            int harmIdx = harmLag - minLag;
            if (harmIdx >= 0 && harmIdx < harmonicCorrelationSize) {
                // Spread window: sum (2a-1) bins centered at harmIdx
                float sum = 0.0f;
                int count = 0;
                for (int b = 1 - a; b <= a - 1; b++) {
                    int idx = harmIdx + b;
                    if (idx >= 0 && idx < harmonicCorrelationSize) {
                        sum += correlationAtLag[idx];
                        count++;
                    }
                }
                if (count > 0) {
                    combAcf += sum / static_cast<float>(2 * a - 1);
                    harmonicsUsed++;
                }
            }
        }
        fullCombAcf[li] = (harmonicsUsed > 0) ? combAcf : 0.0f;
    }

    // === OCTAVE FOLDING (v43: Fix #3, BTrack-style) ===
    // For each bin at lag L, also add the comb score at lag L/2 (double BPM).
    // The fundamental at L gets evidence from both L and L/2, while the
    // double-time candidate at L/2 only gets evidence from L/2 (its own L/4
    // is out of range). This gives the fundamental a ~2x advantage over
    // its double-time harmonic — the key discrimination missing from our
    // system that caused universal ~195 BPM lock.
    // Only lags >= 2*minLag (i.e., BPM <= ~99) get folding benefit;
    // upper-octave candidates (100-200 BPM) naturally don't get the bonus.
    float acfObs[TEMPO_BINS];
    for (int i = 0; i < TEMPO_BINS; i++) {
        int lag = tempoBinLags_[i];
        int li = lag - minLag;
        float score = 0.0f;
        if (li >= 0 && li < correlationSize) {
            score = fullCombAcf[li];
        }
        // Octave folding: add comb score at half-period (double BPM)
        int halfLag = lag / 2;
        int halfLi = halfLag - minLag;
        if (halfLi >= 0 && halfLi < correlationSize) {
            score += fullCombAcf[halfLi];
        }
        // 3:2 octave folding: add comb score at 2L/3 (sesquialtera harmonic)
        // For lag=46 (86 BPM): 2*46/3=30 (128 BPM comb), giving 86 BPM a boost
        // For lag<30 (>132 BPM): 2L/3 < minLag, safely skipped
        if (fold32Enabled) {
            int twoThirdLag = (lag * 2) / 3;
            int twoThirdLi = twoThirdLag - minLag;
            if (twoThirdLi >= 0 && twoThirdLi < correlationSize) {
                score += fullCombAcf[twoThirdLi];
            }
        }
        acfObs[i] = score * rayleighWeight_[i] / (avgEnergy + 0.001f);
        if (acfObs[i] < 0.01f) acfObs[i] = 0.01f;
    }
    // Exponentiate by weight (only for multiplicative path — pipeline uses raw comb-on-ACF)
    if (!btrkPipeline && bayesAcfWeight != 1.0f) {
        for (int i = 0; i < TEMPO_BINS; i++) {
            acfObs[i] = powf(acfObs[i], bayesAcfWeight);
        }
    }

    // BTrack pipeline: adaptive threshold on comb-on-ACF (moving-average subtraction + HWR)
    // Removes background level, leaving only genuine periodicity peaks.
    // btrkThreshWindow controls half-window size (0=off, 1-5 bins each side).
    // Two-pass like BTrack: compute thresholds from original data, then apply.
    if (btrkPipeline && btrkThreshWindow > 0) {
        int threshHalf = btrkThreshWindow;
        float thresh[TEMPO_BINS];
        // Pass 1: compute local-mean threshold from unmodified acfObs
        for (int i = 0; i < TEMPO_BINS; i++) {
            float sum = 0.0f;
            int count = 0;
            for (int j = i - threshHalf; j <= i + threshHalf; j++) {
                if (j >= 0 && j < TEMPO_BINS) {
                    sum += acfObs[j];
                    count++;
                }
            }
            thresh[i] = sum / static_cast<float>(count);
        }
        // Pass 2: subtract threshold + half-wave rectify
        for (int i = 0; i < TEMPO_BINS; i++) {
            float val = acfObs[i] - thresh[i];
            acfObs[i] = (val > 0.0f) ? val : 0.0f;
        }
    }

    // (FT observation removed v52 — dead code since v28)

    // === 4. COMB FILTER BANK OBSERVATION ===
    float combObs[TEMPO_BINS];
    if (!btrkPipeline && combBankEnabled) {
        for (int i = 0; i < TEMPO_BINS; i++) {
            combObs[i] = combFilterBank_.getFilterEnergy(i);
            if (combObs[i] < 0.01f) combObs[i] = 0.01f;  // Floor
        }
        if (bayesCombWeight != 1.0f) {
            for (int i = 0; i < TEMPO_BINS; i++) {
                combObs[i] = powf(combObs[i], bayesCombWeight);
            }
        }
    } else {
        for (int i = 0; i < TEMPO_BINS; i++) combObs[i] = 1.0f;
    }
    for (int i = 0; i < TEMPO_BINS; i++) lastCombObs_[i] = combObs[i];

    // (IOI observation removed v52 — dead code since v28)

    // === 6. COMBINE PREDICTION AND OBSERVATIONS ===
    if (btrkPipeline) {
        // Viterbi max-product (BTrack-style): for each candidate tempo bin,
        // find the maximum prior[j] * transition[j→i], then multiply by observation.
        // Uses only acfObs (comb-on-ACF with adaptive threshold) — no FT/IOI/comb.
        float postSum = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            float maxPred = 0.0f;
            for (int j = 0; j < TEMPO_BINS; j++) {
                float val = tempoStatePrior_[j] * transMatrix_[i][j];
                if (val > maxPred) maxPred = val;
            }
            tempoStatePost_[i] = maxPred * acfObs[i];
            postSum += tempoStatePost_[i];
        }
        // Normalize posterior
        if (postSum > 1e-9f) {
            for (int i = 0; i < TEMPO_BINS; i++) tempoStatePost_[i] /= postSum;
        } else {
            for (int i = 0; i < TEMPO_BINS; i++) tempoStatePost_[i] = 1.0f / TEMPO_BINS;
        }
    } else {
        // Original multiplicative Bayesian fusion (sum-product prediction × all observations)
        // Apply weight exponent to static prior (0=off, 1=standard, >1=stronger pull)
        float weightedPrior[TEMPO_BINS];
        if (bayesPriorWeight != 0.0f) {
            if (bayesPriorWeight == 1.0f) {
                for (int i = 0; i < TEMPO_BINS; i++) weightedPrior[i] = tempoStaticPrior_[i];
            } else {
                for (int i = 0; i < TEMPO_BINS; i++) weightedPrior[i] = powf(tempoStaticPrior_[i], bayesPriorWeight);
            }
        } else {
            for (int i = 0; i < TEMPO_BINS; i++) weightedPrior[i] = 1.0f;  // Disabled
        }

        float postSum = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            tempoStatePost_[i] = prediction[i] * weightedPrior[i] * acfObs[i] * combObs[i];
            postSum += tempoStatePost_[i];
        }
        // Normalize posterior
        if (postSum > 1e-9f) {
            for (int i = 0; i < TEMPO_BINS; i++) tempoStatePost_[i] /= postSum;
        } else {
            // Degenerate — reset to uniform
            for (int i = 0; i < TEMPO_BINS; i++) tempoStatePost_[i] = 1.0f / TEMPO_BINS;
        }
    }

    // === ONSET-DENSITY OCTAVE DISCRIMINATOR ===
    // Penalizes bins where the implied transients-per-beat is implausible.
    // For dance music at BPM B with onset density D (onsets/sec):
    //   transients_per_beat = D / (B/60) = 60*D/B
    // If this ratio is outside [densityMinPerBeat, densityMaxPerBeat],
    // apply a Gaussian penalty to suppress implausible tempos.
    if (densityOctaveEnabled && onsetDensity_ > 0.1f) {
        float densitySum = 0.0f;
        for (int i = 0; i < TEMPO_BINS; i++) {
            float bpm = tempoBinBpms_[i];
            float transPerBeat = 60.0f * onsetDensity_ / bpm;
            float penalty = 1.0f;

            if (densityTarget > 0.0f) {
                // Target-density mode: Gaussian centered on densityTarget trans/beat.
                // Penalizes all bins proportionally to distance from target.
                // Provides smooth, symmetric discrimination without hard thresholds.
                float diff = (transPerBeat - densityTarget) / densityTarget;
                penalty = expf(-densityPenaltyExp * diff * diff);
            } else if (transPerBeat < densityMinPerBeat) {
                // Legacy min/max mode: too few transients
                float diff = (densityMinPerBeat - transPerBeat) / densityMinPerBeat;
                penalty = expf(-densityPenaltyExp * diff * diff);
            } else if (transPerBeat > densityMaxPerBeat) {
                // Legacy min/max mode: too many transients
                float diff = (transPerBeat - densityMaxPerBeat) / densityMaxPerBeat;
                penalty = expf(-densityPenaltyExp * diff * diff);
            }

            tempoStatePost_[i] *= penalty;
            densitySum += tempoStatePost_[i];
        }
        // Re-normalize
        if (densitySum > 1e-9f) {
            for (int i = 0; i < TEMPO_BINS; i++) tempoStatePost_[i] /= densitySum;
        }
    }

    // Posterior uniform floor: mix with uniform to prevent hard mode lock.
    // With a tight transition matrix (lambda=0.07), the prediction step drives
    // distant bins to zero, making tempo jumps (e.g., 68→136 BPM) mathematically
    // impossible. Mixing in a small uniform component ensures every bin retains
    // non-zero probability, allowing strong observations to gradually pull the
    // posterior toward the correct tempo.
    if (posteriorFloor > 0.0f) {
        float alpha = clampf(posteriorFloor, 0.0f, 0.5f);
        float uniform = alpha / static_cast<float>(TEMPO_BINS);
        float scale = 1.0f - alpha;
        for (int i = 0; i < TEMPO_BINS; i++) {
            tempoStatePost_[i] = scale * tempoStatePost_[i] + uniform;
        }
    }

    // === 7. EXTRACT MAP ESTIMATE with per-sample ACF harmonic disambiguation ===
    int bestBin = 0;
    float bestPost = tempoStatePost_[0];
    for (int i = 1; i < TEMPO_BINS; i++) {
        if (tempoStatePost_[i] > bestPost) {
            bestPost = tempoStatePost_[i];
            bestBin = i;
        }
    }
    int preCorrectionBin = bestBin;  // Save for disambiguation feedback

    // Per-sample ACF harmonic disambiguation (skipped in BTrack pipeline — comb-on-ACF
    // with adaptive threshold provides structural octave handling, making post-hoc
    // correction redundant and a source of overcorrection).
    if (!btrkPipeline) {
        const float HARMONIC_2X_THRESH = harmonic2xThresh;   // Half-lag ACF ratio for 2x BPM correction
        const float HARMONIC_1_5X_THRESH = harmonic15xThresh; // 2/3-lag ACF ratio for 1.5x BPM correction
        int bestLag = tempoBinLags_[bestBin];
        int halfLag = bestLag / 2;          // 2x BPM
        int twoThirdLag = bestLag * 2 / 3;  // 1.5x BPM

        // Get raw ACF at the MAP bin's lag
        int bestLagIdx = bestLag - minLag;
        float bestAcf = (bestLagIdx >= 0 && bestLagIdx < correlationSize)
                        ? correlationAtLag[bestLagIdx] : 0.0f;

        if (bestAcf > 0.001f) {
            // Check half-lag (2x BPM): if strong, prefer double tempo
            bool corrected = false;
            int halfIdx = halfLag - minLag;
            if (halfIdx >= 0 && halfIdx < correlationSize) {
                float halfAcf = correlationAtLag[halfIdx];
                if (halfAcf > HARMONIC_2X_THRESH * bestAcf) {
                    float halfBpm = OSS_FRAMES_PER_MIN / static_cast<float>(halfLag);
                    int closest = findClosestTempoBin(halfBpm);
                    if (closest >= 0 && fabsf(tempoBinBpms_[closest] - halfBpm) < halfBpm * 0.1f) {
                        bestBin = closest;
                        corrected = true;
                    }
                }
            }

            // Check 2/3-lag (1.5x BPM): if strong, prefer 3/2 tempo
            if (!corrected) {
                int twoThirdIdx = twoThirdLag - minLag;
                if (twoThirdIdx >= 0 && twoThirdIdx < correlationSize) {
                    float twoThirdAcf = correlationAtLag[twoThirdIdx];
                    if (twoThirdAcf > HARMONIC_1_5X_THRESH * bestAcf) {
                        float twoThirdBpm = OSS_FRAMES_PER_MIN / static_cast<float>(twoThirdLag);
                        int closest = findClosestTempoBin(twoThirdBpm);
                        if (closest >= 0 && fabsf(tempoBinBpms_[closest] - twoThirdBpm) < twoThirdBpm * 0.1f) {
                            bestBin = closest;
                        }
                    }
                }
            }
        }
    }
    // === DOWNWARD HARMONIC CORRECTION ===
    // Corrects 3:2 and 2:1 lock by checking if a slower fundamental
    // has comparable ACF support. Posterior-gated: only overrides the MAP
    // when the Bayesian posterior shows genuine ambiguity (fundamental bin
    // has at least 50% of the MAP probability, i.e., posterior >= 0.5 × MAP).
    // This prevents false correction when the prior/density/comb filters
    // confidently favor the MAP.
    // Disabled by default: overcorrects 136 BPM trance to ~98 BPM (3:2 ambiguity).
    if (downwardCorrectEnabled) {
        int bestLag = tempoBinLags_[bestBin];
        int bestLagIdx = bestLag - minLag;
        float bestAcf = (bestLagIdx >= 0 && bestLagIdx < correlationSize)
                        ? correlationAtLag[bestLagIdx] : 0.0f;
        float mapPosterior = tempoStatePost_[bestBin];

        if (bestAcf > 0.001f) {
            // Check 3:2 downward: fundamental at 1.5x the lag (2/3 BPM)
            int fundamentalLag = (bestLag * 3 + 1) / 2;  // Round instead of truncate for odd lags
            int fundIdx = fundamentalLag - minLag;
            bool corrected = false;
            if (fundIdx >= 0 && fundIdx < correlationSize) {
                float fundAcf = correlationAtLag[fundIdx];
                // Lag-compensated: compare raw (unnormalized) ACF values
                float threshold = 1.0f * static_cast<float>(bestLag) / static_cast<float>(fundamentalLag);
                if (fundAcf > threshold * bestAcf) {
                    float fundBpm = OSS_FRAMES_PER_MIN / static_cast<float>(fundamentalLag);
                    int closest = findClosestTempoBin(fundBpm);
                    if (closest >= 0 && fabsf(tempoBinBpms_[closest] - fundBpm) < fundBpm * 0.1f) {
                        // Posterior gate: only correct if posterior shows ambiguity
                        float fundPosterior = tempoStatePost_[closest];
                        if (fundPosterior > 0.5f * mapPosterior) {
                            bestBin = closest;
                            corrected = true;
                        }
                    }
                }
            }

            // Check 2:1 downward: fundamental at 2x the lag (1/2 BPM)
            if (!corrected) {
                int doubleLag = bestLag * 2;
                int dblIdx = doubleLag - minLag;
                if (dblIdx >= 0 && dblIdx < correlationSize) {
                    float dblAcf = correlationAtLag[dblIdx];
                    float dblThreshold = 1.0f * static_cast<float>(bestLag) / static_cast<float>(doubleLag);
                    if (dblAcf > dblThreshold * bestAcf) {
                        float dblBpm = OSS_FRAMES_PER_MIN / static_cast<float>(doubleLag);
                        int closest = findClosestTempoBin(dblBpm);
                        if (closest >= 0 && fabsf(tempoBinBpms_[closest] - dblBpm) < dblBpm * 0.1f) {
                            float dblPosterior = tempoStatePost_[closest];
                            if (dblPosterior > 0.5f * mapPosterior) {
                                bestBin = closest;
                            }
                        }
                    }
                }
            }
        }
    }

    bayesBestBin_ = bestBin;

    // Disambiguation feedback: nudge posterior toward corrected bin.
    // Active for both pipelines — the downward harmonic correction needs
    // feedback to prevent the posterior from drifting back to 3:2 lock.
    if (bestBin != preCorrectionBin && disambigNudge > 0.0f) {
        float nudge = clampf(disambigNudge, 0.0f, 0.5f);
        float transfer = tempoStatePost_[preCorrectionBin] * nudge;
        tempoStatePost_[preCorrectionBin] -= transfer;
        tempoStatePost_[bestBin] += transfer;
    }

    // Quadratic interpolation for sub-bin precision
    float interpolatedBpm = tempoBinBpms_[bestBin];
    if (bestBin > 0 && bestBin < TEMPO_BINS - 1) {
        float y0 = tempoStatePost_[bestBin - 1];
        float y1 = tempoStatePost_[bestBin];
        float y2 = tempoStatePost_[bestBin + 1];
        float denom = 2.0f * (2.0f * y1 - y0 - y2);
        if (fabsf(denom) > 1e-9f) {
            float delta = (y0 - y2) / denom;  // Fractional bin offset (-0.5 to +0.5)
            delta = clampf(delta, -0.5f, 0.5f);
            // Linearly interpolate BPM between adjacent bin centers
            if (delta > 0.0f) {
                interpolatedBpm = tempoBinBpms_[bestBin] + delta * (tempoBinBpms_[bestBin + 1] - tempoBinBpms_[bestBin]);
            } else {
                interpolatedBpm = tempoBinBpms_[bestBin] + delta * (tempoBinBpms_[bestBin] - tempoBinBpms_[bestBin - 1]);
            }
        }
    }

    // === 8. DEBUG OUTPUT ===
    if (debugPrint) {
        Serial.print(F("{\"type\":\"RHYTHM_DEBUG2\",\"bpm\":"));
        Serial.print(interpolatedBpm, 1);
        Serial.print(F(",\"bb\":"));
        Serial.print(bestBin);
        Serial.print(F(",\"bc\":"));
        Serial.print(bestPost, 4);
        Serial.print(F(",\"acf\":"));
        Serial.print(acfObs[bestBin], 3);
        Serial.print(F(",\"cb\":"));
        Serial.print(combObs[bestBin], 3);
        Serial.print(F(",\"ms\":"));
        Serial.print(odfMeanSubEnabled ? 1 : 0);
        Serial.println(F("}"));
    }

    // === 9. UPDATE TEMPO ===
    // (externalTempoActive check removed v64 — PF/HMM/forward filter removed)
    if (periodicityStrength_ > 0.25f) {
        float newBpm = clampf(interpolatedBpm, bpmMin, bpmMax);

        // Smooth tempo update (EMA)
        bpm_ = bpm_ * tempoSmoothingFactor + newBpm * (1.0f - tempoSmoothingFactor);

        beatPeriodMs_ = 60000.0f / bpm_;

        // Update beat period in samples for CBSS
        int newPeriodSamples = static_cast<int>(beatPeriodMs_ * samplesPerMs + 0.5f);
        BLINKY_ASSERT(newPeriodSamples >= 10 && newPeriodSamples <= OSS_BUFFER_SIZE / 2,
                       "AudioController: beat period out of range");
        if (newPeriodSamples < 10) newPeriodSamples = 10;
        if (newPeriodSamples > OSS_BUFFER_SIZE / 2) newPeriodSamples = OSS_BUFFER_SIZE / 2;

        // Phase 2.1: Beat-boundary tempo — defer period update to next beat fire
        // BTrack only updates tempo at beat time, preventing mid-beat period discontinuities
        if (beatBoundaryTempo && beatCount_ > 0) {
            pendingBeatPeriod_ = newPeriodSamples;
            if (debugPrint && newPeriodSamples != beatPeriodSamples_) {
                Serial.print(F("{\"type\":\"BEAT_TEMPO_DEFER\",\"cur\":"));
                Serial.print(beatPeriodSamples_);
                Serial.print(F(",\"pend\":"));
                Serial.print(newPeriodSamples);
                Serial.println(F("}"));
            }
        } else {
            beatPeriodSamples_ = newPeriodSamples;
        }

        // (ensemble_.getFusion().setTempoHint removed v67 — BandFlux pipeline removed)

        // Update tempo velocity if BPM changed significantly
        if (fabsf(bpm_ - prevBpm_) / (prevBpm_ > 1.0f ? prevBpm_ : 1.0f) > tempoChangeThreshold) {
            float dt = autocorrPeriodMs / 1000.0f;
            updateTempoVelocity(bpm_, dt);
        }
    }

    // (PLP phase extraction step removed v44 — zero effect, redundant with onset snap)

    // === 10. SAVE POSTERIOR AS NEXT PRIOR ===
    for (int i = 0; i < TEMPO_BINS; i++) {
        tempoStatePrior_[i] = tempoStatePost_[i];
    }
}

// (computeFTObservations removed v52 — dead code since v28)
// (computeIOIObservations removed v52 — dead code since v28)

// ===== LOG-GAUSSIAN WEIGHT COMPUTATION =====

void AudioController::recomputeLogGaussianWeights(int T) {
    // v45: Use effectiveTightness_ (modulated by adaptive tightness) instead of raw cbssTightness
    if (T == logGaussianLastT_ && effectiveTightness_ == logGaussianLastTight_) return;
    logGaussianLastT_ = T;
    logGaussianLastTight_ = effectiveTightness_;
    int searchMin = T / 2;
    int searchMax = T * 2;
    logGaussianWeightsSize_ = searchMax - searchMin + 1;
    if (logGaussianWeightsSize_ > MAX_BEAT_PERIOD * 2) {
        logGaussianWeightsSize_ = MAX_BEAT_PERIOD * 2;
    }
    for (int i = 0; i < logGaussianWeightsSize_; i++) {
        int offset = searchMin + i;
        // Log-Gaussian: peak at offset==T, decay for offsets away from T
        float logRatio = logf((float)offset / (float)T);
        float a = effectiveTightness_ * logRatio;
        logGaussianWeights_[i] = expf(-0.5f * a * a);
    }
}

// ===== COUNTER MANAGEMENT =====

void AudioController::renormalizeCounters() {
    // Prevent signed integer overflow (UB in C++).
    // At 60 Hz, sampleCounter_ reaches 1M after ~4.6 hours.
    // Renormalize counters to keep values small while preserving their
    // differences (which is all the CBSS/IOI logic depends on).
    if (sampleCounter_ > 1000000) {
        // Expected after ~4.6 hours of continuous operation — not an error
        int shift = sampleCounter_ - OSS_BUFFER_SIZE;
        sampleCounter_ -= shift;
        lastBeatSample_ -= shift;
        lastTransientSample_ -= shift;
        if (lastBeatSample_ < 0) lastBeatSample_ = 0;
        if (lastTransientSample_ < 0) lastTransientSample_ = -1;
    }
}

// ===== CBSS BEAT TRACKING =====

void AudioController::updateCBSS(float onsetStrength) {
    // CBSS[n] = (1-alpha)*OSS[n] + alpha*max_weighted(CBSS[n-2T : n-T/2])
    // Uses log-Gaussian transition weighting (BTrack-style) instead of flat max
    int T = beatPeriodSamples_;
    if (T < 10) T = 10;

    // Adaptive tightness (v45): modulate cbssTightness based on onset confidence
    // Strong onsets → looser tightness (allow phase correction toward real beats)
    // Weak onsets → tighter (resist noise-driven drift during quiet passages)
    if (adaptiveTightnessEnabled && cbssMean_ > 0.001f) {
        float ossRatio = onsetStrength / cbssMean_;
        float mult;
        if (ossRatio >= tightnessConfThreshHigh) {
            mult = tightnessLowMult;
        } else if (ossRatio <= tightnessConfThreshLow) {
            mult = tightnessHighMult;
        } else {
            // Linear interpolation between thresholds to avoid step-function jitter
            float t = (ossRatio - tightnessConfThreshLow)
                    / (tightnessConfThreshHigh - tightnessConfThreshLow);
            mult = tightnessHighMult + t * (tightnessLowMult - tightnessHighMult);
        }
        // Quantize to 8 levels so log-Gaussian weight cache can hit.
        // Without quantization, effectiveTightness_ changes nearly every frame,
        // causing recomputeLogGaussianWeights() to re-run at 66 Hz (~250 float ops).
        float raw = cbssTightness * mult;
        float step = (cbssTightness * tightnessHighMult - cbssTightness * tightnessLowMult) / 8.0f;
        if (step > 0.01f) {
            effectiveTightness_ = cbssTightness * tightnessLowMult + roundf((raw - cbssTightness * tightnessLowMult) / step) * step;
        } else {
            effectiveTightness_ = raw;
        }
    } else {
        effectiveTightness_ = cbssTightness;
    }

    recomputeLogGaussianWeights(T);

    float maxWeightedCBSS = 0.0f;
    int searchMin = T / 2;

    for (int i = 0; i < logGaussianWeightsSize_; i++) {
        int offset = searchMin + i;
        int idx = sampleCounter_ - offset;
        if (idx < 0) continue;
        float val = cbssBuffer_[idx % OSS_BUFFER_SIZE] * logGaussianWeights_[i];
        if (val > maxWeightedCBSS) maxWeightedCBSS = val;
    }

    // NN ODF adaptation: lower alpha gives onsets more weight in the CBSS recursion.
    // BandFlux peaks at ~2-5, so at alpha=0.9: onset weight = 0.1*3 = 0.3.
    // NN peaks at ~0.6-0.9, so at alpha=0.9: onset weight = 0.1*0.8 = 0.08 (too weak).
    // At alpha=0.8: onset weight = 0.2*0.8 = 0.16, still conservative but 2x stronger.
    // Only override if user hasn't explicitly lowered cbssAlpha below the NN target.
    float baseAlpha = (nnActive_ && cbssAlpha > 0.8f) ? 0.8f : cbssAlpha;

    // During warmup (first N beats), use lower alpha so onsets contribute more
    // to the CBSS. This gives ~4x more onset weight during initial phase acquisition,
    // preventing random phase lock from weak early CBSS values.
    float effectiveAlpha = baseAlpha;
    if (cbssWarmupBeats > 0 && beatCount_ < cbssWarmupBeats) {
        effectiveAlpha = baseAlpha * 0.55f;  // e.g., 0.9 * 0.55 = 0.495 → onset gets ~50% weight
    }

    float cbssVal = (1.0f - effectiveAlpha) * onsetStrength + effectiveAlpha * maxWeightedCBSS;
    cbssBuffer_[sampleCounter_ % OSS_BUFFER_SIZE] = cbssVal;

    // Update running mean of CBSS for adaptive threshold
    // EMA alpha ≈ 1/120 ≈ 0.008 → tau ~120 frames (~2 seconds at 60 Hz)
    cbssMean_ = cbssMean_ * (1.0f - cbssMeanAlpha) + cbssVal * cbssMeanAlpha;

    sampleCounter_++;
    renormalizeCounters();
}

void AudioController::predictBeat() {
    int T = beatPeriodSamples_;
    if (T < 10) T = 10;
    if (T > MAX_BEAT_PERIOD) T = MAX_BEAT_PERIOD;

    // Precompute beat expectation Gaussian if T changed
    if (T != beatExpectationLastT_) {
        beatExpectationLastT_ = T;
        beatExpectationSize_ = T;  // Window covers one full beat period ahead
        float halfT = T / 2.0f;
        float sigma = halfT;  // Gaussian sigma = half beat period
        for (int i = 0; i < beatExpectationSize_; i++) {
            float diff = (i + 1) - halfT;  // Center at T/2
            beatExpectationWindow_[i] = expf(-diff * diff / (2.0f * sigma * sigma));
        }
    }

    // Synthesize future CBSS values by feeding zero onset strength
    // into the CBSS recursion for beatExpectationSize_ frames ahead
    float futureCBSS[MAX_BEAT_PERIOD];

    recomputeLogGaussianWeights(T);

    int simCounter = sampleCounter_;
    for (int i = 0; i < beatExpectationSize_; i++) {
        // Same CBSS formula but with zero onset
        float maxWeightedCBSS = 0.0f;
        int searchMin = T / 2;
        for (int j = 0; j < logGaussianWeightsSize_; j++) {
            int offset = searchMin + j;
            int idx = simCounter - offset;
            if (idx < 0) continue;
            float val;
            if (idx >= sampleCounter_) {
                // Read from our synthesized future
                int futureIdx = idx - sampleCounter_;
                // cppcheck-suppress knownConditionTrueFalse  ; defensive guard for clarity
                val = (futureIdx >= 0 && futureIdx < i) ? futureCBSS[futureIdx] : 0.0f;
            } else {
                val = cbssBuffer_[idx % OSS_BUFFER_SIZE];
            }
            val *= logGaussianWeights_[j];
            if (val > maxWeightedCBSS) maxWeightedCBSS = val;
        }
        // alpha=1.0 for future synthesis (pure momentum, no new onset)
        futureCBSS[i] = cbssAlpha * maxWeightedCBSS;
        simCounter++;
    }

    // Find argmax of Gaussian-weighted future CBSS
    float maxScore = 0.0f;
    int bestOffset = beatExpectationSize_ / 2;  // Default to center
    for (int i = 0; i < beatExpectationSize_; i++) {
        float score = futureCBSS[i] * beatExpectationWindow_[i];
        if (score > maxScore) {
            maxScore = score;
            bestOffset = i;
        }
    }

    // Apply timing offset to compensate for ODF smoothing + CBSS propagation delay
    int adjusted = bestOffset + 1 - static_cast<int>(beatTimingOffset);
    if (adjusted < 1) adjusted = 1;  // Never schedule in the past
    timeToNextBeat_ = adjusted;
    timeToNextPrediction_ = timeToNextBeat_ + T / 2;  // Next prediction at midpoint
    lastBeatWasPredicted_ = true;  // Mark that prediction refined the next beat time
}

void AudioController::checkOctaveAlternative() {
    // Shadow CBSS octave checker: compare CBSS score at current tempo T
    // vs double-time T/2 and half-time 2T. Switches if the alternative
    // scores significantly better. Fixes both double-time and half-time lock.
    // Inspired by BeatNet's "tempo investigators."
    int T = beatPeriodSamples_;
    if (T < 20) return;  // Can't halve below minimum period

    int halfT = T / 2;
    int doubleT = T * 2;

    // Score each tempo by summing CBSS values at expected beat positions.
    // cbssBuffer_ is circular with sampleCounter_ as the write head;
    // idx % OSS_BUFFER_SIZE maps absolute sample indices to buffer positions.
    // Use a FIXED number of beats (4) for all candidates so that faster tempos
    // don't get more samples (which biased mean scores toward fast tempos).
    static constexpr int OCTAVE_CHECK_BEATS = 4;

    float scoreT = 0.0f;
    int countT = 0;

    // Score at current tempo T: sum CBSS at positions spaced T apart
    for (int b = 0; b < OCTAVE_CHECK_BEATS; b++) {
        int idx = sampleCounter_ - 1 - b * T;
        if (idx >= 0) {
            scoreT += cbssBuffer_[idx % OSS_BUFFER_SIZE];
            countT++;
        }
    }
    if (countT > 0) scoreT /= static_cast<float>(countT);

    // Helper: score a candidate period by averaging CBSS at expected beat positions
    // Uses the same fixed beat count for fair comparison across tempos.
    auto scorePeriod = [&](int period) -> float {
        float score = 0.0f;
        int count = 0;
        for (int b = 0; b < OCTAVE_CHECK_BEATS; b++) {
            int idx = sampleCounter_ - 1 - b * period;
            if (idx >= 0) {
                score += cbssBuffer_[idx % OSS_BUFFER_SIZE];
                count++;
            }
        }
        return (count > 0) ? score / static_cast<float>(count) : 0.0f;
    };

    // Helper: check if a candidate period is valid (within BPM range and buffer)
    auto isValidPeriod = [&](int period) -> bool {
        if (period < 10 || period >= OSS_BUFFER_SIZE / 2) return false;
        float candidateBpm = OSS_FRAMES_PER_MIN / static_cast<float>(period);
        return candidateBpm >= bpmMin && candidateBpm <= bpmMax;
    };

    // Try a candidate: score it and switch if it beats current tempo
    auto tryCandidate = [&](int period) -> bool {
        if (!isValidPeriod(period)) return false;
        float score = scorePeriod(period);
        if (scoreT > 0.001f && score > octaveScoreRatio * scoreT) {
            switchTempo(period);
            return true;
        }
        return false;
    };

    // --- Check double-time (T/2 = faster) ---
    if (tryCandidate(halfT)) return;

    // --- Check half-time (2T = slower) ---
    if (tryCandidate(doubleT)) return;

    // --- Check sesquialtera ratios (3:2) ---
    if (sesquiCheckEnabled) {
        // 3T/2 (slower): e.g., 128 BPM -> 84 BPM
        if (tryCandidate((T * 3) / 2)) return;
        // 2T/3 (faster): e.g., 84 BPM -> 128 BPM
        if (tryCandidate((T * 2) / 3)) return;
    }
}

// (checkPhaseAlignment() removed v44 — net-negative on 18-track validation)

void AudioController::switchTempo(int newPeriodSamples) {
    beatPeriodSamples_ = newPeriodSamples;
    float newBpm = OSS_FRAMES_PER_MIN / static_cast<float>(newPeriodSamples);
    bpm_ = clampf(newBpm, bpmMin, bpmMax);
    beatPeriodMs_ = 60000.0f / bpm_;

    // Also update Bayesian posterior — nudge toward the new tempo bin
    int newBin = findClosestTempoBin(bpm_);
    if (newBin >= 0 && newBin < TEMPO_BINS) {
        // Transfer mass from old best bin to new (tempoNudge=0.8 default)
        float transfer = tempoStatePost_[bayesBestBin_] * tempoNudge;
        tempoStatePost_[bayesBestBin_] -= transfer;
        tempoStatePost_[newBin] += transfer;
        // Update prior to match
        for (int i = 0; i < TEMPO_BINS; i++) {
            tempoStatePrior_[i] = tempoStatePost_[i];
        }
        bayesBestBin_ = newBin;
    }

    // Reset countdown for new period
    timeToNextBeat_ = newPeriodSamples;
    timeToNextPrediction_ = newPeriodSamples / 2;
}

// (initHmmState, updateHmmForward, buildHmmTransitionMatrix removed v53 — joint HMM dead code)

// (updatePhaseTracker removed v64 — HMM phase tracker A/B tested, no benefit over CBSS)
// (detectHmmBeat removed v64 — HMM beat detection A/B tested, no benefit over CBSS)

// (initForwardFilter/updateForwardFilter/detectForwardFilterBeat removed v64 —
//  forward filter A/B tested: severe half-time bias, 17/18 octave errors)


// (Particle filter beat tracking removed v64 — all PF functions: pfRandom, pfGaussianRandom,
//  initParticleFilter, pfUpdate, pfPredict, pfUpdateWeights, pfResample, pfExtractConsensus,
//  pfDetectBeat. Never outperformed CBSS in A/B testing.)

// (Multi-agent beat tracking removed v64 — initBeatAgents/detectBeatMultiAgent never
//  outperformed single-agent CBSS predict+countdown in A/B testing)

// (checkMetricalContrast removed v64 — A/B tested, no benefit for octave disambiguation)

// (checkTemplateMatch removed v64 — A/B tested: baseline wins 10 vs 8, no net benefit)

// (checkSubbeatAlternation removed v64 — A/B tested: baseline wins 10 vs 8, no net benefit)

void AudioController::detectBeat() {
    uint32_t nowMs = time_.millis();

    timeToNextBeat_--;
    timeToNextPrediction_--;

    bool beatDetected = false;

    // Run prediction at beat midpoint
    if (timeToNextPrediction_ <= 0) {
        predictBeat();
    }

    // Beat declared when countdown reaches threshold AND CBSS is above adaptive threshold.
    // Bidirectional snap: delay declaration by 3 frames (~45ms at 66 Hz) so the
    // backward-only onset snap window covers frames arriving after the predicted beat time.
    int beatDeclareDelay = bidirectionalSnap ? 3 : 0;
    if (timeToNextBeat_ <= -beatDeclareDelay) {
        // Adaptive threshold: suppress beats during silence/breakdowns
        // When cbssThresholdFactor > 0, require current CBSS > factor * running mean.
        // Grace period: bypass threshold for the first few beats so silence→music
        // transitions aren't suppressed while cbssMean_ catches up.
        static constexpr int CBSS_GRACE_PERIOD_BEATS = 5;
        float currentCBSS = cbssBuffer_[(sampleCounter_ > 0 ? sampleCounter_ - 1 : 0) % OSS_BUFFER_SIZE];
        bool cbssAboveThreshold = (cbssThresholdFactor <= 0.0f) ||
                                   (beatCount_ < CBSS_GRACE_PERIOD_BEATS) ||
                                   (currentCBSS > cbssThresholdFactor * cbssMean_);

        if (cbssAboveThreshold) {
            // Save previous beat anchor BEFORE onset snap overwrites it (needed by PLL)
            int prevBeatSample = lastBeatSample_;

            // Onset snap: anchor the beat at the strongest nearby onset
            // in the raw OSS buffer, rather than at the exact countdown position.
            // This corrects small phase errors each beat cycle, pulling the
            // beat grid toward actual onset positions over time.
            if (onsetSnapWindow > 0 && ossCount_ > 0) {
                int W = static_cast<int>(onsetSnapWindow);
                float bestOSS = -1.0f;
                int bestSnapOffset = 0;
                for (int d = 0; d <= W; d++) {
                    int idx = sampleCounter_ - 1 - d;
                    if (idx < 0) break;
                    float oss = ossBuffer_[idx % OSS_BUFFER_SIZE];
                    if (oss > bestOSS) {
                        bestOSS = oss;
                        bestSnapOffset = d;
                    }
                }
                // Hysteresis: prefer previous snap position if nearly as strong (>80%).
                // Only apply after consecutive beats at the same offset, so
                // syncopated music can still shift the snap point immediately.
                static constexpr uint8_t SNAP_HYSTERESIS_MIN_CONSISTENCY = 2;
                if (bestOSS > 0.0f && abs(bestSnapOffset - lastSnapOffset_) > 1
                    && snapConsistencyCount_ >= SNAP_HYSTERESIS_MIN_CONSISTENCY) {
                    int prevIdx = sampleCounter_ - 1 - lastSnapOffset_;
                    if (prevIdx >= 0 && lastSnapOffset_ <= W) {
                        float prevOSS = ossBuffer_[prevIdx % OSS_BUFFER_SIZE];
                        if (prevOSS > bestOSS * snapHysteresis) {
                            bestSnapOffset = lastSnapOffset_;
                        }
                    }
                }
                // Track consistency: increment if offset unchanged, reset otherwise
                if (bestSnapOffset == lastSnapOffset_) {
                    if (snapConsistencyCount_ < 255) snapConsistencyCount_++;
                } else {
                    snapConsistencyCount_ = 0;
                }
                lastSnapOffset_ = bestSnapOffset;
                lastBeatSample_ = sampleCounter_ - bestSnapOffset;
            } else {
                lastBeatSample_ = sampleCounter_;
                // Reset snap state when onset snap is disabled so stale
                // consistency counts don't gate hysteresis if re-enabled.
                snapConsistencyCount_ = 0;
                lastSnapOffset_ = 0;
            }

            // PLL proportional+integral phase correction (v45)
            // Compares inter-beat interval (IBI) against expected period T.
            // If beats consistently arrive early (IBI < T), the integral accumulates
            // and shifts the beat grid forward. Corrects residual drift that onset
            // snap alone cannot fix (e.g., systematic latency bias).
            // Applied AFTER onset snap so it corrects residual error.
            if (pllEnabled && beatCount_ > 2) {
                int T = beatPeriodSamples_;
                if (T < 10) T = 10;
                // IBI-based phase error: how far was the actual inter-beat interval from T?
                // Positive error = beat came late (IBI > T), negative = early (IBI < T)
                int ibi = lastBeatSample_ - prevBeatSample;
                float phaseError = static_cast<float>(ibi - T) / static_cast<float>(T);
                // Clamp to [-0.5, 0.5] to handle edge cases (octave jumps, missed beats)
                if (phaseError > 0.5f) phaseError = 0.5f;
                if (phaseError < -0.5f) phaseError = -0.5f;

                // Proportional correction: shift beat anchor toward correct phase
                float correction = pllKp * phaseError * static_cast<float>(T);

                // Integral: accumulate persistent bias (leaky integrator, tau ~20 beats)
                pllPhaseIntegral_ = clampf(pllSmoother * pllPhaseIntegral_ + phaseError, -10.0f, 10.0f);
                correction += pllKi * pllPhaseIntegral_ * static_cast<float>(T);

                // Apply correction to lastBeatSample_.
                // First N beats: relax to ±T/2 for faster initial phase lock.
                // After warmup: ±T/4 to prevent large jumps during stable tracking.
                int maxShift = (beatCount_ < pllWarmupBeats) ? T / 2 : T / 4;
                int shift = static_cast<int>(correction);
                if (shift > maxShift) shift = maxShift;
                if (shift < -maxShift) shift = -maxShift;
                lastBeatSample_ += shift;
            }

            if (beatCount_ < 65535) beatCount_++;
            beatDetected = true;
            cbssConfidence_ = clampf(cbssConfidence_ + beatConfBoost, 0.0f, 1.0f);
            updateBeatStability(nowMs);

            // Downbeat value from FrameBeatNN's EMA-smoothed output.
            // downbeatSmoothed_ is updated every frame in updateOnsetDensity().
            float downbeatValue = downbeatSmoothed_;

            if (downbeatValue > dbThreshold) {
                control_.downbeat = downbeatValue;
                beatInMeasure_ = 1;  // Reset measure counter on downbeat
            } else {
                control_.downbeat = 0.0f;
                if (beatInMeasure_ > 0) {
                    beatInMeasure_++;
                    if (beatInMeasure_ > 4) beatInMeasure_ = 1;
                }
            }
            control_.beatInMeasure = beatInMeasure_;

            // Capture whether prediction refined this beat's timing (for streaming)
            // Must happen before reset so streaming reads the correct value
            lastFiredBeatPredicted_ = lastBeatWasPredicted_;

            // Phase 2.1: Apply pending tempo at beat boundary
            if (pendingBeatPeriod_ > 0) {
                beatPeriodSamples_ = pendingBeatPeriod_;
                pendingBeatPeriod_ = -1;
            }

            // Shadow CBSS octave checker: periodically test if double-time is better
            if (octaveCheckEnabled) {
                beatsSinceOctaveCheck_++;
                if (beatsSinceOctaveCheck_ >= octaveCheckBeats) {
                    checkOctaveAlternative();
                    beatsSinceOctaveCheck_ = 0;
                }
            }

            // (phase alignment checker removed v44 — net-negative on 18-track validation)
            // (metrical contrast/template match/subbeat alternation calls removed v64 — no benefit)
        }

        // Always reset timers (even if suppressed) to prevent re-firing stale countdown.
        // Account for snapDelay so the effective beat interval stays at T, not T + snapDelay.
        int T = beatPeriodSamples_;
        if (T < 10) T = 10;
        timeToNextBeat_ = T - beatDeclareDelay;
        timeToNextPrediction_ = T / 2;
        lastBeatWasPredicted_ = false;  // Reset; prediction will set true when it runs
    }

    // Decay confidence when no beat
    if (!beatDetected) {
        cbssConfidence_ *= beatConfidenceDecay;
        // Decay downbeat activation between beats so generators see a brief
        // pulse (~150ms) rather than a sustained high value for the entire beat period.
        control_.downbeat *= dbDecay;
        if (control_.downbeat < 0.01f) control_.downbeat = 0.0f;
        // Decay PLL integral during silence to prevent wind-up.
        // Without this, the integral accumulates large values during breakdowns
        // and causes a phase jump when beats resume.
        static constexpr float PLL_INTEGRAL_DECAY_FACTOR = 0.95f;
        pllPhaseIntegral_ *= PLL_INTEGRAL_DECAY_FACTOR;
    }

    // Derive phase (deterministic counter-based)
    // (fwdPhaseOnly hybrid mode removed v64 — phase tracker removed, no benefit)
    int T = beatPeriodSamples_;
    if (T < 10) T = 10;
    {
        float newPhase = static_cast<float>(sampleCounter_ - lastBeatSample_) / static_cast<float>(T);
        newPhase = fmodf(newPhase, 1.0f);
        if (newPhase < 0.0f) newPhase += 1.0f;
        if (!isfinite(newPhase)) newPhase = 0.0f;
        phase_ = newPhase;
    }

    predictNextBeat(nowMs);
}

// ===== PULSE DETECTION =====

uint16_t AudioController::effectivePulseCooldownMs() const {
    // Tempo-adaptive cooldown: beatPeriod/6, clamped to [PULSE_MIN_COOLDOWN_MS, PULSE_MAX_COOLDOWN_MS]
    if (bpm_ < 30.0f) return PULSE_MAX_COOLDOWN_MS;
    float beatPeriodMs = 60000.0f / bpm_;
    uint16_t adaptiveCooldown = static_cast<uint16_t>(beatPeriodMs / 6.0f);
    if (adaptiveCooldown < PULSE_MIN_COOLDOWN_MS) adaptiveCooldown = PULSE_MIN_COOLDOWN_MS;
    if (adaptiveCooldown > PULSE_MAX_COOLDOWN_MS) adaptiveCooldown = PULSE_MAX_COOLDOWN_MS;
    return adaptiveCooldown;
}

// ===== OUTPUT SYNTHESIS =====

void AudioController::synthesizeEnergy() {
    // mic_.getLevel() is the adaptive-normalized audio level (0-1).
    // This is the source of truth for energy — it tracks actual loudness with
    // proper peak/valley tracking. Don't use spectral RMS (compressor inflates it).
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
    // Use ODF-derived pulse strength (replaces ensemble transient detection, v67)
    float pulse = lastPulseStrength_;

    // Apply beat-aligned modulation when rhythm is detected (visual effect only)
    if (pulse > 0.0f && periodicityStrength_ > activationThreshold) {
        float distFromBeat = phase_ < 0.5f ? phase_ : (1.0f - phase_);

        float modulation;
        if (distFromBeat < pulseNearBeatThreshold) {
            // Near beat: boost transient
            modulation = pulseBoostOnBeat;
        } else if (distFromBeat > pulseFarFromBeatThreshold) {
            // Away from beat: suppress transient
            modulation = pulseSuppressOffBeat;
        } else {
            // Transition zone: interpolate between boost and suppress
            float transitionWidth = pulseFarFromBeatThreshold - pulseNearBeatThreshold;
            // FIX: Guard against division by zero if thresholds are equal
            if (transitionWidth < 0.001f) {
                modulation = pulseBoostOnBeat;  // Default to boost
            } else {
                float t = (distFromBeat - pulseNearBeatThreshold) / transitionWidth;
                modulation = pulseBoostOnBeat * (1.0f - t) + pulseSuppressOffBeat * t;
            }
        }

        // Blend modulation based on periodicity strength
        pulse *= (1.0f - periodicityStrength_) + modulation * periodicityStrength_;
    }

    control_.pulse = clampf(pulse, 0.0f, 1.0f);
}

void AudioController::synthesizePhase() {
    // Phase is derived deterministically from CBSS beat counter
    control_.phase = phase_;
}

void AudioController::synthesizeRhythmStrength() {
    // Blend autocorrelation periodicity with CBSS beat tracking confidence
    float strength = periodicityStrength_ * rhythmBlend + cbssConfidence_ * (1.0f - rhythmBlend);

    // Apply activation threshold with soft knee
    if (strength < activationThreshold) {
        strength *= strength / activationThreshold;  // Quadratic falloff below threshold
    }

    // Onset density nudge applied AFTER soft knee to avoid squaring negative values.
    // High density slightly boosts rhythm confidence, low density (ambient) reduces it.
    // Range: ±0.1 modulation, centered at 3 onsets/s
    float densityNudge = clampf((onsetDensity_ - 3.0f) * 0.05f, -0.1f, 0.1f);
    strength += densityNudge;

    control_.rhythmStrength = clampf(strength, 0.0f, 1.0f);

    // Reset measure tracking when rhythm is lost (beatInMeasure 0 = unknown)
    if (control_.rhythmStrength < activationThreshold * 0.5f) {
        beatInMeasure_ = 0;
        control_.beatInMeasure = 0;
        control_.downbeat = 0.0f;
    }
}

void AudioController::updateOnsetDensity(uint32_t nowMs) {
    // Update every 1 second
    uint32_t elapsed = nowMs - onsetDensityWindowStart_;
    if (elapsed >= 1000) {
        float rawDensity = (float)onsetCountInWindow_ * (1000.0f / (float)elapsed);
        // EMA smoothing for onset density (independent from periodicityStrength_)
        onsetDensity_ = onsetDensity_ * onsetDensityBlend + rawDensity * (1.0f - onsetDensityBlend);
        onsetCountInWindow_ = 0;
        onsetDensityWindowStart_ = nowMs;
    }
    control_.onsetDensity = onsetDensity_;

    // Smooth NN downbeat activation with EMA for beat-synchronized sampling.
    // The smoothed value is sampled in detectBeat() when a beat fires, so
    // downbeat only triggers on actual beats (not between beats).
    if (nnActive_ && frameBeatNN_.hasDownbeatOutput()) {
        float rawDownbeat = frameBeatNN_.getLastDownbeat();
        downbeatSmoothed_ = downbeatSmoothed_ * (1.0f - dbEmaAlpha) + rawDownbeat * dbEmaAlpha;
    } else {
        downbeatSmoothed_ *= 0.9f;  // Decay when no downbeat output
    }
    // Note: control_.downbeat is set in detectBeat(), not here.
}

// ============================================================================
// Tempo Prior and Stability Methods
// ============================================================================

// (computeTempoPrior removed — Bayesian fusion applies prior directly in initTempoState)

void AudioController::updateBeatStability(uint32_t nowMs) {
    // Only update when we detect a beat
    // Called from detectBeat() when a real beat is found

    if (lastBeatMs_ == 0) {
        lastBeatMs_ = nowMs;
        return;
    }

    // Calculate inter-beat interval
    float ibiMs = static_cast<float>(nowMs - lastBeatMs_);
    lastBeatMs_ = nowMs;

    // Skip unreasonable intervals (< 200ms = > 300 BPM, > 2000ms = < 30 BPM)
    if (ibiMs < 200.0f || ibiMs > 2000.0f) {
        return;
    }

    // Add to circular buffer
    interBeatIntervals_[ibiWriteIdx_] = ibiMs;
    ibiWriteIdx_ = (ibiWriteIdx_ + 1) % STABILITY_BUFFER_SIZE;
    if (ibiCount_ < STABILITY_BUFFER_SIZE) {
        ibiCount_++;
    }

    // Need at least 4 intervals for meaningful stability
    if (ibiCount_ < 4) {
        beatStability_ = 0.0f;
        return;
    }

    // Compute mean and variance of IBIs
    float sum = 0.0f;
    int count = (ibiCount_ < static_cast<int>(stabilityWindowBeats))
                ? ibiCount_
                : static_cast<int>(stabilityWindowBeats);

    for (int i = 0; i < count; i++) {
        int idx = (ibiWriteIdx_ - 1 - i + STABILITY_BUFFER_SIZE) % STABILITY_BUFFER_SIZE;
        sum += interBeatIntervals_[idx];
    }
    float mean = sum / static_cast<float>(count);

    float variance = 0.0f;
    for (int i = 0; i < count; i++) {
        int idx = (ibiWriteIdx_ - 1 - i + STABILITY_BUFFER_SIZE) % STABILITY_BUFFER_SIZE;
        float diff = interBeatIntervals_[idx] - mean;
        variance += diff * diff;
    }
    variance /= static_cast<float>(count);

    // Coefficient of variation (normalized standard deviation)
    // CV = stddev / mean, typical values 0.01 (very stable) to 0.2 (unstable)
    float stddev = sqrtf(variance);
    float cv = (mean > 0.0f) ? stddev / mean : 1.0f;

    // Convert to stability: 1.0 = perfectly stable, 0.0 = very unstable
    // Map CV of 0.05 -> stability 1.0, CV of 0.2 -> stability 0.0
    beatStability_ = clampf(1.0f - (cv - 0.02f) / 0.15f, 0.0f, 1.0f);
}

void AudioController::updateTempoVelocity(float newBpm, float dt) {
    if (dt <= 0.0f) return;

    // Calculate rate of tempo change (BPM per second)
    float bpmChange = newBpm - prevBpm_;
    float instantVelocity = bpmChange / dt;

    // Smooth the velocity estimate
    tempoVelocity_ = tempoVelocity_ * 0.8f + instantVelocity * 0.2f;

    // Clamp to reasonable range (±50 BPM/sec)
    tempoVelocity_ = clampf(tempoVelocity_, -50.0f, 50.0f);

    prevBpm_ = newBpm;
}

void AudioController::predictNextBeat(uint32_t nowMs) {
    // Predict when next beat will occur based on current phase and tempo
    if (beatPeriodMs_ <= 0.0f || !isfinite(phase_)) {
        nextBeatMs_ = nowMs;
        return;
    }

    // Time until next beat = (1.0 - phase) * beatPeriodMs
    float timeToNextBeat = (1.0f - phase_) * beatPeriodMs_;

    // Apply tempo velocity correction if significant rhythm detected
    if (periodicityStrength_ > activationThreshold && fabsf(tempoVelocity_) > 0.5f) {
        // Adjust prediction based on tempo trend
        // If tempo is increasing (velocity > 0), next beat comes slightly sooner
        float velocityCorrection = -tempoVelocity_ * 0.01f * timeToNextBeat;
        timeToNextBeat += velocityCorrection;
    }

    // Add lookahead offset
    // FIX: Compute signed offset first to prevent unsigned underflow
    float offsetMs = timeToNextBeat - beatLookaheadMs;
    if (offsetMs < 0.0f) {
        nextBeatMs_ = nowMs;  // Beat is imminent or past
    } else {
        nextBeatMs_ = nowMs + static_cast<uint32_t>(offsetMs);
    }
}

// ============================================================================
// Onset Strength Computation Methods
// ============================================================================

// (computeSpectralFluxBands removed v64 — unreachable with unifiedOdf=true default since v54.
//  Legacy independent spectral flux for CBSS, replaced by BandFlux pre-threshold activation.)

// ============================================================================
// Comb Filter Bank Implementation (Independent Tempo Validation)
// ============================================================================

void CombFilterBank::init(float frameRate) {
    frameRate_ = frameRate;

    // Distribute 20 filters evenly from MIN_LAG (~198 BPM) to MAX_LAG (~60 BPM)
    for (int i = 0; i < NUM_FILTERS; i++) {
        float t = static_cast<float>(i) / static_cast<float>(NUM_FILTERS - 1);
        int lag = MIN_LAG + static_cast<int>(t * (MAX_LAG - MIN_LAG) + 0.5f);
        filterLags_[i] = lag;
        filterBPMs_[i] = (frameRate_ * 60.0f) / static_cast<float>(lag);
    }

    reset();
    initialized_ = true;
}

void CombFilterBank::reset() {
    // Clear per-filter output delay lines
    for (int i = 0; i < NUM_FILTERS; i++) {
        for (int j = 0; j < MAX_LAG; j++) {
            resonatorDelay_[i][j] = 0.0f;
        }
    }
    for (int i = 0; i < MAX_LAG; i++) {
        resonatorHistory_[i] = 0.0f;
    }
    writeIdx_ = 0;
    historyIdx_ = 0;

    // Clear resonator state
    for (int i = 0; i < NUM_FILTERS; i++) {
        resonatorOutput_[i] = 0.0f;
        resonatorEnergy_[i] = 0.0f;
    }

    // Reset results
    peakBPM_ = 120.0f;
    peakConfidence_ = 0.0f;
    peakPhase_ = 0.0f;
    peakFilterIdx_ = NUM_FILTERS / 2;  // Start near middle (120 BPM)
    frameCount_ = 0;
}

void CombFilterBank::process(float input) {
    if (!initialized_) {
        init(60.0f);  // Default to 60 Hz
    }

    // 1. Update all resonators using Scheirer (1998) IIR comb filter:
    //    y[n] = (1-α)·x[n] + α·y[n-L]
    //    Each filter reads its OWN delayed output (not the shared input).
    float oneMinusAlpha = 1.0f - feedbackGain;

    for (int i = 0; i < NUM_FILTERS; i++) {
        int lag = filterLags_[i];

        // Read this filter's own delayed output: y[n-L]
        int readIdx = (writeIdx_ - lag + MAX_LAG) % MAX_LAG;
        float delayedOutput = resonatorDelay_[i][readIdx];

        // IIR comb filter equation
        float y = oneMinusAlpha * input + feedbackGain * delayedOutput;
        resonatorOutput_[i] = y;

        // Store output in this filter's delay line
        resonatorDelay_[i][writeIdx_] = y;

        // Smooth energy tracking (exponential moving average)
        float absOut = y > 0.0f ? y : -y;
        resonatorEnergy_[i] = 0.95f * resonatorEnergy_[i] + 0.05f * absOut;
    }

    // 2. Advance shared write index
    writeIdx_ = (writeIdx_ + 1) % MAX_LAG;

    // 4. Find peak energy (NO tempo prior - this provides independent validation)
    //    Autocorrelation already applies tempo prior, so comb bank uses raw energy
    //    to provide truly independent confirmation of tempo
    float maxEnergy = 0.0f;
    int bestIdx = peakFilterIdx_;  // Sticky to previous (hysteresis)

    for (int i = 0; i < NUM_FILTERS; i++) {
        // Use raw resonator energy without tempo prior bias
        float energy = resonatorEnergy_[i];

        // 10% hysteresis to prevent jitter
        if (energy > maxEnergy * 1.1f) {
            maxEnergy = energy;
            bestIdx = i;
        }
    }

    peakFilterIdx_ = bestIdx;
    peakBPM_ = filterBPMs_[bestIdx];

    // 5. Track resonator history at peak filter for phase extraction
    resonatorHistory_[historyIdx_] = resonatorOutput_[bestIdx];
    historyIdx_ = (historyIdx_ + 1) % filterLags_[bestIdx];

    // 6. Compute confidence (peak-to-mean energy ratio)
    float totalEnergy = 0.0f;
    for (int i = 0; i < NUM_FILTERS; i++) {
        totalEnergy += resonatorEnergy_[i];
    }
    float meanEnergy = totalEnergy / NUM_FILTERS;
    float ratio = resonatorEnergy_[bestIdx] / (meanEnergy + 0.001f) - 1.0f;
    peakConfidence_ = ratio > 0.0f ? (ratio > 1.0f ? 1.0f : ratio) : 0.0f;

    // 7. Extract phase every 4 frames to save CPU
    frameCount_++;
    if (frameCount_ >= 4) {
        frameCount_ = 0;
        extractPhase();
    }
}

void CombFilterBank::extractPhase() {
    int lag = filterLags_[peakFilterIdx_];
    float omega = 1.0f / static_cast<float>(lag);  // Normalized frequency

    // Complex exponential correlation to extract phase
    // c = Σ resonator[t] · e^(-j·2π·ω·t)
    // phase = -angle(c) / 2π
    float realSum = 0.0f;
    float imagSum = 0.0f;
    static constexpr float COMB_TWO_PI = 6.283185307f;

    // Use phasor rotation to avoid per-sample cosf/sinf calls
    // Initialize phasor at angle=0 (cos=1, sin=0) and rotate by -2π·ω each step
    float phaseStep = -COMB_TWO_PI * omega;
    float phasorReal = 1.0f;  // cos(0)
    float phasorImag = 0.0f;  // sin(0)
    float rotReal = cosf(phaseStep);
    float rotImag = sinf(phaseStep);

    for (int i = 0; i < lag; i++) {
        int idx = (historyIdx_ - 1 - i + MAX_LAG) % MAX_LAG;
        float sample = resonatorHistory_[idx];

        realSum += sample * phasorReal;
        imagSum += sample * phasorImag;

        // Rotate phasor: (pR + j·pI) * (rR + j·rI)
        float newReal = phasorReal * rotReal - phasorImag * rotImag;
        float newImag = phasorReal * rotImag + phasorImag * rotReal;
        phasorReal = newReal;
        phasorImag = newImag;
    }

    // Compute phase from complex sum
    float phase = -atan2f(imagSum, realSum) / COMB_TWO_PI;

    // Normalize to [0, 1)
    if (phase < 0.0f) phase += 1.0f;
    if (phase >= 1.0f) phase -= 1.0f;

    peakPhase_ = phase;
}

