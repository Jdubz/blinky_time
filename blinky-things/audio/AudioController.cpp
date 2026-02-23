#include "AudioController.h"
#include "../inputs/SerialConsole.h"
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

    // Initialize ensemble detector
    ensemble_.begin();

    // Reset OSS buffer and timestamps
    for (int i = 0; i < OSS_BUFFER_SIZE; i++) {
        ossBuffer_[i] = 0.0f;
        ossTimestamps_[i] = 0;
    }
    ossWriteIdx_ = 0;
    ossCount_ = 0;

    // Reset spectral flux state
    for (int i = 0; i < SPECTRAL_BINS; i++) {
        prevMagnitudes_[i] = 0.0f;
    }
    prevMagnitudesValid_ = false;

    // Reset per-band OSS tracking for adaptive weighting
    for (int band = 0; band < BAND_COUNT; band++) {
        for (int i = 0; i < BAND_OSS_BUFFER_SIZE; i++) {
            bandOssBuffers_[band][i] = 0.0f;
        }
        bandPeriodicityStrength_[band] = 0.0f;
        crossBandCorrelation_[band] = 0.0f;
        bandPeakiness_[band] = 0.0f;
    }
    bandOssWriteIdx_ = 0;
    bandOssCount_ = 0;
    adaptiveBandWeights_[0] = bassBandWeight;   // Use tunable defaults
    adaptiveBandWeights_[1] = midBandWeight;
    adaptiveBandWeights_[2] = highBandWeight;
    lastBandAutocorrMs_ = 0;
    bandSynchrony_ = 0.0f;

    // Reset max-filtered previous magnitudes (SuperFlux vibrato suppression)
    for (int i = 0; i < SPECTRAL_BINS; i++) {
        maxFilteredPrevMags_[i] = 0.0f;
    }

    // Reset tempo estimation
    bpm_ = 120.0f;
    beatPeriodMs_ = 500.0f;
    periodicityStrength_ = 0.0f;

    // Reset phase tracking
    phase_ = 0.0f;
    pulseTrainPhase_ = 0.0f;
    pulseTrainConfidence_ = 0.0f;

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
    lastTempoPriorWeight_ = 1.0f;

    // Reset timing
    lastAutocorrMs_ = 0;
    lastSignificantAudioMs_ = 0;

    // Reset onset density tracking
    onsetDensity_ = 0.0f;
    onsetCountInWindow_ = 0;
    onsetDensityWindowStart_ = time_.millis();

    // Reset IOI onset buffer
    for (int i = 0; i < IOI_ONSET_BUFFER_SIZE; i++) ioiOnsetSamples_[i] = 0;
    ioiOnsetWriteIdx_ = 0;
    ioiOnsetCount_ = 0;
    lastFtMagRatio_ = 0.0f;

    // Initialize and reset comb filter bank
    // Uses 60 Hz frame rate assumption (same as OSS buffer)
    combFilterBank_.init(60.0f);

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
    logGaussianLastT_ = 0;
    logGaussianLastTight_ = 0.0f;
    logGaussianWeightsSize_ = 0;
    beatExpectationLastT_ = 0;
    beatExpectationSize_ = 0;

    // Reset output
    control_ = AudioControl();
    lastEnsembleOutput_ = EnsembleOutput();

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

    // 2. Feed samples to ensemble detector from mic's ring buffer
    //    This provides samples for all spectral detectors (FFT-based)
    static int16_t sampleBuffer[256];  // Matches FFT_SIZE
    int samplesRead = mic_.getSamplesForExternal(sampleBuffer, 256);
    if (samplesRead > 0) {
        ensemble_.addSamples(sampleBuffer, samplesRead);
    }

    // 3. Run ensemble detector with current audio frame data
    //    The ensemble uses level for time-domain detectors (drummer)
    //    and spectral data when available
    lastEnsembleOutput_ = ensemble_.update(
        mic_.getLevel(),
        mic_.getRawLevel(),
        nowMs,
        dt
    );

    // 3b. Count onsets for density tracking
    if (lastEnsembleOutput_.transientStrength > 0.0f) {
        onsetCountInWindow_++;
    }

    // 3c. Phase correction: when a transient occurs near a predicted beat
    //     boundary, nudge lastBeatSample_ to align phase with the transient.
    //     This corrects cumulative drift from small BPM errors.
    if (lastEnsembleOutput_.transientStrength > 0.0f) {
        lastTransientSample_ = sampleCounter_;

        // Record onset in IOI ring buffer for inter-onset interval analysis
        if (ioiEnabled) {
            ioiOnsetSamples_[ioiOnsetWriteIdx_] = sampleCounter_;
            ioiOnsetWriteIdx_ = (ioiOnsetWriteIdx_ + 1) % IOI_ONSET_BUFFER_SIZE;
            if (ioiOnsetCount_ < IOI_ONSET_BUFFER_SIZE) ioiOnsetCount_++;
        }

        if (phaseCorrectionStrength > 0.0f && beatCount_ > 2 && beatPeriodSamples_ >= 10) {
            int T = beatPeriodSamples_;
            int elapsed = sampleCounter_ - lastBeatSample_;
            int phaseError = elapsed % T;
            if (phaseError > T / 2) phaseError -= T;  // Center: -T/2 to +T/2

            int window = T / 4;  // Correction window: ±25% of beat period
            if (phaseError != 0 && phaseError > -window && phaseError < window) {
                int correction = static_cast<int>(phaseError * phaseCorrectionStrength);
                if (correction != 0) {
                    lastBeatSample_ += correction;
                }
            }
        }
    }

    // 4. Get onset strength for rhythm analysis
    //    This is INDEPENDENT of transient detection - analyzes energy patterns
    //    Two methods available:
    //    - Spectral flux: captures energy CHANGES (better for beat timing)
    //    - Multi-band RMS: captures absolute levels (legacy behavior)
    //    Controlled by ossFluxWeight parameter (1.0 = pure flux, 0.0 = pure RMS)
    float onsetStrength = 0.0f;

    // Get spectral data from ensemble
    const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();

    if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
        const float* magnitudes = spectral.getMagnitudes();
        int numBins = spectral.getNumBins();

        // Compute spectral flux with per-band outputs for adaptive weighting
        float bassFlux = 0.0f, midFlux = 0.0f, highFlux = 0.0f;
        float fluxOss = computeSpectralFluxBands(magnitudes, numBins, bassFlux, midFlux, highFlux);
        float rmsOss = computeMultiBandRms(magnitudes, numBins);

        // Blend based on ossFluxWeight
        // ossFluxWeight = 1.0: pure spectral flux (recommended)
        // ossFluxWeight = 0.0: pure RMS (legacy behavior)
        onsetStrength = ossFluxWeight * fluxOss + (1.0f - ossFluxWeight) * rmsOss;

        // Store per-band samples for adaptive weight calculation
        if (adaptiveBandWeightEnabled) {
            addBandOssSamples(bassFlux, midFlux, highFlux);
        }
    } else {
        // Fallback when no spectral data: use normalized level
        onsetStrength = mic_.getLevel();
        // Reset prev magnitudes since we have no spectral data this frame
        prevMagnitudesValid_ = false;
    }

    // Apply ODF smoothing before all consumers (OSS buffer, comb bank, CBSS)
    onsetStrength = smoothOnsetStrength(onsetStrength);
    lastSmoothedOnset_ = onsetStrength;

    // Update per-band periodicities periodically (same rate as main autocorr)
    if (adaptiveBandWeightEnabled && nowMs - lastBandAutocorrMs_ >= autocorrPeriodMs) {
        updateBandPeriodicities(nowMs);
        lastBandAutocorrMs_ = nowMs;
    }

    // Track when we last had significant audio
    // Use a threshold above typical noise floor (~0.02) to avoid tracking silence
    const float SIGNIFICANT_AUDIO_THRESHOLD = 0.05f;
    bool hasSignificantAudio = (onsetStrength > SIGNIFICANT_AUDIO_THRESHOLD ||
                                 mic_.getLevel() > SIGNIFICANT_AUDIO_THRESHOLD);

    // 5. Add sample to onset strength buffer with timestamp
    // Only add significant audio to avoid filling buffer with noise patterns
    if (hasSignificantAudio) {
        lastSignificantAudioMs_ = nowMs;
        addOssSample(onsetStrength, nowMs);
    } else {
        // Add zero during silence to maintain buffer timing but not contribute to correlation
        addOssSample(0.0f, nowMs);
    }

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

    // 7. Update CBSS and detect beats
    //    Uses spectral flux (continuous onset strength signal) so CBSS tracks
    //    the audio energy pattern regardless of whether transients are detected.
    updateCBSS(onsetStrength);
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

void AudioController::setDetectorEnabled(DetectorType type, bool enabled) {
    ensemble_.setDetectorEnabled(type, enabled);
}

void AudioController::setDetectorWeight(DetectorType type, float weight) {
    ensemble_.setDetectorWeight(type, weight);
}

void AudioController::setDetectorThreshold(DetectorType type, float threshold) {
    ensemble_.setDetectorThreshold(type, threshold);
}

void AudioController::setBpmRange(float minBpm, float maxBpm) {
    bpmMin = clampf(minBpm, 30.0f, 120.0f);
    bpmMax = clampf(maxBpm, 80.0f, 300.0f);

    if (bpmMin >= bpmMax) {
        bpmMin = 60.0f;
        bpmMax = 200.0f;
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
    // Fallback should never be needed (ossCount_ >= 60 with valid timestamps ensures bufferDurationMs > 0)
    // but use defensive 60 Hz assumption if it somehow occurs
    float samplesPerMs = bufferDurationMs > 0 ? (float)ossCount_ / (float)bufferDurationMs : 0.06f;

    int minLag = static_cast<int>(minLagMs * samplesPerMs);
    int maxLag = static_cast<int>(maxLagMs * samplesPerMs);

    if (minLag < 10) minLag = 10;
    if (maxLag > ossCount_ / 2) maxLag = ossCount_ / 2;
    if (minLag >= maxLag) return;

    // Compute signal energy for normalization
    float signalEnergy = 0.0f;
    float maxOss = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        signalEnergy += ossBuffer_[idx] * ossBuffer_[idx];
        if (ossBuffer_[idx] > maxOss) maxOss = ossBuffer_[idx];
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
    static float correlationAtLag[200] = {0};
    int correlationSize = maxLag - minLag + 1;
    if (correlationSize > 200) correlationSize = 200;

    // FIX: Clear the portion we'll use to prevent stale data
    for (int i = 0; i < correlationSize; i++) {
        correlationAtLag[i] = 0.0f;
    }

    float maxCorrelation = 0.0f;
    int bestLag = minLag;  // FIX: Initialize to valid lag, not 0

    // ODF mean subtraction (BTrack-style detrending)
    // Removes DC bias from autocorrelation — without this, all lags appear
    // somewhat correlated due to the non-zero mean of the OSS buffer.
    float ossMean = 0.0f;
    if (odfMeanSubEnabled) {
        for (int i = 0; i < ossCount_; i++) {
            int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            ossMean += ossBuffer_[idx];
        }
        ossMean /= static_cast<float>(ossCount_);
        // Adjust signalEnergy to variance: sum((x-mean)^2) = sum(x^2) - N*mean^2
        // This keeps normalization consistent with the mean-subtracted autocorrelation.
        signalEnergy -= static_cast<float>(ossCount_) * ossMean * ossMean;
        if (signalEnergy < 0.001f) signalEnergy = 0.001f;  // Guard against floating-point undershoot
    }

    for (int lag = minLag; lag <= maxLag && (lag - minLag) < 200; lag++) {
        float correlation = 0.0f;
        int count = ossCount_ - lag;

        // FIX: Skip if count <= 0 to prevent division by zero
        if (count <= 0) continue;

        for (int i = 0; i < count; i++) {
            int idx1 = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            int idx2 = (ossWriteIdx_ - 1 - i - lag + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            correlation += (ossBuffer_[idx1] - ossMean) * (ossBuffer_[idx2] - ossMean);
        }

        correlation /= static_cast<float>(count);
        correlationAtLag[lag - minLag] = correlation;

        if (correlation > maxCorrelation) {
            maxCorrelation = correlation;
            bestLag = lag;
        }
    }

    // Compute periodicity strength (normalized correlation)
    float avgEnergy = signalEnergy / static_cast<float>(ossCount_);
    float normCorrelation = maxCorrelation / (avgEnergy + 0.001f);

    // Smooth periodicity strength updates
    float newStrength = clampf(normCorrelation * 1.5f, 0.0f, 1.0f);
    periodicityStrength_ = periodicityStrength_ * 0.7f + newStrength * 0.3f;

    // === HARMONIC PRODUCT SPECTRUM (additive enhancement) ===
    // Percival-style: add sub-harmonic energy to each lag BEFORE peak extraction.
    // Peaks at the true beat period have strong autocorrelation at 2x their lag
    // (every 2 beats), so additive enhancement boosts them. False harmonic peaks
    // lack this support and are NOT boosted (but never penalized).
    // Process forward (i=0..half): safe because we read from i*2 > i.
    if (hpsEnabled) {
        int halfSize = correlationSize / 2;
        for (int i = 0; i < halfSize; i++) {
            correlationAtLag[i] += correlationAtLag[i * 2];
        }
    }

    // === PEAK EXTRACTION (find best tempo peak) ===
    // Apply tempo prior to find the strongest peak (HPS already applied above)
    // Also collect top N candidates for pulse train evaluation
    float bestWeightedCorr = 0.0f;
    int bestWeightedLag = bestLag;

    // Candidate collection for pulse train evaluation
    static constexpr int MAX_PT_SLOTS = 10;
    static constexpr int MIN_PT_LAG_SEP = 3;  // Prevent clustering of nearby lags
    struct { int lag; float score; } ptCandidates[MAX_PT_SLOTS];
    int numPtCandidates = 0;
    int maxPtCandidates = (pulseTrainEnabled && pulseTrainCandidates >= 2) ?
        (pulseTrainCandidates < MAX_PT_SLOTS ? pulseTrainCandidates : MAX_PT_SLOTS) : 0;

    for (int lag = minLag; lag <= maxLag && (lag - minLag) < correlationSize; lag++) {
        int lagIdx = lag - minLag;
        float correlation = correlationAtLag[lagIdx];
        float normCorr = correlation / (avgEnergy + 0.001f);

        // Skip weak peaks (tunable threshold)
        if (normCorr < peakMinCorrelation) continue;

        // Apply tempo prior
        float lagBpm = 60000.0f / (static_cast<float>(lag) / samplesPerMs);
        lagBpm = clampf(lagBpm, bpmMin, bpmMax);
        float priorWeight = computeTempoPrior(lagBpm);

        float weightedCorr = normCorr * priorWeight;

        // Track overall best
        if (weightedCorr > bestWeightedCorr) {
            bestWeightedCorr = weightedCorr;
            bestWeightedLag = lag;
            lastTempoPriorWeight_ = priorWeight;
        }

        // Collect diverse candidates for pulse train evaluation
        if (maxPtCandidates > 0) {
            // Check if too close to an existing candidate
            bool merged = false;
            for (int c = 0; c < numPtCandidates; c++) {
                int dist = lag - ptCandidates[c].lag;
                if (dist < 0) dist = -dist;
                if (dist <= MIN_PT_LAG_SEP) {
                    if (weightedCorr > ptCandidates[c].score) {
                        ptCandidates[c].lag = lag;
                        ptCandidates[c].score = weightedCorr;
                    }
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (numPtCandidates < maxPtCandidates) {
                    ptCandidates[numPtCandidates].lag = lag;
                    ptCandidates[numPtCandidates].score = weightedCorr;
                    numPtCandidates++;
                } else {
                    // Replace weakest candidate if current is stronger
                    int weakest = 0;
                    for (int c = 1; c < numPtCandidates; c++) {
                        if (ptCandidates[c].score < ptCandidates[weakest].score)
                            weakest = c;
                    }
                    if (weightedCorr > ptCandidates[weakest].score) {
                        ptCandidates[weakest].lag = lag;
                        ptCandidates[weakest].score = weightedCorr;
                    }
                }
            }
        }
    }

    // === PULSE TRAIN EVALUATION (Percival & Tzanetakis 2014) ===
    // When enabled with enough candidates, re-ranks by actual onset alignment.
    // Replaces harmonic disambiguation because the pulse train template already
    // incorporates 2x and 3/2x harmonic support in its scoring.
    // Guard: require enough data to evaluate the autocorrelation winner (4 beat cycles).
    // Without this, startup with limited data skips slow candidates and falsely picks fast tempos.
    bool pulseTrainActive = pulseTrainEnabled && numPtCandidates > 1 && ossCount_ >= bestWeightedLag * 4;
    if (pulseTrainActive) {
        int ptLags[MAX_PT_SLOTS] = {0};
        float ptScores[MAX_PT_SLOTS] = {0};
        for (int c = 0; c < numPtCandidates; c++) {
            ptLags[c] = ptCandidates[c].lag;
            ptScores[c] = ptCandidates[c].score;
        }

        int ptWinner = evaluatePulseTrains(ptLags, ptScores, numPtCandidates, samplesPerMs, shouldPrintDebug);
        if (ptWinner > 0) {
            bestWeightedLag = ptWinner;
            // Update bestWeightedCorr for the new winner so downstream
            // checks (comb bank cross-validation) use the correct threshold
            int winIdx = ptWinner - minLag;
            if (winIdx >= 0 && winIdx < correlationSize) {
                float winCorr = correlationAtLag[winIdx] / (avgEnergy + 0.001f);
                float winBpm = 60000.0f / (static_cast<float>(ptWinner) / samplesPerMs);
                bestWeightedCorr = winCorr * computeTempoPrior(clampf(winBpm, bpmMin, bpmMax));
            }
        }
    } else if (tempoPriorEnabled) {
        // === HARMONIC DISAMBIGUATION (fallback when pulse train disabled) ===
        // Check multiple harmonic ratios to avoid sub-harmonic locking.
        float currentBpm = 60000.0f / (static_cast<float>(bestWeightedLag) / samplesPerMs);

        // Check half-lag (2x BPM) — prefer faster tempo if strong
        int halfLag = bestWeightedLag / 2;
        if (halfLag >= minLag) {
            int halfIdx = halfLag - minLag;
            if (halfIdx < correlationSize) {
                float halfCorr = correlationAtLag[halfIdx] / (avgEnergy + 0.001f);
                float halfBpm = 60000.0f / (static_cast<float>(halfLag) / samplesPerMs);
                float priorCurrent = computeTempoPrior(currentBpm);
                float priorHalf = computeTempoPrior(halfBpm);

                if (halfCorr > bestWeightedCorr * harmonicUp2xThresh && priorHalf >= priorCurrent * 0.85f) {
                    if (shouldPrintDebug) {
                        Serial.print(F("{\"type\":\"HARMONIC_FIX\",\"dir\":\"up\",\"from\":"));
                        Serial.print(currentBpm, 1);
                        Serial.print(F(",\"to\":"));
                        Serial.print(halfBpm, 1);
                        Serial.println(F("}"));
                    }
                    bestWeightedLag = halfLag;
                    currentBpm = halfBpm;
                }
            }
        }

        // Check 2/3-lag (3/2x BPM) — common metrical relationship
        int twoThirdLag = bestWeightedLag * 2 / 3;
        if (twoThirdLag >= minLag) {
            int ttIdx = twoThirdLag - minLag;
            if (ttIdx < correlationSize) {
                float ttCorr = correlationAtLag[ttIdx] / (avgEnergy + 0.001f);
                float ttBpm = 60000.0f / (static_cast<float>(twoThirdLag) / samplesPerMs);
                float priorCurrent = computeTempoPrior(currentBpm);
                float priorTT = computeTempoPrior(ttBpm);

                if (ttCorr > bestWeightedCorr * harmonicUp32Thresh && priorTT >= priorCurrent * 0.90f) {
                    if (shouldPrintDebug) {
                        Serial.print(F("{\"type\":\"HARMONIC_FIX\",\"dir\":\"up3/2\",\"from\":"));
                        Serial.print(currentBpm, 1);
                        Serial.print(F(",\"to\":"));
                        Serial.print(ttBpm, 1);
                        Serial.println(F("}"));
                    }
                    bestWeightedLag = twoThirdLag;
                    currentBpm = ttBpm;
                }
            }
        }

        // Check double-lag (half BPM) — original disambiguation (prefer fundamental)
        int doubleLag = bestWeightedLag * 2;
        if (doubleLag <= maxLag) {
            int dblIdx = doubleLag - minLag;
            if (dblIdx >= 0 && dblIdx < correlationSize) {
                float dblCorr = correlationAtLag[dblIdx] / (avgEnergy + 0.001f);
                float dblBpm = 60000.0f / (static_cast<float>(doubleLag) / samplesPerMs);
                float priorCurrent = computeTempoPrior(currentBpm);
                float priorSlow = computeTempoPrior(dblBpm);

                if (dblCorr > bestWeightedCorr * 0.6f && priorSlow >= priorCurrent * 0.95f) {
                    if (shouldPrintDebug) {
                        Serial.print(F("{\"type\":\"HARMONIC_FIX\",\"dir\":\"down\",\"from\":"));
                        Serial.print(currentBpm, 1);
                        Serial.print(F(",\"to\":"));
                        Serial.print(dblBpm, 1);
                        Serial.println(F("}"));
                    }
                    bestWeightedLag = doubleLag;
                }
            }
        }
    }

    // === COMB BANK CROSS-VALIDATION ===
    // If the comb bank (independent, no prior) disagrees with autocorrelation,
    // check if autocorrelation has evidence at the comb bank's suggested tempo.
    // This catches non-harmonic locking where autocorr picks a spurious peak.
    if (combBankEnabled && combFilterBank_.getPeakConfidence() > combCrossValMinConf) {
        float combBpm = combFilterBank_.getPeakBPM();
        float autoBpm = 60000.0f / (static_cast<float>(bestWeightedLag) / samplesPerMs);

        // Only intervene when they disagree by >10%
        float disagreement = fabsf(combBpm - autoBpm) / autoBpm;
        if (disagreement > 0.1f) {
            // Convert comb BPM to autocorrelation lag
            int combLag = static_cast<int>(60000.0f / combBpm * samplesPerMs + 0.5f);

            // Check if autocorrelation has evidence at this lag
            if (combLag >= minLag && combLag <= maxLag) {
                int combIdx = combLag - minLag;
                if (combIdx < correlationSize) {
                    float combCorr = correlationAtLag[combIdx] / (avgEnergy + 0.001f);

                    // Accept if autocorr at comb lag exceeds threshold of best
                    if (combCorr > bestWeightedCorr * combCrossValMinCorr) {
                        if (shouldPrintDebug) {
                            Serial.print(F("{\"type\":\"COMB_XVAL\",\"from\":"));
                            Serial.print(autoBpm, 1);
                            Serial.print(F(",\"to\":"));
                            Serial.print(combBpm, 1);
                            Serial.print(F(",\"combConf\":"));
                            Serial.print(combFilterBank_.getPeakConfidence(), 2);
                            Serial.print(F(",\"combCorr\":"));
                            Serial.print(combCorr, 3);
                            Serial.println(F("}"));
                        }
                        bestWeightedLag = combLag;
                    }
                }
            }
        }
    }

    // === IOI HISTOGRAM CROSS-VALIDATION ===
    // Measures actual time intervals between detected onset events (kicks/snares).
    // If IOI histogram has a clear peak at a FASTER tempo AND autocorrelation
    // has some evidence at that lag, switch to it.
    // UPWARD ONLY: Sub-harmonic locking (BPM too low) is the primary failure mode.
    // Allowing downward correction caused regressions (machine-drum 118→96 BPM)
    // because noisy transient intervals cluster at slower tempos.
    float ioiPeakBpm = 0.0f;  // For debug output
    if (ioiEnabled && ioiOnsetCount_ >= 8) {
        int ioiLag = computeIOIPeakLag(minLag, maxLag);
        if (ioiLag > 0) {
            ioiPeakBpm = 60000.0f / (static_cast<float>(ioiLag) / samplesPerMs);
            float autoBpm = 60000.0f / (static_cast<float>(bestWeightedLag) / samplesPerMs);

            // Only intervene when IOI suggests FASTER tempo (shorter lag) by >10%
            // Blocks downward corrections which amplify sub-harmonic locking
            if (ioiPeakBpm > autoBpm * 1.1f) {
                int ioiIdx = ioiLag - minLag;
                if (ioiIdx >= 0 && ioiIdx < correlationSize) {
                    float ioiCorr = correlationAtLag[ioiIdx] / (avgEnergy + 0.001f);

                    // Accept if autocorr at IOI lag exceeds threshold of best
                    if (ioiCorr > bestWeightedCorr * ioiMinAutocorr) {
                        if (shouldPrintDebug) {
                            Serial.print(F("{\"type\":\"IOI_XVAL\",\"from\":"));
                            Serial.print(autoBpm, 1);
                            Serial.print(F(",\"to\":"));
                            Serial.print(ioiPeakBpm, 1);
                            Serial.print(F(",\"ioiCorr\":"));
                            Serial.print(ioiCorr, 3);
                            Serial.print(F(",\"onsets\":"));
                            Serial.print(ioiOnsetCount_);
                            Serial.println(F("}"));
                        }
                        bestWeightedLag = ioiLag;
                    } else if (shouldPrintDebug) {
                        // Near-miss: IOI had a candidate but autocorr evidence too weak
                        Serial.print(F("{\"type\":\"IOI_MISS\",\"bpm\":"));
                        Serial.print(ioiPeakBpm, 1);
                        Serial.print(F(",\"corr\":"));
                        Serial.print(ioiCorr, 3);
                        Serial.print(F(",\"need\":"));
                        Serial.print(bestWeightedCorr * ioiMinAutocorr, 3);
                        Serial.println(F("}"));
                    }
                }
            }
        }
    }

    // === FOURIER TEMPOGRAM CROSS-VALIDATION ===
    // DFT of OSS buffer suppresses sub-harmonics (unlike autocorrelation which creates them).
    // A 143 BPM signal shows peaks at 143, 286 BPM but NEVER at 72 BPM.
    // Uses Goertzel algorithm for efficient single-frequency magnitude computation.
    float ftPeakBpm = 0.0f;  // For debug output
    if (ftEnabled && ossCount_ >= 60) {
        int ftLag = computeFourierTempogramPeakLag(minLag, maxLag);
        if (ftLag > 0) {
            ftPeakBpm = 60000.0f / (static_cast<float>(ftLag) / samplesPerMs);
            float autoBpm = 60000.0f / (static_cast<float>(bestWeightedLag) / samplesPerMs);

            // Only intervene when FT disagrees with autocorrelation by >10%
            if (fabsf(ftPeakBpm - autoBpm) / autoBpm > 0.1f) {
                int ftIdx = ftLag - minLag;
                if (ftIdx >= 0 && ftIdx < correlationSize) {
                    float ftCorr = correlationAtLag[ftIdx] / (avgEnergy + 0.001f);

                    // Accept if autocorrelation has some evidence at FT lag
                    if (ftCorr > bestWeightedCorr * ftMinAutocorr) {
                        if (shouldPrintDebug) {
                            Serial.print(F("{\"type\":\"FT_XVAL\",\"from\":"));
                            Serial.print(autoBpm, 1);
                            Serial.print(F(",\"to\":"));
                            Serial.print(ftPeakBpm, 1);
                            Serial.print(F(",\"ftCorr\":"));
                            Serial.print(ftCorr, 3);
                            Serial.println(F("}"));
                        }
                        bestWeightedLag = ftLag;
                    } else if (shouldPrintDebug) {
                        // Near-miss: FT had a candidate but autocorr evidence too weak
                        Serial.print(F("{\"type\":\"FT_MISS\",\"bpm\":"));
                        Serial.print(ftPeakBpm, 1);
                        Serial.print(F(",\"corr\":"));
                        Serial.print(ftCorr, 3);
                        Serial.print(F(",\"need\":"));
                        Serial.print(bestWeightedCorr * ftMinAutocorr, 3);
                        Serial.println(F("}"));
                    }
                }
            }
        }
    }

    // DEBUG: Print correlation results (only when debug enabled)
    if (shouldPrintDebug) {
        float detectedBpm = (bestWeightedLag > 0) ? 60000.0f / (static_cast<float>(bestWeightedLag) / samplesPerMs) : 0.0f;
        Serial.print(F("{\"type\":\"RHYTHM_DEBUG2\",\"bestLag\":"));
        Serial.print(bestWeightedLag);
        Serial.print(F(",\"maxCorr\":"));
        Serial.print(maxCorrelation, 6);
        Serial.print(F(",\"normCorr\":"));
        Serial.print(normCorrelation, 4);
        Serial.print(F(",\"newStr\":"));
        Serial.print(newStrength, 3);
        Serial.print(F(",\"bpm\":"));
        Serial.print(detectedBpm, 1);
        Serial.print(F(",\"hps\":"));
        Serial.print(hpsEnabled ? 1 : 0);
        Serial.print(F(",\"pt\":"));
        Serial.print(pulseTrainActive ? 1 : 0);
        Serial.print(F(",\"ioi\":"));
        Serial.print(ioiPeakBpm, 1);
        Serial.print(F(",\"ic\":"));
        Serial.print(ioiOnsetCount_);
        Serial.print(F(",\"ft\":"));
        Serial.print(ftPeakBpm, 1);
        Serial.print(F(",\"ftr\":"));
        Serial.print(lastFtMagRatio_, 2);
        Serial.print(F(",\"ms\":"));
        Serial.print(odfMeanSubEnabled ? 1 : 0);
        Serial.println(F("}"));
    }

    // === UPDATE TEMPO ===
    if (bestWeightedLag > 0 && periodicityStrength_ > 0.25f) {
        float newBpm = 60000.0f * samplesPerMs / static_cast<float>(bestWeightedLag);
        newBpm = clampf(newBpm, bpmMin, bpmMax);

        // Tempo rate limiting during active tracking
        if (periodicityStrength_ > activationThreshold && bpm_ > 1.0f) {
            float maxChange = bpm_ * (maxBpmChangePerSec / 100.0f) * (autocorrPeriodMs / 1000.0f);
            if (maxChange < 1.0f) maxChange = 1.0f;
            float bpmDiff = newBpm - bpm_;
            if (bpmDiff > maxChange) newBpm = bpm_ + maxChange;
            else if (bpmDiff < -maxChange) newBpm = bpm_ - maxChange;
        }

        // Check if tempo change is large enough to snap vs smooth
        float changeRatio = (bpm_ > 1.0f) ? fabsf(newBpm - bpm_) / bpm_ : 1.0f;

        if (changeRatio > tempoSnapThreshold) {
            // Large change: snap to new tempo
            bpm_ = newBpm;
        } else {
            // Small change: smooth (tunable blend for convergence speed)
            bpm_ = bpm_ * tempoSmoothFactor + newBpm * (1.0f - tempoSmoothFactor);
        }

        beatPeriodMs_ = 60000.0f / bpm_;

        // Update beat period in samples for CBSS
        // Uses adaptive samplesPerMs for accurate conversion
        int newPeriodSamples = static_cast<int>(beatPeriodMs_ * samplesPerMs + 0.5f);
        if (newPeriodSamples < 10) newPeriodSamples = 10;
        if (newPeriodSamples > OSS_BUFFER_SIZE / 2) newPeriodSamples = OSS_BUFFER_SIZE / 2;
        beatPeriodSamples_ = newPeriodSamples;

        // Update ensemble detector with tempo hint for adaptive cooldown
        ensemble_.getFusion().setTempoHint(bpm_);

        // Update tempo velocity if BPM changed significantly
        if (fabsf(bpm_ - prevBpm_) / (prevBpm_ > 1.0f ? prevBpm_ : 1.0f) > tempoChangeThreshold) {
            float dt = autocorrPeriodMs / 1000.0f;
            updateTempoVelocity(bpm_, dt);
        }

        // Compute Fourier phase for debug (not used for tracking — CBSS handles phase)
        pulseTrainPhase_ = computePulseTrainPhase(bestWeightedLag);
    }
}

// ===== ODF SMOOTHING =====

float AudioController::smoothOnsetStrength(float raw) {
    int width = odfSmoothWidth;
    if (width < 3) width = 3;
    if (width > ODF_SMOOTH_MAX) width = ODF_SMOOTH_MAX;
    odfSmoothBuffer_[odfSmoothIdx_] = raw;
    odfSmoothIdx_ = (odfSmoothIdx_ + 1) % width;
    float sum = 0.0f;
    for (int i = 0; i < width; i++) sum += odfSmoothBuffer_[i];
    return sum / width;
}

// ===== FOURIER TEMPOGRAM CROSS-VALIDATION =====

int AudioController::computeFourierTempogramPeakLag(int minLag, int maxLag) {
    if (ossCount_ < 60) return 0;  // Need at least 1s of data

    int bestLag = 0;
    float bestMagSq = 0.0f;
    float sumMagSq = 0.0f;
    int numEvals = 0;

    // Compute OSS mean for detrending (removes DC from DFT)
    float mean = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - ossCount_ + i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        mean += ossBuffer_[idx];
    }
    mean /= static_cast<float>(ossCount_);

    // Evaluate Goertzel at each candidate lag
    // For lag L: frequency = 1/L cycles per sample, coeff = 2*cos(2*pi/L)
    for (int lag = minLag; lag <= maxLag; lag++) {
        float omega = 2.0f * 3.14159265f / static_cast<float>(lag);
        float coeff = 2.0f * cosf(omega);

        float s1 = 0.0f, s2 = 0.0f;

        // Process OSS buffer in chronological order (mean-subtracted)
        for (int i = 0; i < ossCount_; i++) {
            int idx = (ossWriteIdx_ - ossCount_ + i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            float s0 = (ossBuffer_[idx] - mean) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        // Goertzel magnitude squared: |X(k)|^2 = s1^2 + s2^2 - coeff*s1*s2
        float magSq = s1 * s1 + s2 * s2 - coeff * s1 * s2;

        sumMagSq += magSq;
        numEvals++;

        if (magSq > bestMagSq) {
            bestMagSq = magSq;
            bestLag = lag;
        }
    }

    // Require peak to be significantly above mean
    if (numEvals < 5) { lastFtMagRatio_ = 0.0f; return 0; }
    float meanMagSq = sumMagSq / static_cast<float>(numEvals);
    if (meanMagSq < 1e-10f) { lastFtMagRatio_ = 0.0f; return 0; }

    // Store magnitude ratio for debug: sqrt(bestMagSq/meanMagSq)
    lastFtMagRatio_ = sqrtf(bestMagSq / meanMagSq);

    // Compare magnitude (not squared): peak_mag > ratio * mean_mag
    // Equivalent: peak_magSq > ratio^2 * mean_magSq
    float ratioSq = ftMinMagnitudeRatio * ftMinMagnitudeRatio;
    if (bestMagSq < ratioSq * meanMagSq) return 0;

    return bestLag;
}

// ===== IOI HISTOGRAM CROSS-VALIDATION =====

int AudioController::computeIOIPeakLag(int minLag, int maxLag) {
    if (ioiOnsetCount_ < 8) return 0;

    // Stack histogram matching autocorrelation range
    int histSize = maxLag - minLag + 1;
    if (histSize > 200) histSize = 200;
    int histogram[200] = {0};

    // Iterate over all onset pairs to compute inter-onset intervals
    // Onsets are stored in a ring buffer; read them newest-first
    int n = ioiOnsetCount_;
    for (int i = 0; i < n; i++) {
        int idxI = (ioiOnsetWriteIdx_ - 1 - i + IOI_ONSET_BUFFER_SIZE) % IOI_ONSET_BUFFER_SIZE;
        int sampleI = ioiOnsetSamples_[idxI];

        for (int j = i + 1; j < n; j++) {
            int idxJ = (ioiOnsetWriteIdx_ - 1 - j + IOI_ONSET_BUFFER_SIZE) % IOI_ONSET_BUFFER_SIZE;
            int sampleJ = ioiOnsetSamples_[idxJ];

            int interval = sampleI - sampleJ;  // Always positive (i is newer)
            if (interval <= 0) continue;  // Safety check

            // Early exit: if interval exceeds 2x max range, no more useful pairs
            if (interval > 2 * maxLag) break;

            // Direct match: interval falls in [minLag, maxLag]
            if (interval >= minLag && interval <= maxLag) {
                int bin = interval - minLag;
                if (bin < histSize) histogram[bin]++;
            }

            // Folded match: interval is ~2x a beat period (skipped beat)
            // Map to half-interval
            if (interval >= 2 * minLag) {
                int halfInterval = interval / 2;
                if (halfInterval >= minLag && halfInterval <= maxLag) {
                    int bin = halfInterval - minLag;
                    if (bin < histSize) histogram[bin]++;
                }
            }
        }
    }

    // In-place 3-bin smoothing to cluster jittered intervals (±1 sample tolerance)
    // Avoids splitting a clear peak across adjacent bins.
    // Uses lookback variable (`prev`) to avoid a second 800-byte stack array.
    // Each bin reads raw `next` (not yet overwritten) and saved raw `prev` (from before overwrite).
    int prev = 0;
    for (int i = 0; i < histSize; i++) {
        int curr = histogram[i];
        int next = (i < histSize - 1) ? histogram[i + 1] : 0;
        histogram[i] = prev + curr + next;
        prev = curr;
    }

    // Find peak and compute mean on smoothed histogram
    int peakBin = 0;
    int peakCount = 0;
    int totalCount = 0;
    for (int i = 0; i < histSize; i++) {
        totalCount += histogram[i];
        if (histogram[i] > peakCount) {
            peakCount = histogram[i];
            peakBin = i;
        }
    }

    if (totalCount == 0 || peakCount == 0) return 0;

    float mean = static_cast<float>(totalCount) / static_cast<float>(histSize);

    // Peak must be significantly above mean
    if (mean < 0.001f || static_cast<float>(peakCount) < ioiMinPeakRatio * mean) return 0;

    return peakBin + minLag;
}

// ===== LOG-GAUSSIAN WEIGHT COMPUTATION =====

void AudioController::recomputeLogGaussianWeights(int T) {
    if (T == logGaussianLastT_ && cbssTightness == logGaussianLastTight_) return;
    logGaussianLastT_ = T;
    logGaussianLastTight_ = cbssTightness;
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
        float a = cbssTightness * logRatio;
        logGaussianWeights_[i] = expf(-0.5f * a * a);
    }
}

// ===== CBSS BEAT TRACKING =====

void AudioController::updateCBSS(float onsetStrength) {
    // CBSS[n] = (1-alpha)*OSS[n] + alpha*max_weighted(CBSS[n-2T : n-T/2])
    // Uses log-Gaussian transition weighting (BTrack-style) instead of flat max
    int T = beatPeriodSamples_;
    if (T < 10) T = 10;

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

    float cbssVal = (1.0f - cbssAlpha) * onsetStrength + cbssAlpha * maxWeightedCBSS;
    cbssBuffer_[sampleCounter_ % OSS_BUFFER_SIZE] = cbssVal;

    sampleCounter_++;

    // Prevent signed integer overflow (UB in C++).
    // At 60 Hz, sampleCounter_ reaches 1M after ~4.6 hours.
    // Renormalize both counters to keep values small while preserving
    // their difference (which is all the CBSS logic depends on).
    // The CBSS circular buffer uses modular indexing, so absolute values don't matter.
    if (sampleCounter_ > 1000000) {
        int shift = sampleCounter_ - OSS_BUFFER_SIZE;
        sampleCounter_ -= shift;
        lastBeatSample_ -= shift;
        lastTransientSample_ -= shift;
        if (lastBeatSample_ < 0) lastBeatSample_ = 0;
        if (lastTransientSample_ < 0) lastTransientSample_ = -1;
    }
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

void AudioController::detectBeat() {
    uint32_t nowMs = time_.millis();

    timeToNextBeat_--;
    timeToNextPrediction_--;

    bool beatDetected = false;

    // Run prediction at beat midpoint
    if (timeToNextPrediction_ <= 0) {
        predictBeat();
    }

    // Beat declared when countdown reaches zero
    if (timeToNextBeat_ <= 0) {
        lastBeatSample_ = sampleCounter_;
        if (beatCount_ < 65535) beatCount_++;
        beatDetected = true;
        cbssConfidence_ = clampf(cbssConfidence_ + 0.15f, 0.0f, 1.0f);
        updateBeatStability(nowMs);

        // Capture whether prediction refined this beat's timing (for streaming)
        // Must happen before reset so streaming reads the correct value
        lastFiredBeatPredicted_ = lastBeatWasPredicted_;

        // Reset timers to prevent re-firing on subsequent frames.
        // Use current beat period as fallback; next prediction will refine.
        int T = beatPeriodSamples_;
        if (T < 10) T = 10;
        timeToNextBeat_ = T;
        timeToNextPrediction_ = T / 2;
        lastBeatWasPredicted_ = false;  // Reset; prediction will set true when it runs
    }

    // Decay confidence when no beat
    if (!beatDetected) {
        cbssConfidence_ *= beatConfidenceDecay;
    }

    // Derive phase deterministically
    int T = beatPeriodSamples_;
    if (T < 10) T = 10;
    float newPhase = static_cast<float>(sampleCounter_ - lastBeatSample_) / static_cast<float>(T);
    newPhase = fmodf(newPhase, 1.0f);
    if (newPhase < 0.0f) newPhase += 1.0f;
    if (!isfinite(newPhase)) newPhase = 0.0f;
    phase_ = newPhase;

    predictNextBeat(nowMs);
}

// ===== OUTPUT SYNTHESIS =====

void AudioController::synthesizeEnergy() {
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
    // Use ensemble transient strength instead of single-detector output
    float pulse = lastEnsembleOutput_.transientStrength;

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
    float strength = periodicityStrength_ * 0.6f + cbssConfidence_ * 0.4f;

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
}

void AudioController::updateOnsetDensity(uint32_t nowMs) {
    // Update every 1 second
    uint32_t elapsed = nowMs - onsetDensityWindowStart_;
    if (elapsed >= 1000) {
        float rawDensity = (float)onsetCountInWindow_ * (1000.0f / (float)elapsed);
        // EMA: 70/30 blend (matches periodicityStrength_ pattern)
        onsetDensity_ = onsetDensity_ * 0.7f + rawDensity * 0.3f;
        onsetCountInWindow_ = 0;
        onsetDensityWindowStart_ = nowMs;
    }
    control_.onsetDensity = onsetDensity_;
}

// ============================================================================
// Pulse Train Evaluation (Percival & Tzanetakis 2014)
// ============================================================================

int AudioController::evaluatePulseTrains(const int* candidateLags, const float* candidateScores,
                                          int numCandidates, float samplesPerMs, bool debugPrint) {
    if (numCandidates <= 0) return 0;
    if (numCandidates == 1) return candidateLags[0];

    static constexpr int MAX_CANDIDATES = 10;
    if (numCandidates > MAX_CANDIDATES) numCandidates = MAX_CANDIDATES;

    float magScores[MAX_CANDIDATES];
    float varScores[MAX_CANDIDATES];

    for (int c = 0; c < numCandidates; c++) {
        int lag = candidateLags[c];
        magScores[c] = 0.0f;
        varScores[c] = 0.0f;

        // Need at least 4 beats at fundamental to evaluate
        if (lag <= 0 || ossCount_ < lag * 4) continue;

        float maxMag = 0.0f;
        float sumMag = 0.0f;
        float sumMagSq = 0.0f;

        for (int phase = 0; phase < lag; phase++) {
            float score = 0.0f;

            for (int b = 0; b < 4; b++) {
                // Fundamental (weight 1.0)
                int idx = phase + b * lag;
                if (idx < ossCount_) {
                    int bufIdx = (ossWriteIdx_ - ossCount_ + idx + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
                    score += ossBuffer_[bufIdx];
                }

                // Double period (weight 0.5) — every 2 beats
                idx = phase + b * lag * 2;
                if (idx < ossCount_) {
                    int bufIdx = (ossWriteIdx_ - ossCount_ + idx + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
                    score += 0.5f * ossBuffer_[bufIdx];
                }

                // 3/2 period (weight 0.5) — compound meter support
                idx = phase + b * (lag * 3 / 2);
                if (idx < ossCount_) {
                    int bufIdx = (ossWriteIdx_ - ossCount_ + idx + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
                    score += 0.5f * ossBuffer_[bufIdx];
                }
            }

            if (score > maxMag) maxMag = score;
            sumMag += score;
            sumMagSq += score * score;
        }

        magScores[c] = maxMag;

        // Variance = E[x^2] - E[x]^2 (online, no per-phase allocation needed)
        float mean = sumMag / static_cast<float>(lag);
        varScores[c] = (sumMagSq / static_cast<float>(lag)) - (mean * mean);
        if (varScores[c] < 0.0f) varScores[c] = 0.0f;  // numerical safety
    }

    // Normalize each score type to sum to 1, then combine (rank-level fusion)
    // Three signals: onset alignment (mag), phase selectivity (var), autocorrelation+prior (acf)
    // The autocorrelation score anchors the fusion to the tempo prior, preventing
    // the pulse train from overriding to sub-harmonics that happen to align with onsets.
    float sumMag = 0.0f, sumVar = 0.0f, sumAcf = 0.0f;
    for (int c = 0; c < numCandidates; c++) {
        sumMag += magScores[c];
        sumVar += varScores[c];
        sumAcf += candidateScores[c];
    }
    if (sumMag < 1e-9f) sumMag = 1e-9f;
    if (sumVar < 1e-9f) sumVar = 1e-9f;
    if (sumAcf < 1e-9f) sumAcf = 1e-9f;

    float bestCombo = -1.0f;
    int bestIdx = 0;
    for (int c = 0; c < numCandidates; c++) {
        float combo = magScores[c] / sumMag + varScores[c] / sumVar + candidateScores[c] / sumAcf;
        if (combo > bestCombo) {
            bestCombo = combo;
            bestIdx = c;
        }
    }

    if (debugPrint) {
        float winBpm = 60000.0f / (static_cast<float>(candidateLags[bestIdx]) / samplesPerMs);
        Serial.print(F("{\"type\":\"PULSE_TRAIN\",\"winner\":"));
        Serial.print(winBpm, 1);
        Serial.print(F(",\"n\":"));
        Serial.print(numCandidates);
        Serial.print(F(",\"mag\":"));
        Serial.print(magScores[bestIdx], 2);
        Serial.print(F(",\"var\":"));
        Serial.print(varScores[bestIdx], 3);
        Serial.print(F(",\"acf\":"));
        Serial.print(candidateScores[bestIdx], 3);
        Serial.print(F(",\"cands\":["));
        for (int c = 0; c < numCandidates; c++) {
            if (c > 0) Serial.print(',');
            float bpm = 60000.0f / (static_cast<float>(candidateLags[c]) / samplesPerMs);
            Serial.print(bpm, 0);
        }
        Serial.println(F("]}"));
    }

    return candidateLags[bestIdx];
}

// ============================================================================
// Tempo Prior and Stability Methods
// ============================================================================

float AudioController::computeTempoPrior(float bpm) const {
    if (!tempoPriorEnabled || tempoPriorWidth <= 0.0f) {
        return 1.0f;  // No prior weighting
    }

    // Gaussian prior: exp(-0.5 * ((bpm - center) / width)^2)
    float diff = bpm - tempoPriorCenter;
    float normalized = diff / tempoPriorWidth;
    float gaussianWeight = expf(-0.5f * normalized * normalized);

    // Blend between no-prior (1.0) and full prior based on tempoPriorStrength
    // At strength=0: return 1.0 (no prior effect)
    // At strength=1: return gaussianWeight (full prior)
    return 1.0f + tempoPriorStrength * (gaussianWeight - 1.0f);
}

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

float AudioController::computeSpectralFlux(const float* magnitudes, int numBins) {
    // Wrapper for backward compatibility - discards per-band outputs
    float bassFlux, midFlux, highFlux;
    return computeSpectralFluxBands(magnitudes, numBins, bassFlux, midFlux, highFlux);
}

float AudioController::computeSpectralFluxBands(const float* magnitudes, int numBins,
                                                 float& outBassFlux, float& outMidFlux, float& outHighFlux) {
    // Band-weighted half-wave rectified spectral flux with SuperFlux-style vibrato suppression
    // Captures frame-to-frame energy INCREASES only (onsets, not decays)
    //
    // Key insight from SuperFlux (Böck & Widmer, DAFx 2013):
    // - Vibrato causes narrow-band periodic fluctuations
    // - Maximum filtering across adjacent bins smooths these out
    // - Broadband transients (real beats) are preserved
    //
    // Band weighting: use adaptive weights if enabled, otherwise fixed defaults
    // Sample rate: 16kHz, FFT size: 256, bin resolution: 62.5 Hz/bin
    // Bass: bins 1-10 (62.5Hz-625Hz)
    // Mid: bins 11-40 (687.5Hz-2.5kHz)
    // High: bins 41-127 (2.56kHz-7.94kHz)

    float bassFlux = 0.0f;
    float midFlux = 0.0f;
    float highFlux = 0.0f;
    int bassBinCount = 0, midBinCount = 0, highBinCount = 0;

    // Noise floor threshold - ignore tiny fluctuations in sustained content
    const float FLUX_NOISE_FLOOR = 0.005f;

    int binsUsed = numBins < SPECTRAL_BINS ? numBins : SPECTRAL_BINS;

    if (prevMagnitudesValid_) {
        // SuperFlux-style vibrato suppression:
        // Compare current magnitude against MAX of previous neighboring bins
        // This suppresses narrow-band fluctuations (vibrato) while preserving broadband transients

        // Bass band: bins 1-10 (use max_size=1 for bass, less vibrato there)
        for (int i = 1; i < 11 && i < binsUsed; i++) {
            // Use max-filtered previous value (vibrato suppression)
            float diff = magnitudes[i] - maxFilteredPrevMags_[i];
            if (diff > FLUX_NOISE_FLOOR) {  // Half-wave rectify with noise gate
                bassFlux += diff;
            }
            bassBinCount++;
        }
        // Mid band: bins 11-40 (primary vibrato region, use max filter)
        for (int i = 11; i < 41 && i < binsUsed; i++) {
            float diff = magnitudes[i] - maxFilteredPrevMags_[i];
            if (diff > FLUX_NOISE_FLOOR) {
                midFlux += diff;
            }
            midBinCount++;
        }
        // High band: bins 41+ (tremolo/vibrato common here too)
        for (int i = 41; i < binsUsed; i++) {
            float diff = magnitudes[i] - maxFilteredPrevMags_[i];
            if (diff > FLUX_NOISE_FLOOR) {
                highFlux += diff;
            }
            highBinCount++;
        }

        // Normalize each band by bin count
        if (bassBinCount > 0) bassFlux /= static_cast<float>(bassBinCount);
        if (midBinCount > 0) midFlux /= static_cast<float>(midBinCount);
        if (highBinCount > 0) highFlux /= static_cast<float>(highBinCount);
    }

    // Store current frame for next comparison (with max filtering for vibrato suppression)
    for (int i = 0; i < binsUsed; i++) {
        prevMagnitudes_[i] = magnitudes[i];
    }
    // Apply max filter: each bin becomes max of itself and neighbors
    // This is the key SuperFlux insight - vibrato affects single bins,
    // but real onsets affect multiple adjacent bins
    for (int i = 1; i < binsUsed - 1; i++) {
        float maxVal = prevMagnitudes_[i];
        if (prevMagnitudes_[i - 1] > maxVal) maxVal = prevMagnitudes_[i - 1];
        if (prevMagnitudes_[i + 1] > maxVal) maxVal = prevMagnitudes_[i + 1];
        maxFilteredPrevMags_[i] = maxVal;
    }
    // Edge bins: just copy
    maxFilteredPrevMags_[0] = prevMagnitudes_[0];
    if (binsUsed > 1) {
        maxFilteredPrevMags_[binsUsed - 1] = prevMagnitudes_[binsUsed - 1];
    }
    prevMagnitudesValid_ = true;

    // Output individual band fluxes (pre-compression) for adaptive weighting
    outBassFlux = bassFlux;
    outMidFlux = midFlux;
    outHighFlux = highFlux;

    // Weighted sum: use adaptive weights if enabled, otherwise fixed defaults
    float flux;
    if (adaptiveBandWeightEnabled) {
        flux = adaptiveBandWeights_[0] * bassFlux +
               adaptiveBandWeights_[1] * midFlux +
               adaptiveBandWeights_[2] * highFlux;
    } else {
        // Fixed tunable weights
        flux = bassBandWeight * bassFlux + midBandWeight * midFlux + highBandWeight * highFlux;
    }

    // Apply log compression for dynamic range
    // Maps flux to 0-1 range with soft knee at low values
    // log(1 + x*10) / log(11) maps [0, 1] -> [0, 1] with compression
    static const float invLog11 = 1.0f / logf(11.0f);
    float compressed = logf(1.0f + flux * 10.0f) * invLog11;

    return compressed;
}

float AudioController::computeMultiBandRms(const float* magnitudes, int numBins) {
    // Legacy multi-band RMS energy calculation
    // Kept for comparison and fallback
    //
    // Sample rate: 16kHz, FFT size: 256, bin resolution: 62.5 Hz/bin
    // Bass: bins 1-10 (62.5Hz-625Hz)
    // Mid: bins 11-40 (687.5Hz-2.5kHz)
    // High: bins 41-127 (2.56kHz-7.94kHz)

    float bassEnergy = 0.0f;
    float midEnergy = 0.0f;
    float highEnergy = 0.0f;
    int bassBinCount = 0, midBinCount = 0, highBinCount = 0;

    for (int i = 1; i < 11 && i < numBins; i++) {
        bassEnergy += magnitudes[i] * magnitudes[i];
        bassBinCount++;
    }
    for (int i = 11; i < 41 && i < numBins; i++) {
        midEnergy += magnitudes[i] * magnitudes[i];
        midBinCount++;
    }
    for (int i = 41; i < numBins; i++) {
        highEnergy += magnitudes[i] * magnitudes[i];
        highBinCount++;
    }

    // RMS (root mean square) - use actual bin count for accurate normalization
    bassEnergy = bassBinCount > 0 ? sqrtf(bassEnergy / static_cast<float>(bassBinCount)) : 0.0f;
    midEnergy = midBinCount > 0 ? sqrtf(midEnergy / static_cast<float>(midBinCount)) : 0.0f;
    highEnergy = highBinCount > 0 ? sqrtf(highEnergy / static_cast<float>(highBinCount)) : 0.0f;

    // Weighted sum: emphasize bass and mid for rhythm (where most beats occur)
    return 0.5f * bassEnergy + 0.3f * midEnergy + 0.2f * highEnergy;
}

// ============================================================================
// Adaptive Band Weighting Methods
// ============================================================================

void AudioController::addBandOssSamples(float bassFlux, float midFlux, float highFlux) {
    // Store per-band flux values in circular buffers
    bandOssBuffers_[0][bandOssWriteIdx_] = bassFlux;
    bandOssBuffers_[1][bandOssWriteIdx_] = midFlux;
    bandOssBuffers_[2][bandOssWriteIdx_] = highFlux;

    bandOssWriteIdx_ = (bandOssWriteIdx_ + 1) % BAND_OSS_BUFFER_SIZE;
    if (bandOssCount_ < BAND_OSS_BUFFER_SIZE) {
        bandOssCount_++;
    }
}

float AudioController::computeBandAutocorrelation(int band) {
    // Simplified autocorrelation for a single band
    // Returns the maximum correlation strength in the valid BPM range
    //
    // We use the user's bpmMin/bpmMax settings for the lag range

    // Bounds check
    if (band < 0 || band >= BAND_COUNT) {
        return 0.0f;
    }

    // Need at least 1 second of data for meaningful autocorrelation
    if (bandOssCount_ < 60) {
        return 0.0f;  // Not enough data
    }

    const float* buffer = bandOssBuffers_[band];

    // Number of valid samples to use
    int validCount = bandOssCount_;

    // Estimate frame rate from buffer timing (assume ~60 Hz)
    const float frameRate = 60.0f;

    // Use user's BPM range settings for lag calculation
    // At 60 Hz: 60 BPM = 60 frames/beat, 200 BPM = 18 frames/beat
    int minLag = static_cast<int>(frameRate * 60.0f / bpmMax);  // Faster tempo = shorter lag
    int maxLag = static_cast<int>(frameRate * 60.0f / bpmMin);  // Slower tempo = longer lag

    // Clamp to buffer size (need at least lag samples for correlation)
    if (maxLag > validCount / 2) maxLag = validCount / 2;
    if (minLag < 1) minLag = 1;
    if (maxLag <= minLag) {
        return 0.0f;  // Invalid range
    }

    float maxCorr = 0.0f;

    // Compute mean for normalization using circular buffer indexing
    float mean = 0.0f;
    for (int i = 0; i < validCount; i++) {
        int idx = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
        mean += buffer[idx];
    }
    mean /= static_cast<float>(validCount);

    // Compute variance for normalization
    float variance = 0.0f;
    for (int i = 0; i < validCount; i++) {
        int idx = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
        float diff = buffer[idx] - mean;
        variance += diff * diff;
    }
    if (variance < 0.0001f) {
        return 0.0f;  // No signal variation
    }

    // Test lags in BPM range
    for (int lag = minLag; lag <= maxLag; lag++) {
        float correlation = 0.0f;
        int count = 0;

        for (int i = 0; i < validCount - lag; i++) {
            int idx1 = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
            int idx2 = (idx1 - lag + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
            correlation += (buffer[idx1] - mean) * (buffer[idx2] - mean);
            count++;
        }

        if (count > 0) {
            correlation /= variance;  // Normalize
            if (correlation > maxCorr) {
                maxCorr = correlation;
            }
        }
    }

    return maxCorr;
}

void AudioController::computeCrossBandCorrelation() {
    // Cross-band correlation: measure how synchronized the bands are
    // Real beats cause correlated peaks across multiple bands
    // Vibrato/tremolo affects individual bands independently
    //
    // For each band, compute correlation with the sum of other bands
    // High correlation = this band's periodicity is synchronized with others

    if (bandOssCount_ < 60) {
        // Not enough data
        for (int i = 0; i < BAND_COUNT; i++) {
            crossBandCorrelation_[i] = 0.0f;
        }
        bandSynchrony_ = 0.0f;
        return;
    }

    // Compute mean of each band
    float bandMeans[BAND_COUNT] = {0};
    for (int band = 0; band < BAND_COUNT; band++) {
        for (int i = 0; i < bandOssCount_; i++) {
            int idx = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
            bandMeans[band] += bandOssBuffers_[band][idx];
        }
        bandMeans[band] /= static_cast<float>(bandOssCount_);
    }

    // Compute variance of each band
    float bandVariances[BAND_COUNT] = {0};
    for (int band = 0; band < BAND_COUNT; band++) {
        for (int i = 0; i < bandOssCount_; i++) {
            int idx = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
            float diff = bandOssBuffers_[band][idx] - bandMeans[band];
            bandVariances[band] += diff * diff;
        }
    }

    // Compute correlation of each band with sum of others
    float totalCorr = 0.0f;
    for (int band = 0; band < BAND_COUNT; band++) {
        if (bandVariances[band] < 0.0001f) {
            crossBandCorrelation_[band] = 0.0f;
            continue;
        }

        // Sum of other bands
        float otherVariance = 0.0f;
        float covariance = 0.0f;

        for (int i = 0; i < bandOssCount_; i++) {
            int idx = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
            float thisVal = bandOssBuffers_[band][idx] - bandMeans[band];

            // Sum of other bands at this sample
            float otherSum = 0.0f;
            float otherMeanSum = 0.0f;
            for (int other = 0; other < BAND_COUNT; other++) {
                if (other != band) {
                    otherSum += bandOssBuffers_[other][idx];
                    otherMeanSum += bandMeans[other];
                }
            }
            float otherVal = otherSum - otherMeanSum;
            covariance += thisVal * otherVal;
            otherVariance += otherVal * otherVal;
        }

        if (otherVariance > 0.0001f) {
            float correlation = covariance / sqrtf(bandVariances[band] * otherVariance);
            crossBandCorrelation_[band] = clampf(correlation, 0.0f, 1.0f);
        } else {
            crossBandCorrelation_[band] = 0.0f;
        }

        totalCorr += crossBandCorrelation_[band];
    }

    // Overall synchrony = average cross-band correlation
    bandSynchrony_ = totalCorr / BAND_COUNT;
}

void AudioController::computeBandPeakiness() {
    // Peakiness: ratio of peak values to RMS
    // Transients: sparse, high peaks (high peakiness)
    // Vibrato: continuous, low-level fluctuations (low peakiness)
    //
    // Crest factor = peak / RMS
    // High crest factor indicates impulsive/transient signals

    if (bandOssCount_ < 60) {
        for (int i = 0; i < BAND_COUNT; i++) {
            bandPeakiness_[i] = 0.0f;
        }
        return;
    }

    for (int band = 0; band < BAND_COUNT; band++) {
        float sumSquares = 0.0f;
        float maxVal = 0.0f;

        for (int i = 0; i < bandOssCount_; i++) {
            int idx = (bandOssWriteIdx_ - 1 - i + BAND_OSS_BUFFER_SIZE) % BAND_OSS_BUFFER_SIZE;
            float val = bandOssBuffers_[band][idx];
            sumSquares += val * val;
            if (val > maxVal) maxVal = val;
        }

        float rms = sqrtf(sumSquares / static_cast<float>(bandOssCount_));
        if (rms > 0.001f) {
            // Crest factor, normalized to 0-1 range
            // Typical crest factor: 1.4 (sine) to 10+ (impulsive)
            // Map: 1.5 -> 0, 5.0 -> 1
            float crestFactor = maxVal / rms;
            bandPeakiness_[band] = clampf((crestFactor - 1.5f) / 3.5f, 0.0f, 1.0f);
        } else {
            bandPeakiness_[band] = 0.0f;
        }
    }
}

void AudioController::updateBandPeriodicities(uint32_t nowMs) {
    (void)nowMs;  // Unused for now

    // Step 1: Run autocorrelation on each band
    for (int band = 0; band < BAND_COUNT; band++) {
        float maxCorr = computeBandAutocorrelation(band);

        // Faster EMA convergence (0.5/0.5) - responds in ~2 updates
        bandPeriodicityStrength_[band] =
            0.5f * bandPeriodicityStrength_[band] + 0.5f * maxCorr;
    }

    // Step 2: Compute cross-band correlation (sustained sound rejection)
    computeCrossBandCorrelation();

    // Step 3: Compute peakiness (transient vs sustained classification)
    computeBandPeakiness();

    // Step 4: Combine metrics for final weight calculation
    // Key insight: Only trust periodicity when:
    // - Bands are synchronized (real beats hit multiple bands)
    // - Signal is peaky (transients, not vibrato)

    // Default weights (used when no clear rhythmic periodicity)
    // Uses tunable public variables for real-time calibration
    const float defaultWeights[BAND_COUNT] = {bassBandWeight, midBandWeight, highBandWeight};

    // Calculate effective strength: periodicity × cross-band-correlation × peakiness
    // This suppresses vibrato (low cross-band-corr) and tremolo (low peakiness)
    float effectiveStrength[BAND_COUNT];
    float totalEffective = 0.0f;
    float maxEffective = 0.0f;

    for (int i = 0; i < BAND_COUNT; i++) {
        // Multiplicative combination:
        // - High periodicity alone is NOT enough (could be vibrato)
        // - Need cross-band correlation (real beats hit multiple bands)
        // - Prefer peaky signals (transients, not continuous)
        float syncFactor = 0.3f + 0.7f * crossBandCorrelation_[i];  // 0.3-1.0
        float peakFactor = 0.5f + 0.5f * bandPeakiness_[i];         // 0.5-1.0

        effectiveStrength[i] = bandPeriodicityStrength_[i] * syncFactor * peakFactor;
        totalEffective += effectiveStrength[i];

        if (effectiveStrength[i] > maxEffective) {
            maxEffective = effectiveStrength[i];
        }
    }

    // Only use adaptive weighting when there's STRONG effective periodicity
    // AND good band synchrony (indicates real beat, not vibrato)
    float avgEffective = totalEffective / BAND_COUNT;

    if (totalEffective > 0.1f && avgEffective > 0.15f && bandSynchrony_ > 0.3f) {
        // Calculate dominance factor
        float dominance = (maxEffective / totalEffective) * BAND_COUNT;  // 1.0 = equal, 3.0 = one dominates

        // Scale adaptive blend by:
        // - Effective strength (periodicity × sync × peakiness)
        // - Dominance (one band clearly leads)
        // - Overall synchrony (bands correlate)
        float strengthFactor = clampf((avgEffective - 0.15f) / 0.35f, 0.0f, 1.0f);
        float dominanceFactor = clampf((dominance - 1.0f) / 2.0f, 0.0f, 1.0f);
        float syncFactor = clampf((bandSynchrony_ - 0.3f) / 0.4f, 0.0f, 1.0f);

        // Require all three factors to be good for full adaptive weighting
        float adaptiveBlend = strengthFactor * dominanceFactor * syncFactor * 0.7f;  // Max 70% adaptive

        // Blend: more adaptive when all conditions are met
        for (int i = 0; i < BAND_COUNT; i++) {
            float adaptiveWeight = effectiveStrength[i] / totalEffective;
            adaptiveBandWeights_[i] = adaptiveBlend * adaptiveWeight + (1.0f - adaptiveBlend) * defaultWeights[i];
        }

        // Ensure weights sum to 1.0
        float weightSum = adaptiveBandWeights_[0] + adaptiveBandWeights_[1] + adaptiveBandWeights_[2];
        if (weightSum > 0.0f) {
            for (int i = 0; i < BAND_COUNT; i++) {
                adaptiveBandWeights_[i] /= weightSum;
            }
        }
    } else {
        // Conditions not met (low sync, low peakiness, etc.) - use defaults
        // This is the key improvement: sustained content fails these checks
        for (int i = 0; i < BAND_COUNT; i++) {
            adaptiveBandWeights_[i] = defaultWeights[i];
        }
    }
}

// ============================================================================
// Pulse Train Phase Estimation (Phase 3)
// ============================================================================

float AudioController::generateAndCorrelate(int phaseOffset, int beatPeriod) {
    // Generate ideal pulse train and correlate with OSS buffer in one pass
    // This is more memory-efficient than generating a full buffer
    //
    // Pulse train: Gaussian pulses at beat positions
    // sigma = 10% of beat period (captures timing variance)

    if (beatPeriod < 10 || ossCount_ < beatPeriod) {
        return 0.0f;
    }

    const float sigma = 0.1f;  // 10% of beat period
    const float sigmaSquared2 = 2.0f * sigma * sigma;

    float correlation = 0.0f;
    float pulseEnergy = 0.0f;
    float ossEnergy = 0.0f;

    // Correlate over the OSS buffer
    int samplesToUse = ossCount_ < OSS_BUFFER_SIZE ? ossCount_ : OSS_BUFFER_SIZE;

    for (int i = 0; i < samplesToUse; i++) {
        int ossIdx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        float ossVal = ossBuffer_[ossIdx];

        // Generate pulse train value at this position
        // Position in beat cycle (0 to beatPeriod-1)
        int posInBeat = (i + phaseOffset) % beatPeriod;

        // Distance from nearest beat (0 = on beat)
        float dist = static_cast<float>(posInBeat) / static_cast<float>(beatPeriod);
        if (dist > 0.5f) {
            dist = 1.0f - dist;  // Wrap around: 0.9 -> 0.1 distance
        }

        // Gaussian pulse: exp(-0.5 * (dist/sigma)^2)
        float pulseVal = expf(-0.5f * (dist * dist) / sigmaSquared2);

        // Accumulate correlation
        correlation += ossVal * pulseVal;
        pulseEnergy += pulseVal * pulseVal;
        ossEnergy += ossVal * ossVal;
    }

    // Normalize correlation
    float normalizer = sqrtf(pulseEnergy * ossEnergy);
    if (normalizer > 0.0001f) {
        correlation /= normalizer;
    } else {
        correlation = 0.0f;
    }

    return correlation;
}

float AudioController::computePulseTrainPhase(int beatPeriodSamples) {
    // Extract phase using Fourier analysis at the tempo frequency
    // Based on Predominant Local Pulse (PLP) method by Grosche & Müller (2011)
    //
    // Key insight: The phase of the DFT coefficient at the tempo frequency
    // directly gives the beat alignment. This is more efficient and accurate
    // than brute-force template matching.
    //
    // For tempo period T samples, compute DFT at frequency f = 1/T:
    //   X(f) = Σ oss[n] * exp(-j*2π*f*n)
    //        = Σ oss[n] * cos(2π*n/T) - j * Σ oss[n] * sin(2π*n/T)
    //   Phase = atan2(imag, real)

    if (beatPeriodSamples < 10 || ossCount_ < 60) {
        pulseTrainConfidence_ = 0.0f;
        return 0.0f;
    }

    // Compute DFT at tempo frequency using Goertzel-like approach
    // This is O(n) instead of O(n log n) for full FFT
    float realSum = 0.0f;
    float imagSum = 0.0f;
    float ossEnergy = 0.0f;

    // Angular frequency per sample: 2π / beatPeriodSamples
    const float angularFreq = 2.0f * 3.14159265f / static_cast<float>(beatPeriodSamples);

    int samplesToUse = ossCount_ < OSS_BUFFER_SIZE ? ossCount_ : OSS_BUFFER_SIZE;

    for (int i = 0; i < samplesToUse; i++) {
        int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        float ossVal = ossBuffer_[idx];

        // DFT at tempo frequency
        float angle = angularFreq * static_cast<float>(i);
        realSum += ossVal * cosf(angle);
        imagSum += ossVal * sinf(angle);
        ossEnergy += ossVal * ossVal;
    }

    // Normalize by OSS energy to get confidence (0-1)
    // High magnitude relative to energy = strong periodicity at this tempo
    float normalizer = sqrtf(ossEnergy * static_cast<float>(samplesToUse));
    if (normalizer > 0.0001f) {
        float magnitude = sqrtf(realSum * realSum + imagSum * imagSum);
        pulseTrainConfidence_ = clampf(magnitude / normalizer, 0.0f, 1.0f);
    } else {
        pulseTrainConfidence_ = 0.0f;
        return 0.0f;
    }

    // Extract phase from complex coefficient
    // atan2 gives angle in [-π, π], convert to [0, 1]
    float phaseRadians = atan2f(imagSum, realSum);

    // Convert to beat phase (0-1)
    // Phase 0 = beat peak aligns with most recent sample
    // We negate because we're looking backward in time
    float phase = -phaseRadians / (2.0f * 3.14159265f);

    // Normalize to [0, 1)
    if (phase < 0.0f) phase += 1.0f;
    if (phase >= 1.0f) phase -= 1.0f;

    return clampf(phase, 0.0f, 1.0f);
}

// ============================================================================
// Comb Filter Bank Implementation (Independent Tempo Validation)
// ============================================================================

void CombFilterBank::init(float frameRate) {
    frameRate_ = frameRate;

    // Compute lag and BPM for each filter
    // Distribute filters evenly from MIN_LAG (180 BPM) to MAX_LAG (60 BPM)
    // At 60 Hz: lag 20 = 180 BPM, lag 60 = 60 BPM
    for (int i = 0; i < NUM_FILTERS; i++) {
        // Linear interpolation of lag values
        float t = static_cast<float>(i) / static_cast<float>(NUM_FILTERS - 1);
        int lag = MIN_LAG + static_cast<int>(t * (MAX_LAG - MIN_LAG) + 0.5f);
        filterLags_[i] = lag;

        // Convert lag to BPM: BPM = frameRate * 60 / lag
        filterBPMs_[i] = (frameRate_ * 60.0f) / static_cast<float>(lag);
    }

    reset();
    initialized_ = true;
}

void CombFilterBank::reset() {
    // Clear delay line
    for (int i = 0; i < MAX_LAG; i++) {
        delayLine_[i] = 0.0f;
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

    // 1. Write input to shared delay line
    delayLine_[writeIdx_] = input;

    // 2. Update all resonators using proper Scheirer (1998) equation:
    //    y[n] = (1-α)·x[n] + α·y[n-L]
    //    where α = feedbackGain
    float oneMinusAlpha = 1.0f - feedbackGain;

    for (int i = 0; i < NUM_FILTERS; i++) {
        int lag = filterLags_[i];

        // Read delayed output (y[n-L])
        int readIdx = (writeIdx_ - lag + MAX_LAG) % MAX_LAG;
        float delayed = delayLine_[readIdx];

        // Proper comb filter equation
        resonatorOutput_[i] = oneMinusAlpha * input + feedbackGain * delayed;

        // Smooth energy tracking (exponential moving average)
        float absOut = resonatorOutput_[i] > 0.0f ? resonatorOutput_[i] : -resonatorOutput_[i];
        resonatorEnergy_[i] = 0.95f * resonatorEnergy_[i] + 0.05f * absOut;
    }

    // 3. Advance delay line write index
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

