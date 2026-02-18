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
    targetPhase_ = 0.0f;
    pulseTrainPhase_ = 0.0f;
    pulseTrainConfidence_ = 0.0f;
    transientPhaseError_ = 0.0f;
    lastTransientCorrectionMs_ = 0;

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

    // Reset comb filter phase tracker
    combFilter_.reset();

    // Initialize and reset comb filter bank
    // Uses 60 Hz frame rate assumption (same as OSS buffer)
    combFilterBank_.init(60.0f);

    // Reset fusion state
    fusedPhase_ = 0.0f;
    fusedConfidence_ = 0.0f;
    consensusMetric_ = 0.0f;
    transientHint_ = 0.0f;

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

    // 4. Get onset strength for rhythm analysis
    //    This is INDEPENDENT of transient detection - analyzes energy patterns
    //    Two methods available:
    //    - Spectral flux: captures energy CHANGES (better for beat timing)
    //    - Multi-band RMS: captures absolute levels (legacy behavior)
    //    Controlled by ossFluxWeight parameter (1.0 = pure flux, 0.0 = pure RMS)
    float onsetStrength = 0.0f;
    float bassFlux = 0.0f, midFlux = 0.0f, highFlux = 0.0f;

    // Get spectral data from ensemble
    const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();

    if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
        const float* magnitudes = spectral.getMagnitudes();
        int numBins = spectral.getNumBins();

        // Compute spectral flux with per-band outputs for adaptive weighting
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

    // 3. Add sample to onset strength buffer with timestamp
    // Only add significant audio to avoid filling buffer with noise patterns
    if (hasSignificantAudio) {
        lastSignificantAudioMs_ = nowMs;
        addOssSample(onsetStrength, nowMs);
    } else {
        // Add zero during silence to maintain buffer timing but not contribute to correlation
        addOssSample(0.0f, nowMs);
    }

    // 4. Run autocorrelation periodically (tunable period, default 500ms)
    if (nowMs - lastAutocorrMs_ >= autocorrPeriodMs) {
        runAutocorrelation(nowMs);
        lastAutocorrMs_ = nowMs;
    }

    // 4b. Update comb filter phase tracker (runs every frame)
    //     Provides independent phase estimate that can be fused with autocorrelation
    if (combFilterWeight > 0.0f) {
        // Update comb filter parameters
        combFilter_.feedbackGain = combFeedback;
        combFilter_.setTempo(bpm_);
        combFilter_.process(onsetStrength);
    }

    // 4c. Update comb filter bank (independent tempo validation)
    //     Provides tempo validation without depending on autocorrelation
    //     Does NOT use tempo prior - that's applied in autocorrelation only
    if (combBankEnabled) {
        // Update bank parameters
        combFilterBank_.feedbackGain = combBankFeedback;

        // Process onset strength through the bank
        combFilterBank_.process(onsetStrength);

        // Boost confidence for matching hypotheses
        float combBPM = combFilterBank_.getPeakBPM();
        float combConf = combFilterBank_.getPeakConfidence();

        // Only apply boost if comb filter has meaningful confidence
        if (combConf > 0.1f) {
            for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
                TempoHypothesis& hyp = multiHypothesis_.hypotheses[i];
                if (!hyp.active) continue;

                // Check if hypothesis BPM matches comb filter BPM (within 5%)
                float bpmDiff = hyp.bpm > combBPM ?
                    (hyp.bpm - combBPM) / hyp.bpm :
                    (combBPM - hyp.bpm) / combBPM;

                if (bpmDiff < 0.05f) {
                    // Boost confidence by up to 20%, scaled by match quality
                    float boost = combConf * 0.2f * (1.0f - bpmDiff / 0.05f);
                    hyp.confidence += boost;
                    if (hyp.confidence > 1.0f) hyp.confidence = 1.0f;
                }
            }
        }
    }

    // 5. Update transient-based phase correction (PLL)
    //    When transients are detected, use them to nudge phase toward actual beat timing
    updateTransientPhaseCorrection(lastEnsembleOutput_.transientStrength, nowMs);

    // 6. Update phase tracking
    updatePhase(dt, nowMs);

    // 6b. Fuse rhythm estimates from multiple systems
    //     Combines autocorrelation, Fourier phase, and comb filter with confidence weighting
    if (fusionEnabled) {
        fuseRhythmEstimates();
    }

    // 7. Synthesize output
    synthesizeEnergy();
    synthesizePulse();
    synthesizePhase();
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
    // Need at least 3 seconds of data for reliable tempo estimation
    if (ossCount_ < 180) {
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
    // Fallback should never be needed (ossCount_ >= 180 ensures bufferDurationMs > 0)
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
        // Also decay all hypotheses during silence
        for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
            if (multiHypothesis_.hypotheses[i].active) {
                multiHypothesis_.hypotheses[i].strength *= 0.85f;
                if (multiHypothesis_.hypotheses[i].strength < multiHypothesis_.minStrengthToKeep) {
                    multiHypothesis_.hypotheses[i].active = false;
                }
            }
        }
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

    for (int lag = minLag; lag <= maxLag && (lag - minLag) < 200; lag++) {
        float correlation = 0.0f;
        int count = ossCount_ - lag;

        // FIX: Skip if count <= 0 to prevent division by zero
        if (count <= 0) continue;

        for (int i = 0; i < count; i++) {
            int idx1 = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            int idx2 = (ossWriteIdx_ - 1 - i - lag + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            correlation += ossBuffer_[idx1] * ossBuffer_[idx2];
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

    // === MULTI-PEAK EXTRACTION ===
    // Find up to 4 peaks in autocorrelation function for multi-hypothesis tracking
    AutocorrPeak peaks[4];
    int numPeaks = 0;

    // Find local maxima above threshold
    for (int lag = minLag; lag <= maxLag && (lag - minLag) < correlationSize; lag++) {
        int lagIdx = lag - minLag;
        float correlation = correlationAtLag[lagIdx];
        float normCorr = correlation / (avgEnergy + 0.001f);

        // Skip if below minimum strength threshold
        if (normCorr < multiHypothesis_.minPeakStrength) continue;

        // Check if below minimum relative height (must be >70% of max peak)
        if (normCorr < normCorrelation * multiHypothesis_.minRelativePeakHeight) continue;

        // Check if this is a local maximum (±2 neighbors must be lower or equal)
        bool isLocalMax = true;
        bool hasStrictlyLowerNeighbor = false;

        for (int delta = -2; delta <= 2; delta++) {
            if (delta == 0) continue;
            int neighborLag = lag + delta;
            if (neighborLag < minLag || neighborLag > maxLag) continue;
            int neighborIdx = neighborLag - minLag;
            // Upper bounds check (lower bound guaranteed by neighborLag >= minLag check above)
            if (neighborIdx >= correlationSize) continue;

            float neighborCorr = correlationAtLag[neighborIdx];
            if (neighborCorr > correlation) {
                isLocalMax = false;
                break;
            }
            if (neighborCorr < correlation) {
                hasStrictlyLowerNeighbor = true;
            }
        }

        // Accept as peak if local max with at least one strictly lower neighbor
        if (isLocalMax && hasStrictlyLowerNeighbor && numPeaks < 4) {
            // Convert lag to BPM for tempo prior calculation
            float lagBpm = 60000.0f / (static_cast<float>(lag) / samplesPerMs);
            lagBpm = clampf(lagBpm, bpmMin, bpmMax);

            // Apply tempo prior weighting (reduces half-time/double-time confusion)
            float priorWeight = computeTempoPrior(lagBpm);
            float weightedCorr = normCorr * priorWeight;

            // Store for debug (use last peak's prior weight)
            lastTempoPriorWeight_ = priorWeight;

            peaks[numPeaks].lag = lag;
            peaks[numPeaks].correlation = correlation;
            peaks[numPeaks].normCorrelation = weightedCorr;  // Use prior-weighted correlation
            numPeaks++;
        }
    }

    // Sort peaks by prior-weighted strength (descending) - simple selection sort
    for (int i = 0; i < numPeaks - 1; i++) {
        for (int j = i + 1; j < numPeaks; j++) {
            if (peaks[j].normCorrelation > peaks[i].normCorrelation) {
                AutocorrPeak temp = peaks[i];
                peaks[i] = peaks[j];
                peaks[j] = temp;
            }
        }
    }

    // === HARMONIC DISAMBIGUATION ===
    // Problem: Autocorrelation produces peaks at both fundamental and harmonics.
    // At 60 BPM (lag L), there's also a peak at 120 BPM (lag L/2).
    // At 180 BPM (lag L), there's also a peak at 90 BPM (lag 2L).
    // Solution: Check if the strongest peak has a related peak at 2x lag (half BPM).
    // CRITICAL: Only prefer slower tempo if tempo prior ALSO supports it.
    // Otherwise, the tempo prior should be the deciding factor.
    if (numPeaks >= 2 && tempoPriorEnabled) {
        int strongestLag = peaks[0].lag;
        float strongestCorr = peaks[0].normCorrelation;
        float strongestBpm = 60000.0f / (static_cast<float>(strongestLag) / samplesPerMs);

        // Look for a peak at approximately 2x lag (half tempo = fundamental)
        int targetLag = strongestLag * 2;
        constexpr float lagTolerance = 0.08f;  // Allow 8% tolerance for timing jitter

        for (int i = 1; i < numPeaks; i++) {
            float lagRatio = static_cast<float>(peaks[i].lag) / static_cast<float>(targetLag);

            // Check if this peak is near 2x lag (half tempo)
            if (lagRatio > (1.0f - lagTolerance) && lagRatio < (1.0f + lagTolerance)) {
                // Found a candidate fundamental (slower tempo)
                float candidateBpm = 60000.0f / (static_cast<float>(peaks[i].lag) / samplesPerMs);

                // Only prefer slower tempo if:
                // 1. It's strong enough (>60% of harmonic peak)
                // 2. Tempo prior favors the slower tempo over the faster one
                constexpr float fundamentalThreshold = 0.60f;
                float priorFaster = computeTempoPrior(strongestBpm);
                float priorSlower = computeTempoPrior(candidateBpm);

                // Skip if tempo prior favors the faster tempo
                if (priorFaster > priorSlower * 1.1f) {
                    // Tempo prior strongly favors faster tempo - don't switch
                    if (shouldPrintDebug) {
                        Serial.print(F("{\"type\":\"HARMONIC_SKIP\",\"fast\":"));
                        Serial.print(strongestBpm, 1);
                        Serial.print(F(",\"slow\":"));
                        Serial.print(candidateBpm, 1);
                        Serial.print(F(",\"priorFast\":"));
                        Serial.print(priorFaster, 3);
                        Serial.print(F(",\"priorSlow\":"));
                        Serial.print(priorSlower, 3);
                        Serial.println(F("}"));
                    }
                    break;
                }

                if (peaks[i].normCorrelation > strongestCorr * fundamentalThreshold) {
                    // Swap: promote the fundamental to position 0
                    AutocorrPeak temp = peaks[0];
                    peaks[0] = peaks[i];
                    peaks[i] = temp;

                    // Debug output for harmonic disambiguation
                    if (shouldPrintDebug) {
                        Serial.print(F("{\"type\":\"HARMONIC_FIX\",\"from\":"));
                        Serial.print(strongestBpm, 1);
                        Serial.print(F(",\"to\":"));
                        Serial.print(candidateBpm, 1);
                        Serial.print(F(",\"ratio\":"));
                        Serial.print(peaks[i].normCorrelation / strongestCorr, 3);
                        Serial.println(F("}"));
                    }
                    break;  // Only consider first harmonic match
                }
            }
        }
    }

    // === UPDATE MULTI-HYPOTHESIS TRACKER ===
    // Check if any hypotheses are active
    bool hasActiveHypothesis = false;
    for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
        if (multiHypothesis_.hypotheses[i].active) {
            hasActiveHypothesis = true;
            break;
        }
    }

    // For each peak, find matching hypothesis or create new one
    for (int i = 0; i < numPeaks; i++) {
        float bpm = 60000.0f / (static_cast<float>(peaks[i].lag) / samplesPerMs);
        bpm = clampf(bpm, bpmMin, bpmMax);

        // Find matching hypothesis
        int matchSlot = multiHypothesis_.findMatchingHypothesis(bpm);

        if (matchSlot >= 0) {
            // Update existing hypothesis
            TempoHypothesis& hypo = multiHypothesis_.hypotheses[matchSlot];

            // Update phase error tracking:
            // Compare hypothesis phase to Fourier-extracted phase (independent measurement)
            // This avoids circular logic of assuming autocorrelation peak = phase 0
            float phaseError = hypo.phase - pulseTrainPhase_;
            // Wrap to shortest distance around circle [-0.5, 0.5]
            if (phaseError > 0.5f) phaseError -= 1.0f;
            if (phaseError < -0.5f) phaseError += 1.0f;
            phaseError = phaseError < 0.0f ? -phaseError : phaseError;  // Absolute value

            // Exponential smoothing: avgPhaseError = 0.9 * old + 0.1 * new
            // Only update if we have confidence in the Fourier phase
            if (pulseTrainConfidence_ > 0.2f) {
                hypo.avgPhaseError = 0.9f * hypo.avgPhaseError + 0.1f * phaseError;
            }

            hypo.strength = peaks[i].normCorrelation;
            hypo.lastUpdateMs = nowMs;
            hypo.correlationPeak = peaks[i].correlation;
            hypo.lagSamples = peaks[i].lag;
            hypo.bpm = bpm;  // Update BPM (may drift slightly)
            hypo.beatPeriodMs = 60000.0f / bpm;
        } else if (peaks[i].normCorrelation > multiHypothesis_.minPeakStrength ||
                   (!hasActiveHypothesis && i == 0)) {
            // Create new hypothesis if:
            // 1. Peak meets strength threshold, OR
            // 2. This is the first peak and NO hypotheses are active (ensures primary gets initialized)
            multiHypothesis_.createHypothesis(bpm, peaks[i].normCorrelation, nowMs,
                                              peaks[i].lag, peaks[i].correlation);
        }
    }

    // DEBUG: Print correlation results (only when debug enabled)
    if (shouldPrintDebug) {
        // Use adaptive timing formula (not 60 Hz assumption)
        float detectedBpm = (bestLag > 0) ? 60000.0f / (static_cast<float>(bestLag) / samplesPerMs) : 0.0f;
        Serial.print(F("{\"type\":\"RHYTHM_DEBUG2\",\"bestLag\":"));
        Serial.print(bestLag);
        Serial.print(F(",\"maxCorr\":"));
        Serial.print(maxCorrelation, 6);
        Serial.print(F(",\"normCorr\":"));
        Serial.print(normCorrelation, 4);
        Serial.print(F(",\"newStr\":"));
        Serial.print(newStrength, 3);
        Serial.print(F(",\"bpm\":"));
        Serial.print(detectedBpm, 1);
        Serial.println(F("}"));
    }

    // Update tempo if periodicity is strong enough
    // Safety: bestLag > 0 check prevents division by zero in calculations below
    if (bestLag > 0 && periodicityStrength_ > 0.25f) {
        // FIX: Use adaptive samplesPerMs instead of hardcoded 60 Hz assumption
        // Formula: BPM = 60000ms/min / (lag_samples / samplesPerMs) = 60000 * samplesPerMs / lag
        float newBpm = 60000.0f * samplesPerMs / static_cast<float>(bestLag);
        newBpm = clampf(newBpm, bpmMin, bpmMax);

        // Smooth BPM changes
        bpm_ = bpm_ * 0.8f + newBpm * 0.2f;
        beatPeriodMs_ = 60000.0f / bpm_;

        // Derive target phase using pulse train cross-correlation and/or peak-finding
        // Pulse train method: correlate OSS with ideal pulse trains at different phases
        // Peak-finding method: find max OSS in recent beat period
        //
        // pulsePhaseWeight controls the blend:
        //   1.0 = pure pulse train (more robust, considers full pattern)
        //   0.0 = pure peak-finding (legacy method)

        float peakPhase = 0.0f;
        float pulsePhase = 0.0f;
        bool peakPhaseValid = false;
        bool pulsePhaseValid = false;

        // Method 1: Peak-finding (legacy)
        // Skip if pulsePhaseWeight >= 1.0 - result won't be used
        if (pulsePhaseWeight < 1.0f) {
            int recentWindow = bestLag;  // Look at one beat period
            float maxRecent = 0.0f;
            int maxRecentIdx = 0;

            for (int i = 0; i < recentWindow && i < ossCount_; i++) {
                int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
                if (ossBuffer_[idx] > maxRecent) {
                    maxRecent = ossBuffer_[idx];
                    maxRecentIdx = i;
                }
            }

            if (maxRecent > 0.05f) {
                peakPhase = static_cast<float>(maxRecentIdx) / static_cast<float>(bestLag);
                peakPhase = clampf(peakPhase, 0.0f, 1.0f);
                peakPhaseValid = true;
            }
        }

        // Method 2: Fourier phase extraction (Phase 3 - PLP-inspired)
        {
            pulsePhase = computePulseTrainPhase(bestLag);
            pulseTrainPhase_ = pulsePhase;  // Store for debugging

            // Only consider valid if confidence is strong
            // The Fourier method uses the entire OSS buffer, so it needs
            // strong periodicity to produce reliable phase estimates.
            // For weak periodicity (sustained content), fall back to peak-finding.
            // Threshold of 0.45 based on testing: balances tempo-sweep improvement
            // with sustained content rejection.
            if (pulseTrainConfidence_ > 0.45f) {
                pulsePhaseValid = true;
            }
        }

        // Blend the two methods based on pulsePhaseWeight and validity
        float newTargetPhase;
        float effectiveWeight = pulsePhaseWeight;

        // Adjust weight based on validity
        if (!pulsePhaseValid && !peakPhaseValid) {
            // Neither method produced valid result - keep current phase
            newTargetPhase = targetPhase_;
        } else if (!pulsePhaseValid) {
            // Only peak-finding is valid
            newTargetPhase = peakPhase;
        } else if (!peakPhaseValid) {
            // Only pulse train is valid
            newTargetPhase = pulsePhase;
        } else if (effectiveWeight >= 1.0f) {
            newTargetPhase = pulsePhase;
        } else if (effectiveWeight <= 0.0f) {
            newTargetPhase = peakPhase;
        } else {
            // Both valid - circular interpolation for phase (handles wraparound)
            // Weight by confidence: higher pulse train confidence = more pulse weight
            float confAdjustedWeight = effectiveWeight * (0.5f + 0.5f * pulseTrainConfidence_);
            confAdjustedWeight = clampf(confAdjustedWeight, 0.0f, 1.0f);

            // Convert to angles, interpolate, convert back
            float angle1 = peakPhase * 2.0f * 3.14159265f;
            float angle2 = pulsePhase * 2.0f * 3.14159265f;
            float sin1 = sinf(angle1), cos1 = cosf(angle1);
            float sin2 = sinf(angle2), cos2 = cosf(angle2);
            float blendSin = (1.0f - confAdjustedWeight) * sin1 + confAdjustedWeight * sin2;
            float blendCos = (1.0f - confAdjustedWeight) * cos1 + confAdjustedWeight * cos2;
            newTargetPhase = atan2f(blendSin, blendCos) / (2.0f * 3.14159265f);
            if (newTargetPhase < 0.0f) newTargetPhase += 1.0f;
        }

        targetPhase_ = clampf(newTargetPhase, 0.0f, 1.0f);
    }
}

void AudioController::updateTransientPhaseCorrection(float transientStrength, uint32_t nowMs) {
    // Only apply correction when rhythm is established
    if (periodicityStrength_ < activationThreshold) {
        return;
    }

    // Only correct on strong transients (above threshold)
    if (transientStrength < transientCorrectionMin) {
        return;
    }

    // Require 2+ detector agreement to prevent single-detector false positives
    // from gradually drifting the phase estimate
    if (lastEnsembleOutput_.detectorAgreement < 2) {
        return;
    }

    // Rate limit corrections to prevent overcorrection (min 80ms between corrections)
    if (nowMs - lastTransientCorrectionMs_ < 80) {
        return;
    }
    lastTransientCorrectionMs_ = nowMs;

    // Calculate phase error: how far is current phase from 0.0 (beat position)?
    // Phase 0.0 = on the beat, 0.5 = halfway between beats
    //
    // If phase is near 0.0 (e.g., 0.1): transient is slightly late, error = +0.1
    // If phase is near 1.0 (e.g., 0.9): transient is early, error = -0.1 (wrap around)
    //
    // We want to nudge phase so that transients land at phase 0.0
    float phaseError = phase_;
    if (phaseError > 0.5f) {
        // Wrap: treat phase 0.9 as -0.1 (transient came early)
        phaseError = phase_ - 1.0f;
    }

    // Weight error by transient strength (stronger transients = more confident correction)
    float weightedError = phaseError * transientStrength;

    // Exponential moving average of phase error
    // Lower alpha (0.15) means slower convergence, which filters out random
    // false positives while still converging on consistent beat patterns
    const float alpha = 0.15f;
    transientPhaseError_ = (1.0f - alpha) * transientPhaseError_ + alpha * weightedError;

    // Clamp error to reasonable range
    if (transientPhaseError_ > 0.25f) transientPhaseError_ = 0.25f;
    if (transientPhaseError_ < -0.25f) transientPhaseError_ = -0.25f;
}

void AudioController::updatePhase(float dt, uint32_t nowMs) {
    // Determine if we have significant audio (for hypothesis decay strategy)
    int32_t silenceMs = static_cast<int32_t>(nowMs - lastSignificantAudioMs_);
    if (silenceMs < 0) silenceMs = 0;  // Handle wraparound
    bool hasSignificantAudio = (silenceMs < 1000);  // Less than 1 second of silence

    // === UPDATE ALL ACTIVE HYPOTHESES ===
    for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
        if (multiHypothesis_.hypotheses[i].active) {
            multiHypothesis_.updateHypothesis(i, dt, nowMs, hasSignificantAudio);
        }
    }

    // === PROMOTE BEST HYPOTHESIS IF NEEDED ===
    multiHypothesis_.promoteBestHypothesis(nowMs);

    // === USE PRIMARY HYPOTHESIS FOR OUTPUT ===
    TempoHypothesis& primary = multiHypothesis_.getPrimary();

    // Track previous phase for beat detection
    float prevPhase = phase_;

    if (primary.active && primary.strength > activationThreshold) {
        // Safety check for NaN/infinity in primary hypothesis
        if (!isfinite(primary.phase)) {
            primary.phase = 0.0f;
        }
        if (!isfinite(primary.bpm) || primary.bpm < 1.0f) {
            primary.bpm = 120.0f;  // Safe default
        }

        // Track tempo changes for continuous estimation
        float oldBpm = bpm_;

        // Use primary hypothesis values
        phase_ = primary.phase;
        bpm_ = primary.bpm;
        periodicityStrength_ = primary.strength;

        // Update ensemble detector with tempo hint for adaptive cooldown
        // This allows faster detection at higher tempos
        ensemble_.getFusion().setTempoHint(bpm_);

        // Update tempo velocity if BPM changed significantly
        // FIX: Guard against division by zero (oldBpm should never be 0, but be defensive)
        if (oldBpm > 1.0f && fabsf(bpm_ - oldBpm) / oldBpm > tempoChangeThreshold) {
            updateTempoVelocity(bpm_, dt);
        }

        // Gradually adapt phase toward target (derived from autocorrelation)
        // This provides additional smoothing on top of hypothesis tracking
        if (periodicityStrength_ > activationThreshold) {
            float phaseDiff = targetPhase_ - phase_;

            // Handle wraparound
            if (phaseDiff > 0.5f) phaseDiff -= 1.0f;
            if (phaseDiff < -0.5f) phaseDiff += 1.0f;

            // Apply gradual correction from autocorrelation
            phase_ += phaseDiff * phaseAdaptRate * dt * 10.0f;

            // === TRANSIENT-BASED PHASE CORRECTION (PLL) ===
            // Apply correction based on running average of transient timing errors
            // This nudges phase so that transients land closer to phase 0.0
            // transientPhaseError_ > 0 means transients arrive late (phase is ahead)
            // transientPhaseError_ < 0 means transients arrive early (phase is behind)
            // Correction: subtract error to align phase with actual transients
            if (fabsf(transientPhaseError_) > 0.01f) {
                phase_ -= transientPhaseError_ * transientCorrectionRate * dt * 10.0f;
            }

            // FIX: Check for NaN before fmodf (NaN persists through fmodf)
            if (!isfinite(phase_)) {
                phase_ = 0.0f;
            }

            // Re-wrap after correction
            phase_ = fmodf(phase_, 1.0f);
            if (phase_ < 0.0f) phase_ += 1.0f;

            // Update primary hypothesis phase with corrected value
            primary.phase = phase_;
        }
    } else {
        // No active primary hypothesis - fall back to legacy behavior
        // Advance phase based on current tempo estimate
        float phaseIncrement = dt * 1000.0f / beatPeriodMs_;
        phase_ += phaseIncrement;

        // Safety check: if phase becomes NaN or infinite, reset to 0
        if (!isfinite(phase_)) {
            phase_ = 0.0f;
        }

        // Wrap phase at 1.0 using fmodf
        phase_ = fmodf(phase_, 1.0f);
        if (phase_ < 0.0f) phase_ += 1.0f;

        // Decay periodicity during silence
        if (silenceMs > 3000) {
            // FIX: Clamp dt to prevent exp underflow on extreme frame drops
            float clampedDt = (dt > 1.0f) ? 1.0f : dt;
            float decayFactor = expf(-0.138629f * clampedDt);
            periodicityStrength_ *= decayFactor;
        }
    }

    // === BEAT DETECTION (phase wrap) ===
    // Detect when phase crosses from high (>0.8) to low (<0.2) = beat occurred
    if (prevPhase > 0.8f && phase_ < 0.2f && periodicityStrength_ > activationThreshold) {
        updateBeatStability(nowMs);
    }

    // === PREDICT NEXT BEAT ===
    predictNextBeat(nowMs);

    // === DEBUG OUTPUT ===
    multiHypothesis_.printDebug(nowMs);
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
    control_.phase = phase_;
}

void AudioController::synthesizeRhythmStrength() {
    // Single metric: autocorrelation periodicity strength
    float strength = periodicityStrength_;

    // Apply activation threshold with soft knee
    if (strength < activationThreshold) {
        strength *= strength / activationThreshold;  // Quadratic falloff below threshold
    }

    control_.rhythmStrength = clampf(strength, 0.0f, 1.0f);
}

// ============================================================================
// TempoHypothesis Implementation
// ============================================================================

void TempoHypothesis::updatePhase(float dt) {
    // FIX: Guard against division by zero
    if (beatPeriodMs < 1.0f) beatPeriodMs = 500.0f;  // Safe default: 120 BPM

    float phaseIncrement = dt * 1000.0f / beatPeriodMs;
    phase += phaseIncrement;

    // Accumulate fractional beats
    beatsSinceUpdate += phaseIncrement;

    // FIX: Check for NaN/infinity before further processing
    if (!isfinite(phase)) {
        phase = 0.0f;
        return;
    }

    // Detect phase wraparound to increment beat count
    // This happens when phase crosses from <1.0 to >=1.0
    if (phase >= 1.0f && beatCount < 65535) {
        // Count how many complete beats occurred (usually 1, but could be >1 if dt is large)
        uint16_t beatsElapsed = static_cast<uint16_t>(phase);  // Integer part
        beatCount += beatsElapsed;
        if (beatCount > 65535) beatCount = 65535;  // Clamp to max
    }

    // Wrap phase to [0, 1)
    phase = fmodf(phase, 1.0f);
    if (phase < 0.0f) phase += 1.0f;
}

void TempoHypothesis::applyBeatDecay(float minStrengthToKeep) {
    if (beatsSinceUpdate < 0.01f) return;  // No decay needed yet

    // Phrase-aware decay: half-life = 32 beats
    // decay_factor = exp(-ln(2) * beats / 32) = exp(-0.02166 * beats)
    float decayFactor = expf(-0.02166f * beatsSinceUpdate);
    strength *= decayFactor;

    // Reset accumulator
    beatsSinceUpdate = 0.0f;

    // Deactivate if too weak
    if (strength < minStrengthToKeep) {
        active = false;
    }
}

void TempoHypothesis::applySilenceDecay(float dt, float minStrengthToKeep) {
    // FIX: Clamp dt to reasonable range (prevent underflow on large dt, skip on dt<=0)
    if (dt <= 0.0f) return;
    if (dt > 1.0f) dt = 1.0f;  // Cap at 1 second per frame (prevents exp underflow)

    // Time-based decay: 5-second half-life
    // decay_factor = exp(-ln(2) * dt / 5.0) = exp(-0.138629 * dt)
    float decayFactor = expf(-0.138629f * dt);
    strength *= decayFactor;

    // Deactivate if too weak
    if (strength < minStrengthToKeep) {
        active = false;
    }
}

float TempoHypothesis::computeConfidence(float strengthWeight, float consistencyWeight, float longevityWeight) const {
    // Normalize beat count to [0, 1] over 32 beats (8 bars)
    float normalizedLongevity = (beatCount > 32) ? 1.0f : static_cast<float>(beatCount) / 32.0f;

    // Phase consistency (1.0 = perfect, 0.0 = terrible)
    // avgPhaseError is in range [0, 1], where 0 = perfect
    float phaseConsistency = 1.0f - avgPhaseError;
    if (phaseConsistency < 0.0f) phaseConsistency = 0.0f;
    if (phaseConsistency > 1.0f) phaseConsistency = 1.0f;

    // Weighted combination
    float result = strengthWeight * strength +
                   consistencyWeight * phaseConsistency +
                   longevityWeight * normalizedLongevity;

    // Clamp to [0, 1] to maintain confidence contract
    if (result < 0.0f) result = 0.0f;
    if (result > 1.0f) result = 1.0f;
    return result;
}

// ============================================================================
// MultiHypothesisTracker Implementation
// ============================================================================

int MultiHypothesisTracker::createHypothesis(float bpm, float strength, uint32_t nowMs, int lagSamples, float correlation) {
    int slot = findBestSlot();
    if (slot < 0) return -1;  // No slot available (shouldn't happen)

    // Store old hypothesis info for debug
    bool wasActive = hypotheses[slot].active;
    float oldBpm = hypotheses[slot].bpm;

    TempoHypothesis& hypo = hypotheses[slot];
    hypo.bpm = bpm;
    hypo.beatPeriodMs = 60000.0f / bpm;
    hypo.phase = 0.0f;
    hypo.strength = strength;
    hypo.confidence = strength;  // Initial confidence = strength
    hypo.avgPhaseError = 0.5f;  // Neutral value - prevents over-confidence in new hypotheses
    hypo.lastUpdateMs = nowMs;
    hypo.createdMs = nowMs;
    hypo.beatCount = 0;
    hypo.beatsSinceUpdate = 0.0f;
    hypo.correlationPeak = correlation;
    hypo.lagSamples = lagSamples;
    hypo.active = true;
    hypo.priority = static_cast<uint8_t>(slot);

    // Debug output for creation/eviction
    // Requires both: debug channel enabled AND verbosity level >= EVENTS
    // Use: "debug hypothesis on" to enable, "set hypodebug 1" for verbosity
    if (debugLevel >= HypothesisDebugLevel::EVENTS &&
        SerialConsole::isDebugChannelEnabled(DebugChannel::HYPOTHESIS)) {
        if (wasActive) {
            Serial.print(F("{\"type\":\"HYPO_EVICT\",\"slot\":"));
            Serial.print(slot);
            Serial.print(F(",\"oldBpm\":"));
            Serial.print(oldBpm, 1);
            Serial.print(F(",\"newBpm\":"));
            Serial.print(bpm, 1);
            Serial.println(F("}"));
        }
        Serial.print(F("{\"type\":\"HYPO_CREATE\",\"slot\":"));
        Serial.print(slot);
        Serial.print(F(",\"bpm\":"));
        Serial.print(bpm, 1);
        Serial.print(F(",\"strength\":"));
        Serial.print(strength, 2);
        Serial.println(F("}"));
    }

    return slot;
}

int MultiHypothesisTracker::findBestSlot() {
    // First, check for inactive slots
    for (int i = 0; i < MAX_HYPOTHESES; i++) {
        if (!hypotheses[i].active) {
            return i;
        }
    }

    // All slots active - find LRU (least recently updated)
    int oldestSlot = 0;
    uint32_t oldestTime = hypotheses[0].lastUpdateMs;

    for (int i = 1; i < MAX_HYPOTHESES; i++) {
        if (hypotheses[i].lastUpdateMs < oldestTime) {
            oldestTime = hypotheses[i].lastUpdateMs;
            oldestSlot = i;
        }
    }

    // Don't evict primary if it's still strong (>0.5 strength)
    if (oldestSlot == 0 && hypotheses[0].strength > 0.5f) {
        // Find LRU among non-primary slots (1, 2, 3) instead
        oldestSlot = 1;
        oldestTime = hypotheses[1].lastUpdateMs;
        for (int i = 2; i < MAX_HYPOTHESES; i++) {
            if (hypotheses[i].lastUpdateMs < oldestTime) {
                oldestTime = hypotheses[i].lastUpdateMs;
                oldestSlot = i;
            }
        }
    }

    return oldestSlot;
}

int MultiHypothesisTracker::findMatchingHypothesis(float bpm) const {
    for (int i = 0; i < MAX_HYPOTHESES; i++) {
        if (!hypotheses[i].active) continue;

        // Guard against division by zero (hypotheses[i].bpm should never be 0, but be defensive)
        if (hypotheses[i].bpm < 1.0f) continue;

        float error = absf(bpm - hypotheses[i].bpm) / hypotheses[i].bpm;
        if (error < bpmMatchTolerance) {
            return i;
        }
    }
    return -1;  // No match
}

void MultiHypothesisTracker::updateHypothesis(int index, float dt, uint32_t nowMs, bool hasSignificantAudio) {
    if (index < 0 || index >= MAX_HYPOTHESES) return;
    if (!hypotheses[index].active) return;

    TempoHypothesis& hypo = hypotheses[index];

    // Always advance phase (for prediction)
    hypo.updatePhase(dt);

    // Update confidence
    hypo.confidence = hypo.computeConfidence(strengthWeight, consistencyWeight, longevityWeight);

    if (hasSignificantAudio) {
        // Active music: apply beat-count decay if enough beats accumulated
        if (hypo.beatsSinceUpdate > 1.0f) {
            hypo.applyBeatDecay(minStrengthToKeep);
            // FIX: Reset accumulator after decay to prevent runaway accumulation
            hypo.beatsSinceUpdate = 0.0f;
        }
    } else {
        // Silence: apply time-based decay after grace period
        int32_t silenceMs = static_cast<int32_t>(nowMs - hypo.lastUpdateMs);
        if (silenceMs < 0) silenceMs = 0;  // Handle wraparound

        if (static_cast<uint32_t>(silenceMs) > silenceGracePeriodMs) {
            hypo.applySilenceDecay(dt, minStrengthToKeep);
        }
    }
}

void MultiHypothesisTracker::promoteBestHypothesis(uint32_t nowMs) {
    // Find best non-primary hypothesis
    int bestSlot = 0;
    float bestConfidence = hypotheses[0].confidence;

    for (int i = 1; i < MAX_HYPOTHESES; i++) {
        if (!hypotheses[i].active) continue;

        if (hypotheses[i].confidence > bestConfidence) {
            bestConfidence = hypotheses[i].confidence;
            bestSlot = i;
        }
    }

    // Promote if significantly better and has enough history
    if (bestSlot != 0 &&
        bestConfidence > hypotheses[0].confidence + promotionThreshold &&
        hypotheses[bestSlot].beatCount >= minBeatsBeforePromotion) {

        // Debug output (requires channel enabled AND verbosity level)
        if (debugLevel >= HypothesisDebugLevel::EVENTS &&
            SerialConsole::isDebugChannelEnabled(DebugChannel::HYPOTHESIS)) {
            Serial.print(F("{\"type\":\"HYPO_PROMOTE\",\"from\":"));
            Serial.print(bestSlot);
            Serial.print(F(",\"to\":0,\"bpm\":"));
            Serial.print(hypotheses[bestSlot].bpm, 1);
            Serial.print(F(",\"conf\":"));
            Serial.print(bestConfidence, 2);
            Serial.println(F("}"));
        }

        // Swap slots (not just pointers - actually swap data)
        // NOTE: createdMs, lastUpdateMs, and beatCount retain their original values
        // This preserves the hypothesis's full history (when it was born, not when promoted)
        TempoHypothesis temp = hypotheses[0];
        hypotheses[0] = hypotheses[bestSlot];
        hypotheses[bestSlot] = temp;

        // Update priority values to reflect new slot positions
        hypotheses[0].priority = 0;
        hypotheses[bestSlot].priority = static_cast<uint8_t>(bestSlot);
    }
}

void MultiHypothesisTracker::printDebug(uint32_t nowMs, const char* eventType, int slotIndex) const {
    // Requires both: debug channel enabled AND verbosity level > OFF
    if (debugLevel == HypothesisDebugLevel::OFF || !SerialConsole::isDebugChannelEnabled(DebugChannel::HYPOTHESIS)) return;

    // EVENT-level messages are printed immediately by the calling functions
    // (createHypothesis, promoteBestHypothesis)

    // SUMMARY and DETAILED are rate-limited
    if (debugLevel >= HypothesisDebugLevel::SUMMARY) {
        // Rate limit to every 2 seconds
        if (nowMs - lastDebugPrintMs_ < 2000) return;
        const_cast<MultiHypothesisTracker*>(this)->lastDebugPrintMs_ = nowMs;

        if (debugLevel == HypothesisDebugLevel::SUMMARY) {
            // Print primary hypothesis only
            const TempoHypothesis& primary = hypotheses[0];
            if (primary.active) {
                Serial.print(F("{\"type\":\"HYPO_PRIMARY\",\"bpm\":"));
                Serial.print(primary.bpm, 1);
                Serial.print(F(",\"phase\":"));
                Serial.print(primary.phase, 2);
                Serial.print(F(",\"strength\":"));
                Serial.print(primary.strength, 2);
                Serial.print(F(",\"conf\":"));
                Serial.print(primary.confidence, 2);
                Serial.print(F(",\"beats\":"));
                Serial.print(primary.beatCount);
                Serial.println(F("}"));
            }
        } else if (debugLevel == HypothesisDebugLevel::DETAILED) {
            // Print all hypotheses
            Serial.print(F("{\"type\":\"HYPO_ALL\",\"h\":["));
            for (int i = 0; i < MAX_HYPOTHESES; i++) {
                if (i > 0) Serial.print(F(","));
                if (hypotheses[i].active) {
                    Serial.print(F("{\"s\":"));
                    Serial.print(i);
                    Serial.print(F(",\"bpm\":"));
                    Serial.print(hypotheses[i].bpm, 1);
                    Serial.print(F(",\"ph\":"));
                    Serial.print(hypotheses[i].phase, 2);
                    Serial.print(F(",\"str\":"));
                    Serial.print(hypotheses[i].strength, 2);
                    Serial.print(F(",\"conf\":"));
                    Serial.print(hypotheses[i].confidence, 2);
                    Serial.print(F(",\"b\":"));
                    Serial.print(hypotheses[i].beatCount);
                    Serial.print(F("}"));
                } else {
                    Serial.print(F("{\"s\":"));
                    Serial.print(i);
                    Serial.print(F(",\"inactive\":true}"));
                }
            }
            Serial.println(F("]}"));
        }
    }
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
    // Only update when we detect a beat (phase crossing 0)
    // Called from updatePhase when phase wraps around

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
    float compressed = logf(1.0f + flux * 10.0f) / logf(11.0f);

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

    // Need at least 2 seconds of data for meaningful autocorrelation
    if (bandOssCount_ < 120) {
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

    // Compute magnitude for confidence
    float magnitude = sqrtf(realSum * realSum + imagSum * imagSum);

    // Normalize by OSS energy to get confidence (0-1)
    // High magnitude relative to energy = strong periodicity at this tempo
    float normalizer = sqrtf(ossEnergy * static_cast<float>(samplesToUse));
    if (normalizer > 0.0001f) {
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
// Comb Filter Phase Tracker Implementation (Phase 4)
// ============================================================================

void CombFilterPhaseTracker::reset() {
    // Clear delay line
    for (int i = 0; i < MAX_LAG; i++) {
        delayLine_[i] = 0.0f;
    }
    writeIdx_ = 0;

    // Reset state
    currentLag_ = 30;  // Default ~120 BPM at 60 Hz
    resonatorOutput_ = 0.0f;
    prevResonatorOutput_ = 0.0f;
    phase_ = 0.0f;
    samplesSincePeak_ = 0;
    confidence_ = 0.0f;
    peakAmplitude_ = 0.0f;
    runningMax_ = 0.0f;
    runningMean_ = 0.0f;
    sampleCount_ = 0;
}

void CombFilterPhaseTracker::setTempo(float bpm, float frameRate) {
    // Convert BPM to lag in samples
    // beatPeriodSeconds = 60 / bpm
    // lagSamples = beatPeriodSeconds * frameRate
    if (bpm < 30.0f) bpm = 30.0f;
    if (bpm > 300.0f) bpm = 300.0f;

    int newLag = static_cast<int>((60.0f / bpm) * frameRate + 0.5f);

    // Clamp to valid range
    if (newLag < 1) newLag = 1;
    if (newLag > MAX_LAG) newLag = MAX_LAG;

    // Only update if significantly different (avoid jitter)
    if (newLag != currentLag_) {
        // Check if change is significant (>5%)
        float ratio = static_cast<float>(newLag) / static_cast<float>(currentLag_);
        if (ratio < 0.95f || ratio > 1.05f) {
            currentLag_ = newLag;
        }
    }
}

void CombFilterPhaseTracker::process(float input) {
    // Read from delay line at current lag
    int readIdx = (writeIdx_ - currentLag_ + MAX_LAG) % MAX_LAG;
    float delayed = delayLine_[readIdx];

    // Comb filter resonator: y[n] = x[n] + feedback * y[n-L]
    // This accumulates energy at the beat period
    resonatorOutput_ = input + feedbackGain * delayed;

    // Write to delay line
    delayLine_[writeIdx_] = resonatorOutput_;
    writeIdx_ = (writeIdx_ + 1) % MAX_LAG;

    // Update running statistics for confidence calculation
    sampleCount_++;
    float absOutput = resonatorOutput_ > 0 ? resonatorOutput_ : -resonatorOutput_;

    // Exponential moving average for mean
    const float alpha = 0.01f;  // Slow adaptation
    runningMean_ = (1.0f - alpha) * runningMean_ + alpha * absOutput;

    // Track maximum for normalization
    if (absOutput > runningMax_) {
        runningMax_ = absOutput;
    } else {
        // Slow decay of max
        runningMax_ *= 0.999f;
    }

    // Phase tracking: detect peaks in resonator output
    // A peak indicates a beat position
    samplesSincePeak_++;

    // Cap samplesSincePeak to prevent overflow and handle missed peaks
    // If we've gone 4 beat periods without a peak, something is wrong
    if (samplesSincePeak_ > currentLag_ * 4) {
        samplesSincePeak_ = currentLag_ * 4;
    }

    // Detect positive peak (local maximum)
    bool peakDetected = false;
    if (prevResonatorOutput_ > 0.0f &&
        resonatorOutput_ < prevResonatorOutput_ &&
        prevResonatorOutput_ > runningMean_ * 1.5f) {
        // Found a peak - update phase estimate
        peakDetected = true;

        // When a peak is detected, we know we're at phase ~0 (on the beat)
        // The phase error is how far our predicted phase was from 0
        // Use this to correct our phase tracking

        // Current predicted phase should be near 0 or 1 at a peak
        float phaseError = phase_;
        if (phaseError > 0.5f) {
            phaseError = phaseError - 1.0f;  // Convert 0.9 to -0.1
        }

        // Apply correction with smoothing
        // Negative error means we're ahead, positive means behind
        phase_ -= phaseError * 0.3f;

        // Normalize phase to [0, 1)
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        if (phase_ < 0.0f) phase_ += 1.0f;

        // Update peak amplitude for confidence
        peakAmplitude_ = 0.8f * peakAmplitude_ + 0.2f * prevResonatorOutput_;

        // Reset counter
        samplesSincePeak_ = 0;
    }

    // Update phase (it advances with time)
    // Phase advances by 1/currentLag per sample
    // Skip the increment on peak detection frames to avoid double-update
    if (!peakDetected) {
        phase_ += 1.0f / static_cast<float>(currentLag_);
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }

    // Compute confidence based on resonator stability
    // High confidence when:
    // 1. Peak amplitude is significantly above mean
    // 2. Peaks occur regularly at expected interval
    if (runningMax_ > 0.001f && peakAmplitude_ > 0.0f) {
        // Peak-to-mean ratio indicates periodicity strength
        float peakRatio = peakAmplitude_ / (runningMean_ + 0.001f);
        // Confidence scales with how much peaks stand out
        confidence_ = 1.0f - 1.0f / (1.0f + peakRatio * 0.5f);

        // Clamp to [0, 1]
        if (confidence_ < 0.0f) confidence_ = 0.0f;
        if (confidence_ > 1.0f) confidence_ = 1.0f;

        // Reduce confidence during startup
        if (sampleCount_ < currentLag_ * 4) {
            confidence_ *= static_cast<float>(sampleCount_) / static_cast<float>(currentLag_ * 4);
        }
    } else {
        confidence_ = 0.0f;
    }

    prevResonatorOutput_ = resonatorOutput_;
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

    for (int i = 0; i < lag; i++) {
        int idx = (historyIdx_ - 1 - i + MAX_LAG) % MAX_LAG;
        float t = static_cast<float>(i);
        float angle = -COMB_TWO_PI * omega * t;

        // Use fast approximations for sin/cos if available
        float c = cosf(angle);
        float s = sinf(angle);

        realSum += resonatorHistory_[idx] * c;
        imagSum += resonatorHistory_[idx] * s;
    }

    // Compute phase from complex sum
    float phase = -atan2f(imagSum, realSum) / COMB_TWO_PI;

    // Normalize to [0, 1)
    if (phase < 0.0f) phase += 1.0f;
    if (phase >= 1.0f) phase -= 1.0f;

    peakPhase_ = phase;
}

// ============================================================================
// Rhythm Fusion Implementation (Phase 5)
// ============================================================================

void AudioController::fuseRhythmEstimates() {
    // Collect phase estimates and confidences from available systems
    // System 0: Autocorrelation (Fourier phase from OSS buffer)
    // System 1: Single comb filter (if combFilterWeight > 0)
    // System 2: Comb filter bank (if combBankEnabled)

    // Count active systems and collect their estimates
    static constexpr int MAX_SYSTEMS = 3;
    float phases[MAX_SYSTEMS] = {0};
    float confidences[MAX_SYSTEMS] = {0};
    int numActive = 0;

    // System 0: Autocorrelation phase (always active)
    // Uses blended Fourier/peak-finding phase from runAutocorrelation
    phases[numActive] = targetPhase_;
    confidences[numActive] = periodicityStrength_;
    numActive++;

    // System 1: Single comb filter (if enabled and has confidence)
    if (combFilterWeight > 0.0f && combFilter_.getConfidence() > 0.1f) {
        phases[numActive] = combFilter_.getPhase();
        // Scale confidence by combFilterWeight
        confidences[numActive] = combFilter_.getConfidence() * combFilterWeight;
        numActive++;
    }

    // System 2: Comb filter bank (if enabled and has confidence)
    if (combBankEnabled && combFilterBank_.getPeakConfidence() > 0.1f) {
        phases[numActive] = combFilterBank_.getPhaseAtPeak();
        // Use bank confidence directly (already independent validation)
        confidences[numActive] = combFilterBank_.getPeakConfidence();
        numActive++;
    }

    // If only one system active, use it directly
    // Note: consensusMetric_ reflects actual confidence, not fake "perfect consensus"
    if (numActive <= 1) {
        fusedPhase_ = phases[0];
        fusedConfidence_ = confidences[0];
        consensusMetric_ = confidences[0];  // Single system = use its confidence
        return;
    }

    // Normalize confidences
    float totalConf = 0.0f;
    for (int i = 0; i < numActive; i++) {
        totalConf += confidences[i];
    }

    if (totalConf < 0.01f) {
        // No confident estimate - keep current phase
        fusedConfidence_ = 0.0f;
        consensusMetric_ = 0.0f;
        return;
    }

    // Weighted circular mean for phase fusion
    // Convert to unit vectors, weight, sum, convert back
    float sinSum = 0.0f;
    float cosSum = 0.0f;

    for (int i = 0; i < numActive; i++) {
        float weight = confidences[i] / totalConf;
        float angle = phases[i] * 2.0f * 3.14159265f;
        sinSum += weight * sinf(angle);
        cosSum += weight * cosf(angle);
    }

    // Convert back to phase
    fusedPhase_ = atan2f(sinSum, cosSum) / (2.0f * 3.14159265f);
    if (fusedPhase_ < 0.0f) fusedPhase_ += 1.0f;

    // Apply transient hint (small nudge based on detected transients)
    // transientPhaseError_ is updated in updateTransientPhaseCorrection
    if (transientHintWeight > 0.0f) {
        float hint = transientPhaseError_ * transientHintWeight;
        // Clamp hint to prevent large jumps
        if (hint > 0.1f) hint = 0.1f;
        if (hint < -0.1f) hint = -0.1f;

        fusedPhase_ += hint;
        // Normalize
        if (fusedPhase_ >= 1.0f) fusedPhase_ -= 1.0f;
        if (fusedPhase_ < 0.0f) fusedPhase_ += 1.0f;

        transientHint_ = hint;  // Store for debugging
    }

    // Compute consensus metric (how much do systems agree?)
    // Based on maximum phase difference between any two systems
    float maxDiff = 0.0f;
    for (int i = 0; i < numActive; i++) {
        for (int j = i + 1; j < numActive; j++) {
            float diff = phases[i] - phases[j];
            // Handle wraparound
            if (diff < 0.0f) diff = -diff;
            if (diff > 0.5f) diff = 1.0f - diff;
            if (diff > maxDiff) maxDiff = diff;
        }
    }

    // Consensus: 1.0 when all agree, 0.0 when maximally different (0.5 apart)
    consensusMetric_ = 1.0f - (maxDiff * 2.0f);
    if (consensusMetric_ < 0.0f) consensusMetric_ = 0.0f;

    // Fused confidence = average confidence × consensus
    // High confidence only when systems agree
    fusedConfidence_ = (totalConf / static_cast<float>(numActive)) * consensusMetric_;

    // Update targetPhase_ with fused result
    // This allows downstream systems to use the improved estimate
    targetPhase_ = fusedPhase_;
}
