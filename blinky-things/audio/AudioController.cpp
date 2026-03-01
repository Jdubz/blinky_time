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

    // Reset IOI onset buffer
    for (int i = 0; i < IOI_ONSET_BUFFER_SIZE; i++) ioiOnsetSamples_[i] = 0;
    ioiOnsetWriteIdx_ = 0;
    ioiOnsetCount_ = 0;

    // Initialize and reset comb filter bank
    // Uses 60 Hz frame rate assumption (same as OSS buffer)
    combFilterBank_.init(60.0f);

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
    prevOdfForDiff_ = 0.0f;
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
    hmmInitialized_ = false;  // HMM will re-init on first use
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
    //    BTrack uses a single ODF for both transient detection and beat tracking.
    //    Phase 2.4: When unifiedOdf is on, use BandFlux pre-threshold value (already computed
    //    in step 3) instead of the separate computeSpectralFluxBands(). This ensures the beat
    //    tracker and transient detector "hear" the same signal.
    float onsetStrength = 0.0f;

    if (unifiedOdf) {
        // Phase 2.4: Use BandFlux continuous pre-threshold activation
        // This is the combined weighted flux BEFORE thresholding/cooldown/peak-picking
        // Log-compressed, band-weighted, vibrato-suppressed — same signal driving transients
        // Guard: if BandFlux didn't run this frame (no spectral data), combinedFlux_ is stale.
        // Fall back to mic level, matching the legacy path behavior.
        const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();
        if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
            onsetStrength = ensemble_.getBandFlux().getPreThresholdFlux();

            // Feed adaptive band weighting from BandFlux per-band values
            if (adaptiveBandWeightEnabled) {
                addBandOssSamples(
                    ensemble_.getBandFlux().getBassFlux(),
                    ensemble_.getBandFlux().getMidFlux(),
                    ensemble_.getBandFlux().getHighFlux()
                );
            }
        } else {
            onsetStrength = mic_.getLevel();
        }
    } else {
        // Legacy path: independent spectral flux computation for CBSS
        const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();

        if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
            const float* magnitudes = spectral.getMagnitudes();
            int numBins = spectral.getNumBins();

            // Compute spectral flux with per-band outputs for adaptive weighting
            float bassFlux = 0.0f, midFlux = 0.0f, highFlux = 0.0f;
            onsetStrength = computeSpectralFluxBands(magnitudes, numBins, bassFlux, midFlux, highFlux);

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
    // When onsetTrainOdf is on, feed binary onset events (1.0 on transient, 0.0 otherwise)
    // to the OSS buffer instead of continuous ODF. This makes the ACF track
    // inter-onset periodicity (immune to enclosure resonance artifacts in continuous flux).
    float ossValue;
    if (odfSource == 1) {
        // Bass energy: sum of whitened bass magnitudes (bins 1-6, 62.5-375 Hz).
        // Tracks kick drum PRESENCE (absolute magnitude) rather than spectral CHANGES (flux).
        const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();
        if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
            const float* mags = spectral.getMagnitudes();
            float bassEnergy = 0.0f;
            for (int i = 1; i <= 6; i++) {
                bassEnergy += mags[i];
            }
            ossValue = bassEnergy;
        } else {
            ossValue = 0.0f;
        }
    } else if (odfSource == 2) {
        // Mic level: broadband time-domain RMS from AdaptiveMic.
        // Not affected by frequency-specific enclosure resonances.
        ossValue = hasSignificantAudio ? mic_.getLevel() : 0.0f;
    } else if (odfSource == 3) {
        // Bass-only flux: just the bass band spectral flux from BandFlux.
        // Eliminates mid/high band contamination from enclosure resonance.
        const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();
        if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
            ossValue = ensemble_.getBandFlux().getBassFlux();
        } else {
            ossValue = 0.0f;
        }
    } else if (odfSource == 4) {
        // Spectral centroid: tracks spectral SHAPE (center of mass of spectrum).
        // Kick drum = centroid drops (bass-heavy), snare = centroid rises.
        // Robust to uniform energy modulation from enclosure resonance.
        // Normalize to 0-1 range (centroid is in Hz, typ 200-4000).
        const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();
        if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
            ossValue = spectral.getSpectralCentroid() / 4000.0f;
            if (ossValue > 1.0f) ossValue = 1.0f;
        } else {
            ossValue = 0.0f;
        }
    } else if (odfSource == 5) {
        // Bass ratio: bass energy / total energy. Tracks kick drum dominance.
        // When kick hits: bass ratio spikes (bass dominates). Between kicks: drops.
        // Immune to overall level modulation (ratio cancels out).
        const SharedSpectralAnalysis& spectral = ensemble_.getSpectral();
        if (spectral.isFrameReady() || spectral.hasPreviousFrame()) {
            const float* mags = spectral.getMagnitudes();
            float bassEnergy = 0.0f;
            for (int i = 1; i <= 6; i++) bassEnergy += mags[i];
            float totalEnergy = spectral.getTotalEnergy();
            ossValue = (totalEnergy > 0.001f) ? (bassEnergy / totalEnergy) : 0.0f;
        } else {
            ossValue = 0.0f;
        }
    } else if (onsetTrainOdf) {
        ossValue = (lastEnsembleOutput_.transientStrength > 0.0f) ? 1.0f : 0.0f;
    } else if (odfDiffMode) {
        // HWR first-difference: max(0, odf[n] - odf[n-1])
        // Emphasizes onset attacks (~30x larger than continuous modulation),
        // suppressing enclosure-induced periodic fluctuations.
        float diff = onsetStrength - prevOdfForDiff_;
        prevOdfForDiff_ = onsetStrength;
        ossValue = (diff > 0.0f) ? diff : 0.0f;
    } else {
        ossValue = hasSignificantAudio ? onsetStrength : 0.0f;
    }
    if (hasSignificantAudio || onsetTrainOdf) {
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

    // 7. Update beat tracking
    //    Hybrid mode (barPointerHmm=true): HMM determines tempo, CBSS detects beats.
    //    The HMM's structural tempo-phase coupling prevents octave lock (best BPM
    //    accuracy in testing). CBSS handles beat detection and phase (proven F1=0.627
    //    when given correct BPM). Without HMM: full Bayesian+CBSS pipeline as before.
    if (barPointerHmm && (hmmInitialized_ || tempoStateInitialized_)) {
        if (!hmmInitialized_) initHmmState();
        if (hmmInitialized_) {
            updateHmmForward(onsetStrength);
            // Feed HMM tempo to CBSS: override Bayesian tempo estimate
            bpm_ = tempoBinBpms_[hmmBestTempo_];
            beatPeriodMs_ = 60000.0f / bpm_;
            beatPeriodSamples_ = hmmPeriods_[hmmBestTempo_];
            bayesBestBin_ = hmmBestTempo_;
        }
    }
    // CBSS always runs for beat detection (uses HMM tempo when active)
    // Apply power-law contrast to sharpen beat/non-beat distinction in CBSS.
    // BTrack squares the ODF (contrast=2.0) before CBSS to make beat positions
    // dominate the cumulative signal, improving phase lock accuracy.
    float cbssInput = onsetStrength;
    if (cbssContrast != 1.0f && cbssInput > 0.0f) {
        cbssInput = powf(cbssInput, cbssContrast);
    }
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

void AudioController::setDetectorEnabled(DetectorType type, bool enabled) {
    ensemble_.setDetectorEnabled(type, enabled);
}

void AudioController::setDetectorWeight(DetectorType type, float weight) {
    ensemble_.setDetectorWeight(type, weight);
}

void AudioController::setDetectorThreshold(DetectorType type, float threshold) {
    ensemble_.setDetectorThreshold(type, threshold);
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

    // FIX: Clear the portion we'll use to prevent stale data (full harmonic range)
    for (int i = 0; i < harmonicCorrelationSize; i++) {
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
    periodicityStrength_ = periodicityStrength_ * 0.7f + newStrength * 0.3f;

    // Apply inverse-lag normalization to ACF (sub-harmonic penalty).
    // For a periodic signal, acf(2T) ≈ acf(T), so sub-harmonics score equally.
    // Dividing by lag penalizes longer periods linearly:
    //   acf(2T)/(2T) = 0.5 * acf(T)/T
    // giving the true period a 2x advantage over its first sub-harmonic.
    // Combined with Rayleigh prior (1.67x), total discrimination is ~3.3x.
    // This must happen AFTER periodicity strength (which uses raw ACF magnitude)
    // but BEFORE Bayesian fusion and harmonic disambiguation (which need
    // discriminative lag-weighted values).
    for (int i = 0; i < harmonicCorrelationSize; i++) {
        int lag = minLag + i;
        float lagF = static_cast<float>(lag);
        correlationAtLag[i] /= lagF;  // lag^1 (proven at F1=0.519)
    }

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
    // Gaussian transition matrix in BPM-space, sigma proportional to source BPM.
    // With 20 bins (lag-uniform grid), the non-uniform BPM spacing creates
    // mild drift toward low BPM, but it's slow enough that observations
    // overcome it. (40 bins had 2x worse drift — reverted to 20.)

    for (int i = 0; i < TEMPO_BINS; i++) {
        for (int j = 0; j < TEMPO_BINS; j++) {
            float bpmDiff = tempoBinBpms_[i] - tempoBinBpms_[j];
            float sigma = bayesLambda * tempoBinBpms_[j];
            if (sigma < 1.0f) sigma = 1.0f;
            float narrow = expf(-0.5f * (bpmDiff * bpmDiff) / (sigma * sigma));

            float harmonicBonus = 0.0f;

            // Harmonic shortcuts only for multiplicative path.
            // BTrack pipeline relies on comb-on-ACF for octave handling;
            // harmonic shortcuts in the transition matrix are redundant.
            if (!btrkPipeline) {
                float htw = harmonicTransWeight;
                float ratio = tempoBinBpms_[i] / tempoBinBpms_[j];

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
                // 3:2 (e.g., 90→135)
                float diff32 = fabsf(ratio - 1.5f);
                if (diff32 < 0.1f) {
                    float w = htw * 0.5f * expf(-diff32 * diff32 * 200.0f);
                    if (w > harmonicBonus) harmonicBonus = w;
                }
                // 2:3 (e.g., 135→90)
                float diff23 = fabsf(ratio - 0.6667f);
                if (diff23 < 0.1f) {
                    float w = htw * 0.5f * expf(-diff23 * diff23 * 200.0f);
                    if (w > harmonicBonus) harmonicBonus = w;
                }
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
        // At 60 Hz: lag = 3600 / bpm
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
    transMatrixHarmonic_ = btrkPipeline ? -2.0f : harmonicTransWeight;

    // Rayleigh prior peaked at ~120 BPM (BTrack-style perceptual weighting)
    // For candidate period T (lag), Rayleigh(T; sigma) = T/sigma^2 * exp(-T^2 / (2*sigma^2))
    // Peaked at sigma = lag corresponding to 120 BPM = 3600/120 = 30 at 60 Hz
    {
        float rayleighSigma = OSS_FRAMES_PER_MIN / 120.0f;  // = 30 (lag for 120 BPM)
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
    }

    // Clear posterior and debug arrays
    for (int i = 0; i < TEMPO_BINS; i++) {
        tempoStatePost_[i] = tempoStatePrior_[i];
        lastFtObs_[i] = 0.0f;
        lastCombObs_[i] = 0.0f;
        lastIoiObs_[i] = 0.0f;
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
float AudioController::getBayesFtObs() const {
    if (bayesBestBin_ >= 0 && bayesBestBin_ < TEMPO_BINS)
        return lastFtObs_[bayesBestBin_];
    return 0.0f;
}
float AudioController::getBayesCombObs() const {
    if (bayesBestBin_ >= 0 && bayesBestBin_ < TEMPO_BINS)
        return lastCombObs_[bayesBestBin_];
    return 0.0f;
}
float AudioController::getBayesIoiObs() const {
    if (bayesBestBin_ >= 0 && bayesBestBin_ < TEMPO_BINS)
        return lastIoiObs_[bayesBestBin_];
    return 0.0f;
}

void AudioController::runBayesianTempoFusion(float* correlationAtLag, int correlationSize,
                                              int minLag, int maxLag, float avgEnergy,
                                              float samplesPerMs, bool debugPrint,
                                              int harmonicCorrelationSize) {
    if (!tempoStateInitialized_) return;

    // === 1. PREDICTION STEP ===
    // Rebuild transition matrix if parameters changed.
    // btrkPipeline disables harmonic shortcuts — use sentinel (-2) so toggling
    // btrkpipeline at runtime triggers a rebuild.
    float effectiveHarmonic = btrkPipeline ? -2.0f : harmonicTransWeight;
    if (bayesLambda != transMatrixLambda_ || effectiveHarmonic != transMatrixHarmonic_) {
        buildTransitionMatrix();
        transMatrixLambda_ = bayesLambda;
        transMatrixHarmonic_ = effectiveHarmonic;
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

    // === 2. AUTOCORRELATION OBSERVATION (BTrack-style 4-harmonic comb) ===
    // For each candidate period T, sum ACF at 1T, 2T, 3T, 4T with spread windows.
    // The true period matches all 4 harmonics; a sub-harmonic at 2T only matches 2.
    // This gives the fundamental a ~4x advantage over sub-harmonics.
    // Multiplied by Rayleigh prior peaked at ~120 BPM for perceptual weighting.
    float acfObs[TEMPO_BINS];
    for (int i = 0; i < TEMPO_BINS; i++) {
        int lag = tempoBinLags_[i];
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
        if (harmonicsUsed > 0) {
            acfObs[i] = combAcf * rayleighWeight_[i] / (avgEnergy + 0.001f);
            if (acfObs[i] < 0.01f) acfObs[i] = 0.01f;
        } else {
            acfObs[i] = 0.01f;  // Out of range — small floor
        }
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

    // === 3. FOURIER TEMPOGRAM OBSERVATION ===
    float ftObs[TEMPO_BINS];
    if (!btrkPipeline && ftEnabled && ossCount_ >= 60) {
        computeFTObservations(ftObs, TEMPO_BINS);
        if (bayesFtWeight != 1.0f) {
            for (int i = 0; i < TEMPO_BINS; i++) {
                ftObs[i] = powf(ftObs[i], bayesFtWeight);
            }
        }
    } else {
        for (int i = 0; i < TEMPO_BINS; i++) ftObs[i] = 1.0f;  // Uniform (no info)
    }
    // Save for debug
    for (int i = 0; i < TEMPO_BINS; i++) lastFtObs_[i] = ftObs[i];

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

    // === 5. IOI HISTOGRAM OBSERVATION ===
    float ioiObs[TEMPO_BINS];
    if (!btrkPipeline && ioiEnabled && ioiOnsetCount_ >= 8) {
        computeIOIObservations(ioiObs, TEMPO_BINS);
        if (bayesIoiWeight != 1.0f) {
            for (int i = 0; i < TEMPO_BINS; i++) {
                ioiObs[i] = powf(ioiObs[i], bayesIoiWeight);
            }
        }
    } else {
        for (int i = 0; i < TEMPO_BINS; i++) ioiObs[i] = 1.0f;
    }
    for (int i = 0; i < TEMPO_BINS; i++) lastIoiObs_[i] = ioiObs[i];

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
            tempoStatePost_[i] = prediction[i] * weightedPrior[i] * acfObs[i] * ftObs[i] * combObs[i] * ioiObs[i];
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
        static constexpr float HARMONIC_2X_THRESH = 0.5f;   // Half-lag ACF ratio for 2x BPM correction
        static constexpr float HARMONIC_1_5X_THRESH = 0.6f; // 2/3-lag ACF ratio for 1.5x BPM correction
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
    bayesBestBin_ = bestBin;

    // Disambiguation feedback (only for multiplicative path)
    if (!btrkPipeline && bestBin != preCorrectionBin && disambigNudge > 0.0f) {
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
        Serial.print(F(",\"ft\":"));
        Serial.print(ftObs[bestBin], 3);
        Serial.print(F(",\"cb\":"));
        Serial.print(combObs[bestBin], 3);
        Serial.print(F(",\"io\":"));
        Serial.print(ioiObs[bestBin], 3);
        Serial.print(F(",\"ms\":"));
        Serial.print(odfMeanSubEnabled ? 1 : 0);
        Serial.println(F("}"));
    }

    // === 9. UPDATE TEMPO ===
    // Skip when bar-pointer HMM is actually running — HMM sets bpm_/beatPeriodSamples_
    // at beat boundaries in detectBeatHmm(). Writing here would override HMM's tempo
    // between beats, causing jitter and wrong phase predictions. The Bayesian posterior
    // (step 10) still updates to keep transMatrix_ current for the HMM.
    // Use hmmInitialized_ (not barPointerHmm) so CBSS fallback still gets tempo updates.
    if (!hmmInitialized_ && periodicityStrength_ > 0.25f) {
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

        // Update ensemble detector with tempo hint for adaptive cooldown
        ensemble_.getFusion().setTempoHint(bpm_);

        // Update tempo velocity if BPM changed significantly
        if (fabsf(bpm_ - prevBpm_) / (prevBpm_ > 1.0f ? prevBpm_ : 1.0f) > tempoChangeThreshold) {
            float dt = autocorrPeriodMs / 1000.0f;
            updateTempoVelocity(bpm_, dt);
        }
    }

    // === 10. SAVE POSTERIOR AS NEXT PRIOR ===
    for (int i = 0; i < TEMPO_BINS; i++) {
        tempoStatePrior_[i] = tempoStatePost_[i];
    }
}

// ===== FOURIER TEMPOGRAM PER-BIN OBSERVATIONS =====

void AudioController::computeFTObservations(float* ftObs, int numBins) {
    // Compute Goertzel magnitude at each of the 40 tempo bin lags
    // Goertzel is O(N) per bin vs O(N log N) for full FFT — faster for sparse evaluation

    // Compute OSS mean for detrending (removes DC from DFT)
    float mean = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - ossCount_ + i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        mean += ossBuffer_[idx];
    }
    mean /= static_cast<float>(ossCount_);

    for (int b = 0; b < numBins; b++) {
        int lag = tempoBinLags_[b];
        if (lag < 5) { ftObs[b] = 0.01f; continue; }

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
        ftObs[b] = (magSq > 0.01f) ? magSq : 0.01f;  // Floor
    }

    // Normalize by mean magnitude across bins so that bayesFtWeight
    // behaves consistently regardless of signal amplitude.
    // Without this, powf(ftObs, weight) would give FT disproportionate
    // influence during loud sections (magSq scales with amplitude squared).
    float ftMean = 0.0f;
    for (int b = 0; b < numBins; b++) ftMean += ftObs[b];
    ftMean /= static_cast<float>(numBins);
    if (ftMean > 0.01f) {
        for (int b = 0; b < numBins; b++) ftObs[b] /= ftMean;
    }
}

// ===== IOI HISTOGRAM PER-BIN OBSERVATIONS =====

void AudioController::computeIOIObservations(float* ioiObs, int numBins) {
    // Accumulate IOI histogram counts at each of the 40 tempo bin lags
    // with ±2 sample tolerance (~33ms at 60Hz) for onset timing jitter

    // Initialize to 1.0 (multiplicative neutral). Unlike ACF which uses 0.01 floor
    // (representing actual correlation strength), IOI bins with zero onset matches
    // should not penalize the posterior — absence of evidence is not evidence of absence.
    for (int b = 0; b < numBins; b++) ioiObs[b] = 1.0f;

    int n = ioiOnsetCount_;

    for (int i = 0; i < n; i++) {
        int idxI = (ioiOnsetWriteIdx_ - 1 - i + IOI_ONSET_BUFFER_SIZE) % IOI_ONSET_BUFFER_SIZE;
        int sampleI = ioiOnsetSamples_[idxI];

        for (int j = i + 1; j < n; j++) {
            int idxJ = (ioiOnsetWriteIdx_ - 1 - j + IOI_ONSET_BUFFER_SIZE) % IOI_ONSET_BUFFER_SIZE;
            int sampleJ = ioiOnsetSamples_[idxJ];

            int interval = sampleI - sampleJ;
            if (interval <= 0) continue;

            // Early exit: interval too long for any bin.
            // Bin 0 has the longest lag (lowest BPM ~60) since CombFilterBank
            // orders filters from low to high BPM.
            if (interval > tempoBinLags_[0] * 3) break;

            // Check interval against each tempo bin lag (with ±2 sample tolerance)
            for (int b = 0; b < numBins; b++) {
                int lag = tempoBinLags_[b];
                // Direct match
                int diff = interval - lag;
                if (diff < 0) diff = -diff;
                if (diff <= 2) {
                    ioiObs[b] += 1.0f;
                }
                // Folded match (2x interval = skipped beat)
                if (interval >= lag * 2 - 2 && interval <= lag * 2 + 2) {
                    ioiObs[b] += 0.5f;  // Half weight for skipped-beat matches
                }
            }
        }
    }
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

    // During warmup (first N beats), use lower alpha so onsets contribute more
    // to the CBSS. This gives ~4x more onset weight during initial phase acquisition,
    // preventing random phase lock from weak early CBSS values.
    float effectiveAlpha = cbssAlpha;
    if (cbssWarmupBeats > 0 && beatCount_ < cbssWarmupBeats) {
        effectiveAlpha = cbssAlpha * 0.55f;  // e.g., 0.9 * 0.55 = 0.495 → onset gets ~50% weight
    }

    float cbssVal = (1.0f - effectiveAlpha) * onsetStrength + effectiveAlpha * maxWeightedCBSS;
    cbssBuffer_[sampleCounter_ % OSS_BUFFER_SIZE] = cbssVal;

    // Update running mean of CBSS for adaptive threshold
    // EMA alpha ≈ 1/120 ≈ 0.008 → tau ~120 frames (~2 seconds at 60 Hz)
    static constexpr float CBSS_MEAN_ALPHA = 0.008f;
    cbssMean_ = cbssMean_ * (1.0f - CBSS_MEAN_ALPHA) + cbssVal * CBSS_MEAN_ALPHA;

    sampleCounter_++;

    // Prevent signed integer overflow (UB in C++).
    // At 60 Hz, sampleCounter_ reaches 1M after ~4.6 hours.
    // Renormalize counters to keep values small while preserving their
    // differences (which is all the CBSS/IOI logic depends on).
    if (sampleCounter_ > 1000000) {
        BLINKY_ASSERT(false, "AudioController: sampleCounter_ renormalization triggered");
        int shift = sampleCounter_ - OSS_BUFFER_SIZE;
        sampleCounter_ -= shift;
        lastBeatSample_ -= shift;
        lastTransientSample_ -= shift;
        if (lastBeatSample_ < 0) lastBeatSample_ = 0;
        if (lastTransientSample_ < 0) lastTransientSample_ = -1;
        for (int i = 0; i < ioiOnsetCount_; i++) {
            ioiOnsetSamples_[i] -= shift;
            if (ioiOnsetSamples_[i] < 0) ioiOnsetSamples_[i] = 0;
        }
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
    // Look back over the last ~4 beats of history
    int lookback = T * 4;
    if (lookback > sampleCounter_) lookback = sampleCounter_;

    float scoreT = 0.0f;
    int countT = 0;

    // Score at current tempo T: sum CBSS at positions spaced T apart
    for (int offset = 0; offset < lookback; offset += T) {
        int idx = sampleCounter_ - 1 - offset;
        if (idx >= 0) {
            scoreT += cbssBuffer_[idx % OSS_BUFFER_SIZE];
            countT++;
        }
    }
    if (countT > 0) scoreT /= static_cast<float>(countT);

    // --- Check double-time (T/2 = faster) ---
    if (halfT >= 10) {
        float scoreHalfT = 0.0f;
        int countHalfT = 0;
        for (int offset = 0; offset < lookback; offset += halfT) {
            int idx = sampleCounter_ - 1 - offset;
            if (idx >= 0) {
                scoreHalfT += cbssBuffer_[idx % OSS_BUFFER_SIZE];
                countHalfT++;
            }
        }
        if (countHalfT > 0) scoreHalfT /= static_cast<float>(countHalfT);

        if (scoreT > 0.001f && scoreHalfT > octaveScoreRatio * scoreT) {
            switchTempo(halfT);
            return;
        }
    }

    // --- Check half-time (2T = slower) ---
    // Use longer lookback for slower tempo (need enough history at 2T spacing)
    int lookbackDouble = doubleT * 4;
    if (lookbackDouble > sampleCounter_) lookbackDouble = sampleCounter_;
    // Only check if 2T is within valid BPM range
    float doubleTPeriodBpm = OSS_FRAMES_PER_MIN / static_cast<float>(doubleT);
    if (doubleTPeriodBpm >= bpmMin && doubleT < OSS_BUFFER_SIZE / 2) {
        float scoreDoubleT = 0.0f;
        int countDoubleT = 0;
        for (int offset = 0; offset < lookbackDouble; offset += doubleT) {
            int idx = sampleCounter_ - 1 - offset;
            if (idx >= 0) {
                scoreDoubleT += cbssBuffer_[idx % OSS_BUFFER_SIZE];
                countDoubleT++;
            }
        }
        if (countDoubleT > 0) scoreDoubleT /= static_cast<float>(countDoubleT);

        if (scoreT > 0.001f && scoreDoubleT > octaveScoreRatio * scoreT) {
            switchTempo(doubleT);
            return;
        }
    }
}

void AudioController::checkPhaseAlignment() {
    // Phase alignment check: scan all phase offsets within one beat period
    // to find where raw onset strength (OSS) peaks. If the optimal phase is
    // far from the current beat phase, shift the beat anchor.
    //
    // Uses OSS (raw onset detection) not CBSS (which is self-reinforcing).
    // For each candidate phase offset φ ∈ [0, T), in steps of T/8:
    //   score(φ) = mean OSS at positions (now - φ), (now - φ - T), (now - φ - 2T), ...
    // The offset with the highest score is where onsets actually occur.
    int T = beatPeriodSamples_;
    if (T < 10 || ossCount_ < T * 3) return;

    // Number of beats to average over (limited by buffer)
    int maxBeats = (OSS_BUFFER_SIZE - T) / T;
    if (maxBeats > 6) maxBeats = 6;
    if (maxBeats < 2) return;

    // Scan phase offsets in steps of T/8 (8 candidates)
    static constexpr int NUM_PHASE_STEPS = 8;
    float bestScore = -1.0f;
    int bestOffset = 0;
    float currentScore = 0.0f;

    for (int step = 0; step < NUM_PHASE_STEPS; step++) {
        int phaseOffset = (step * T) / NUM_PHASE_STEPS;
        float score = 0.0f;
        int count = 0;

        for (int beatIdx = 0; beatIdx < maxBeats; beatIdx++) {
            int lookback = phaseOffset + beatIdx * T;
            if (lookback >= ossCount_ || lookback >= OSS_BUFFER_SIZE) break;

            // Map to circular buffer position
            int idx = sampleCounter_ - 1 - lookback;
            if (idx < 0) continue;
            idx = idx % OSS_BUFFER_SIZE;

            score += ossBuffer_[idx];
            count++;
        }

        if (count > 0) score /= static_cast<float>(count);

        if (step == 0) currentScore = score;  // Current beat phase = offset 0

        if (score > bestScore) {
            bestScore = score;
            bestOffset = phaseOffset;
        }
    }

    // If a different phase has significantly stronger onsets, shift
    if (bestOffset > 0 && currentScore > 0.001f &&
        bestScore > phaseCheckRatio * currentScore) {
        // Shift beat timing: next beat fires bestOffset frames SOONER
        // (because the optimal onset is bestOffset frames BEFORE the current beat position)
        timeToNextBeat_ = T - bestOffset;
        if (timeToNextBeat_ < 1) timeToNextBeat_ = 1;
        timeToNextPrediction_ = timeToNextBeat_ / 2;
    }
}

void AudioController::switchTempo(int newPeriodSamples) {
    beatPeriodSamples_ = newPeriodSamples;
    float newBpm = OSS_FRAMES_PER_MIN / static_cast<float>(newPeriodSamples);
    bpm_ = clampf(newBpm, bpmMin, bpmMax);
    beatPeriodMs_ = 60000.0f / bpm_;

    // Also update Bayesian posterior — nudge toward the new tempo bin
    int newBin = findClosestTempoBin(bpm_);
    if (newBin >= 0 && newBin < TEMPO_BINS) {
        // Transfer mass from old best bin to new
        float transfer = tempoStatePost_[bayesBestBin_] * 0.3f;
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

// ===== BAR-POINTER HMM BEAT TRACKING (Phase 3.1) =====
//
// Joint tempo-phase tracking via bar-pointer model (Whiteley/Bock 2016).
// State = (tempo_bin, position) where position counts up from 0 to period-1.
// Position 0 = beat boundary. At each frame, non-beat positions advance
// deterministically (p-1 → p). When position period-1 wraps back to 0,
// a tempo transition via transMatrix_[20][20] selects the new tempo bin.
// Uses Viterbi (max-product) at beat boundaries, consistent with btrkPipeline.

void AudioController::initHmmState() {
    if (!tempoStateInitialized_) return;  // Need tempo bins from Bayesian init

    // Build offset table: each tempo bin contributes period[t] states
    totalHmmStates_ = 0;
    for (int t = 0; t < TEMPO_BINS; t++) {
        hmmPeriods_[t] = tempoBinLags_[t];
        // Clamp to reasonable range
        if (hmmPeriods_[t] < 10) hmmPeriods_[t] = 10;
        if (hmmPeriods_[t] > 90) hmmPeriods_[t] = 90;
        hmmStateOffsets_[t] = totalHmmStates_;
        totalHmmStates_ += hmmPeriods_[t];
    }
    hmmStateOffsets_[TEMPO_BINS] = totalHmmStates_;  // Sentinel

    // Safety: abort if state space exceeds buffer (should never happen with 20 bins, lags 18-60)
    if (totalHmmStates_ > MAX_HMM_STATES) {
        BLINKY_ASSERT(false, "AudioController: HMM states exceed MAX_HMM_STATES");
        hmmInitialized_ = false;
        return;
    }

    // Initialize alpha to uniform
    float uniformProb = 1.0f / static_cast<float>(totalHmmStates_);
    for (int i = 0; i < totalHmmStates_; i++) {
        hmmAlpha_[i] = uniformProb;
    }

    hmmBestTempo_ = TEMPO_BINS / 2;
    hmmBestPosition_ = hmmPeriods_[hmmBestTempo_] / 2;
    hmmPrevBestPosition_ = hmmBestPosition_;  // Match initial state to prevent spurious first-frame beat
    hmmInitialized_ = true;
}

void AudioController::updateHmmForward(float odf) {
    if (!hmmInitialized_ || totalHmmStates_ == 0) return;

    // Clamp ODF to [0, 1] for observation model.
    // The smoothed ODF (BandFlux pre-threshold) is unbounded above — log-compressed
    // weighted spectral flux with band weights 2.0/1.5/0.1 routinely exceeds 1.0.
    // Without clamping, obsNonBeat floors at 0.01 and observation ratios explode.
    float odfClamped = clampf(odf, 0.0f, 1.0f);

    // Apply power-law contrast to sharpen beat/non-beat distinction.
    // With contrast > 1, small ODF values (non-beats) get pushed toward 0,
    // while values near 1 (beats) stay high. This makes the observation model
    // more decisive, counteracting the structural bias toward longer periods.
    if (hmmContrast != 1.0f && odfClamped > 0.0f) {
        odfClamped = powf(odfClamped, hmmContrast);
    }

    // Observation likelihoods (Bernoulli model, requires odf ∈ [0,1])
    float obsBeat = (odfClamped > 0.01f) ? odfClamped : 0.01f;       // P(obs | beat position)
    float obsNonBeat = (1.0f - odfClamped > 0.01f) ? (1.0f - odfClamped) : 0.01f;  // P(obs | non-beat)

    // Step 1: Save wrap probabilities (probability at last position of each tempo bin)
    // These are the states that will transition to beat positions (position 0)
    float wrapProb[TEMPO_BINS];
    for (int t = 0; t < TEMPO_BINS; t++) {
        int lastPos = hmmStateOffsets_[t] + hmmPeriods_[t] - 1;
        wrapProb[t] = (lastPos < totalHmmStates_) ? hmmAlpha_[lastPos] : 0.0f;
    }

    // Step 2: Shift non-beat states forward (position p-1 → p)
    // This is the deterministic advance: position increments each frame
    for (int t = 0; t < TEMPO_BINS; t++) {
        int offset = hmmStateOffsets_[t];
        int period = hmmPeriods_[t];
        // Shift from end to start to avoid overwriting
        for (int p = period - 1; p >= 1; p--) {
            hmmAlpha_[offset + p] = hmmAlpha_[offset + p - 1] * obsNonBeat;
        }
    }

    // Step 3: Compute beat states (position 0) via Viterbi transition
    // At beat boundary: max over all source tempo bins' wrap probabilities
    for (int t = 0; t < TEMPO_BINS; t++) {
        float maxPred = 0.0f;
        for (int tPrev = 0; tPrev < TEMPO_BINS; tPrev++) {
            float val = wrapProb[tPrev] * transMatrix_[t][tPrev];
            if (val > maxPred) maxPred = val;
        }
        hmmAlpha_[hmmStateOffsets_[t]] = maxPred * obsBeat;
    }

    // Step 4: Normalize to prevent underflow
    float alphaSum = 0.0f;
    for (int i = 0; i < totalHmmStates_; i++) {
        alphaSum += hmmAlpha_[i];
    }
    if (alphaSum > 1e-30f) {
        float invSum = 1.0f / alphaSum;
        for (int i = 0; i < totalHmmStates_; i++) {
            hmmAlpha_[i] *= invSum;
        }
    } else {
        // Degenerate — reset to uniform
        float uniformProb = 1.0f / static_cast<float>(totalHmmStates_);
        for (int i = 0; i < totalHmmStates_; i++) {
            hmmAlpha_[i] = uniformProb;
        }
    }

    // Step 5: Find best state (argmax)
    // Two modes: raw argmax (hmmTempoNorm=false) or tempo-normalized (default).
    // Tempo-normalized divides each bin's best probability by its period length,
    // removing the structural bias where longer-period bins accumulate more mass
    // simply by having more non-beat positions.
    hmmPrevBestPosition_ = hmmBestPosition_;
    float bestAlpha = 0.0f;
    for (int t = 0; t < TEMPO_BINS; t++) {
        int offset = hmmStateOffsets_[t];
        int period = hmmPeriods_[t];
        float periodNorm = hmmTempoNorm ? (1.0f / static_cast<float>(period)) : 1.0f;
        for (int p = 0; p < period; p++) {
            float score = hmmAlpha_[offset + p] * periodNorm;
            if (score > bestAlpha) {
                bestAlpha = score;
                hmmBestTempo_ = t;
                hmmBestPosition_ = p;
            }
        }
    }

    // Update CBSS mean tracking (needed for cbssThresholdFactor even in HMM mode)
    static constexpr float CBSS_MEAN_ALPHA_HMM = 0.008f;
    cbssMean_ = cbssMean_ * (1.0f - CBSS_MEAN_ALPHA_HMM) + odf * CBSS_MEAN_ALPHA_HMM;

    // Advance sample counter (shared with CBSS for phase derivation)
    sampleCounter_++;

    // Prevent signed integer overflow (same as CBSS path)
    if (sampleCounter_ > 1000000) {
        int shift = sampleCounter_ - OSS_BUFFER_SIZE;
        sampleCounter_ -= shift;
        lastBeatSample_ -= shift;
        lastTransientSample_ -= shift;
        if (lastBeatSample_ < 0) lastBeatSample_ = 0;
        if (lastTransientSample_ < 0) lastTransientSample_ = -1;
        for (int i = 0; i < ioiOnsetCount_; i++) {
            ioiOnsetSamples_[i] -= shift;
            if (ioiOnsetSamples_[i] < 0) ioiOnsetSamples_[i] = 0;
        }
    }
}

void AudioController::detectBeatHmm() {
    uint32_t nowMs = time_.millis();

    bool beatDetected = false;

    // Beat fires when best state is at position 0 AND previous was not
    // (i.e., we just crossed a beat boundary)
    if (hmmBestPosition_ == 0 && hmmPrevBestPosition_ != 0) {
        // Adaptive threshold: suppress beats during silence (same as CBSS)
        float currentOdf = lastSmoothedOnset_;
        bool aboveThreshold = (cbssThresholdFactor <= 0.0f) ||
                               (currentOdf > cbssThresholdFactor * cbssMean_);

        if (aboveThreshold) {
            lastBeatSample_ = sampleCounter_;
            if (beatCount_ < 65535) beatCount_++;
            beatDetected = true;
            cbssConfidence_ = clampf(cbssConfidence_ + 0.15f, 0.0f, 1.0f);
            updateBeatStability(nowMs);
            lastFiredBeatPredicted_ = true;  // HMM beats are always "predicted"

            // Update beat period from HMM state
            int hmmPeriod = hmmPeriods_[hmmBestTempo_];
            beatPeriodSamples_ = hmmPeriod;

            // Update BPM from HMM state
            bpm_ = tempoBinBpms_[hmmBestTempo_];
            beatPeriodMs_ = 60000.0f / bpm_;
            bayesBestBin_ = hmmBestTempo_;

            // Update ensemble detector with tempo hint
            ensemble_.getFusion().setTempoHint(bpm_);
        }
    }

    // Decay confidence when no beat
    if (!beatDetected) {
        cbssConfidence_ *= beatConfidenceDecay;
    }

    // Derive phase from HMM state: position / period
    int period = hmmPeriods_[hmmBestTempo_];
    if (period < 10) period = 10;
    float newPhase = static_cast<float>(hmmBestPosition_) / static_cast<float>(period);
    if (newPhase >= 1.0f) newPhase = 0.0f;
    if (newPhase < 0.0f) newPhase = 0.0f;
    if (!isfinite(newPhase)) newPhase = 0.0f;
    phase_ = newPhase;

    // Update timeToNextBeat_ for serial streaming compatibility
    timeToNextBeat_ = period - hmmBestPosition_;

    predictNextBeat(nowMs);
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

    // Beat declared when countdown reaches zero AND CBSS is above adaptive threshold
    if (timeToNextBeat_ <= 0) {
        // Adaptive threshold: suppress beats during silence/breakdowns
        // When cbssThresholdFactor > 0, require current CBSS > factor * running mean
        float currentCBSS = cbssBuffer_[(sampleCounter_ > 0 ? sampleCounter_ - 1 : 0) % OSS_BUFFER_SIZE];
        bool cbssAboveThreshold = (cbssThresholdFactor <= 0.0f) ||
                                   (currentCBSS > cbssThresholdFactor * cbssMean_);

        if (cbssAboveThreshold) {
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
                lastBeatSample_ = sampleCounter_ - bestSnapOffset;
            } else {
                lastBeatSample_ = sampleCounter_;
            }
            if (beatCount_ < 65535) beatCount_++;
            beatDetected = true;
            cbssConfidence_ = clampf(cbssConfidence_ + 0.15f, 0.0f, 1.0f);
            updateBeatStability(nowMs);

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

            // Phase alignment checker: fix anti-phase lock using raw OSS
            if (phaseCheckEnabled) {
                beatsSincePhaseCheck_++;
                if (beatsSincePhaseCheck_ >= phaseCheckBeats) {
                    checkPhaseAlignment();
                    beatsSincePhaseCheck_ = 0;
                }
            }
        }

        // Always reset timers (even if suppressed) to prevent re-firing stale countdown
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

