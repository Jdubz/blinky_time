#include "BandWeightedFluxDetector.h"
#include <math.h>

BandWeightedFluxDetector::BandWeightedFluxDetector()
    : historyCount_(0)
    , diffFrames_(1)
    , prevCombinedFlux_(0.0f)
    , bassFlux_(0.0f)
    , midFlux_(0.0f)
    , highFlux_(0.0f)
    , combinedFlux_(0.0f)
    , averageFlux_(0.0f)
    , frameCount_(0)
    , gamma_(20.0f)
    , bassWeight_(2.0f)
    , midWeight_(1.5f)
    , highWeight_(0.1f)
    , minOnsetDelta_(0.3f)
    , bandDominanceGate_(0.0f)
    , decayRatioThreshold_(0.0f)
    , crestGate_(0.0f)
    , confirmFrames_(3)
    , maxBin_(64)
    , perBandThreshEnabled_(false)
    , perBandThreshMult_(1.5f)
    , confirmCountdown_(0)
    , candidateFlux_(0.0f)
    , minFluxDuringWindow_(0.0f)
    , cachedResult_(DetectionResult::none())
    , averageBassFlux_(0.0f)
    , averageMidFlux_(0.0f)
    , bassHistoryCount_(0)
    , hiResBassFlux_(0.0f)
    , hiResBassEnabled_(false)
{
    memset(historyLogMag_, 0, sizeof(historyLogMag_));
    memset(historyBassLogMag_, 0, sizeof(historyBassLogMag_));
}

void BandWeightedFluxDetector::resetImpl() {
    prevCombinedFlux_ = 0.0f;
    bassFlux_ = 0.0f;
    midFlux_ = 0.0f;
    highFlux_ = 0.0f;
    combinedFlux_ = 0.0f;
    averageFlux_ = 0.0f;
    frameCount_ = 0;
    confirmCountdown_ = 0;
    candidateFlux_ = 0.0f;
    minFluxDuringWindow_ = 0.0f;
    averageBassFlux_ = 0.0f;
    averageMidFlux_ = 0.0f;
    historyCount_ = 0;
    bassHistoryCount_ = 0;
    hiResBassFlux_ = 0.0f;

    memset(historyLogMag_, 0, sizeof(historyLogMag_));
    memset(historyBassLogMag_, 0, sizeof(historyBassLogMag_));
}

DetectionResult BandWeightedFluxDetector::detect(const AudioFrame& frame, float dt) {
    // Skip if disabled or no spectral data
    if (!config_.enabled || !frame.spectralValid) {
        return DetectionResult::none();
    }

    const float* magnitudes = frame.magnitudes;
    int numBins = frame.numBins;

    // Clamp analysis range
    int effectiveMax = maxBin_;
    if (effectiveMax > numBins) effectiveMax = numBins;
    if (effectiveMax > MAX_STORED_BINS) effectiveMax = MAX_STORED_BINS;

    // Step 1: Log-compress current magnitudes
    float logMag[MAX_STORED_BINS];
    for (int k = 0; k < effectiveMax; k++) {
        logMag[k] = fastLog1p(gamma_ * magnitudes[k]);
    }

    // Hi-res bass: log-compress Goertzel magnitudes (when available)
    bool useHiResBass = hiResBassEnabled_ && frame.bassSpectralValid && bassHistoryCount_ > 0;
    float bassLogMag[MAX_BASS_BINS] = {};
    if (hiResBassEnabled_ && frame.bassSpectralValid) {
        for (int b = 0; b < frame.numBassBins && b < MAX_BASS_BINS; b++) {
            bassLogMag[b] = fastLog1p(gamma_ * frame.bassMagnitudes[b]);
        }
        if (bassHistoryCount_ == 0) {
            updateBassPrevFrameState(bassLogMag, frame.numBassBins);
        }
    }

    // If no history frames yet, store and return
    if (historyCount_ == 0) {
        updatePrevFrameState(logMag, effectiveMax);
        if (useHiResBass) updateBassPrevFrameState(bassLogMag, frame.numBassBins);
        return DetectionResult::none();
    }

    // Step 2: Build 3-bin max-filtered reference (SuperFlux vibrato suppression)
    // Uses diffFrames_ to look back N frames (default 1 = previous frame)
    const float* refFrame = getReferenceFrame();
    float maxRef[MAX_STORED_BINS] = {0};
    for (int k = 0; k < effectiveMax; k++) {
        float left  = (k > 0) ? refFrame[k - 1] : refFrame[k];
        float center = refFrame[k];
        float right = (k < effectiveMax - 1) ? refFrame[k + 1] : refFrame[k];
        maxRef[k] = maxf(maxf(left, center), right);
    }

    // Step 3: Compute per-band flux (half-wave rectified)
    computeBandFlux(logMag, maxRef, effectiveMax);

    // Step 3b: Hi-res bass flux override (when Goertzel data available)
    // Uses 12 bins at 31.25 Hz/bin instead of 6 bins at 62.5 Hz/bin
    if (useHiResBass) {
        computeHiResBassFlux(bassLogMag, frame.numBassBins);
        // Replace bass flux from FFT-256 with hi-res Goertzel bass flux
        bassFlux_ = hiResBassFlux_;
    } else {
        hiResBassFlux_ = 0.0f;
    }

    // Step 4: Combined weighted ODF
    combinedFlux_ = bassWeight_ * bassFlux_ + midWeight_ * midFlux_ + highWeight_ * highFlux_;

    // Store for debug
    lastRawValue_ = combinedFlux_;

    // Update running mean (EMA with slow adaptation)
    frameCount_++;
    if (frameCount_ < 10) {
        // Cold start: fast adaptation
        averageFlux_ += 0.2f * (combinedFlux_ - averageFlux_);
        if (perBandThreshEnabled_) {
            averageBassFlux_ += 0.2f * (bassFlux_ - averageBassFlux_);
            averageMidFlux_ += 0.2f * (midFlux_ - averageMidFlux_);
        }
    } else {
        averageFlux_ += 0.02f * (combinedFlux_ - averageFlux_);
        if (perBandThreshEnabled_) {
            averageBassFlux_ += 0.02f * (bassFlux_ - averageBassFlux_);
            averageMidFlux_ += 0.02f * (midFlux_ - averageMidFlux_);
        }
    }

    // Step 5: Additive threshold = mean + delta
    // config_.threshold is the additive delta (not multiplicative)
    float effectiveThreshold = averageFlux_ + config_.threshold;
    currentThreshold_ = effectiveThreshold;

    // === Post-onset decay confirmation (disabled by default, decayRatioThreshold_=0) ===
    // If we're waiting to confirm a previous candidate, track minimum flux.
    // Note: Any new onset during the confirmation window is silently dropped —
    // the early return bypasses all detection logic. At confirmFrames_=3 (~50ms)
    // this is acceptable; at higher values rapid onsets could be missed.
    // Also introduces ~50ms latency on confirmed detections (confirmFrames_ × 16.7ms).
    if (confirmCountdown_ > 0) {
        if (combinedFlux_ < minFluxDuringWindow_) {
            minFluxDuringWindow_ = combinedFlux_;
        }
        confirmCountdown_--;
        if (confirmCountdown_ == 0) {
            // Check if flux dipped at ANY point during the window (percussive = brief dip)
            // Pads never dip — they sustain or rise throughout the window
            float minRatio = minFluxDuringWindow_ / maxf(candidateFlux_, 0.001f);
            if (minRatio <= decayRatioThreshold_) {
                // Flux dipped — confirmed percussive onset
                updatePrevFrameState(logMag, effectiveMax);
                if (useHiResBass) updateBassPrevFrameState(bassLogMag, frame.numBassBins);
                return cachedResult_;
            }
            // Flux never dipped — sustained sound (pad/chord), reject
        }
        // Still waiting or rejected — update reference and return none.
        // Note: updateThresholdBuffer is called here (during window frames) but NOT on
        // the original onset frame (asymmetric design) — this is intentional to prevent
        // loud onsets from inflating the running average while still adapting to post-onset flux.
        updatePrevFrameState(logMag, effectiveMax);
        if (useHiResBass) updateBassPrevFrameState(bassLogMag, frame.numBassBins);
        updateThresholdBuffer(combinedFlux_);
        return DetectionResult::none();
    }

    // Step 6: Hi-hat rejection gate
    // Suppress if ONLY the high band has flux (no bass or mid energy)
    bool hiHatOnly = (highFlux_ > 0.01f) && (bassFlux_ < 0.005f) && (midFlux_ < 0.005f);

    // Detection: combined flux exceeds threshold
    bool detected = (combinedFlux_ > effectiveThreshold) && !hiHatOnly;

    // Per-band independent detection: bass or mid alone exceeds its own threshold
    // Catches kicks hidden in combined flux when mid/high are quiet
    if (!detected && perBandThreshEnabled_ && !hiHatOnly) {
        float bassThresh = averageBassFlux_ + config_.threshold * perBandThreshMult_;
        float midThresh = averageMidFlux_ + config_.threshold * perBandThreshMult_;
        if (bassFlux_ > bassThresh || midFlux_ > midThresh) {
            detected = true;
        }
    }

    // Step 7: Onset sharpness gate — reject slow-rising signals (pads, swells)
    // Kicks jump from ~0 to 2+ in one frame; pads rise 0.01-0.1 per frame
    if (detected && minOnsetDelta_ > 0.0f) {
        float fluxDelta = combinedFlux_ - prevCombinedFlux_;
        if (fluxDelta < minOnsetDelta_) {
            detected = false;
        }
    }

    // Step 8: Band-dominance gate (disabled by default, kept for experimentation)
    if (detected && bandDominanceGate_ > 0.0f) {
        float totalBandFlux = bassFlux_ + midFlux_ + highFlux_;
        if (totalBandFlux > 0.01f) {
            float maxBand = maxf(maxf(bassFlux_, midFlux_), highFlux_);
            float dominance = maxBand / totalBandFlux;
            if (dominance < bandDominanceGate_) {
                detected = false;
            }
        }
    }

    // Step 9: Spectral crest factor gate — reject tonal onsets (pads, chords)
    // Percussive hits are broadband noise (low crest ~2-3), pads are tonal (high crest ~5+)
    if (detected && crestGate_ > 0.0f) {
        float maxMag = 0.0f;
        float sumMag = 0.0f;
        int crestMax = (MID_MAX < effectiveMax) ? MID_MAX : effectiveMax;
        for (int k = BASS_MIN; k < crestMax; k++) {
            if (magnitudes[k] > maxMag) maxMag = magnitudes[k];
            sumMag += magnitudes[k];
        }
        int crestBins = crestMax - BASS_MIN;
        if (crestBins > 0 && sumMag > 1e-10f) {
            float crest = maxMag / (sumMag / crestBins);
            if (crest > crestGate_) {
                detected = false;
            }
        }
    }

    DetectionResult result;

    if (detected) {
        // Strength: how far above threshold, normalized
        float excess = combinedFlux_ - effectiveThreshold;
        float strength = clamp01(excess / maxf(config_.threshold, 0.01f));

        // Confidence
        float confidence = computeConfidence(combinedFlux_, averageFlux_);

        result = DetectionResult::hit(strength, confidence);

        // Step 10: Post-onset decay gate — defer confirmation to check temporal envelope
        // Percussive hits decay rapidly (ratio drops below threshold in N frames)
        // Pads/chords sustain (ratio stays near 1.0)
        if (decayRatioThreshold_ > 0.0f && confirmFrames_ > 0) {
            confirmCountdown_ = confirmFrames_;
            candidateFlux_ = combinedFlux_;
            minFluxDuringWindow_ = combinedFlux_;  // Will track minimum during window
            cachedResult_ = result;
            // Don't return hit yet — wait for decay confirmation
            result = DetectionResult::none();
        }

        // Asymmetric threshold update: do NOT update threshold buffer on detection
        // This prevents loud onsets from inflating the threshold
    } else {
        result = DetectionResult::none();

        // Only update threshold buffer on non-detection frames
        updateThresholdBuffer(combinedFlux_);
    }

    // Store current as reference for next frame
    updatePrevFrameState(logMag, effectiveMax);
    if (useHiResBass) updateBassPrevFrameState(bassLogMag, frame.numBassBins);

    return result;
}

void BandWeightedFluxDetector::updatePrevFrameState(const float* logMag, int effectiveMax) {
    prevCombinedFlux_ = combinedFlux_;

    // Shift history: move each frame back one slot (newest→oldest)
    // Frame 0 = most recent, frame 1 = one before, etc.
    for (int f = MAX_HISTORY_FRAMES - 1; f > 0; f--) {
        memcpy(historyLogMag_[f], historyLogMag_[f - 1], sizeof(float) * MAX_STORED_BINS);
    }

    // Store current frame as most recent history
    for (int k = 0; k < effectiveMax; k++) {
        historyLogMag_[0][k] = logMag[k];
    }
    // Zero remaining bins
    for (int k = effectiveMax; k < MAX_STORED_BINS; k++) {
        historyLogMag_[0][k] = 0.0f;
    }

    if (historyCount_ < MAX_HISTORY_FRAMES) {
        historyCount_++;
    }
}

const float* BandWeightedFluxDetector::getReferenceFrame() const {
    // diffFrames_=1 means previous frame (index 0), =2 means two ago (index 1), etc.
    // Clamp to available history
    int idx = diffFrames_ - 1;
    if (idx >= historyCount_) {
        idx = historyCount_ - 1;
    }
    if (idx < 0) idx = 0;
    return historyLogMag_[idx];
}

void BandWeightedFluxDetector::computeBandFlux(const float* logMag, const float* maxRef, int numBins) {
    // Bass band: bins [BASS_MIN, BASS_MAX)
    bassFlux_ = 0.0f;
    int bassCount = 0;
    int bassMax = (BASS_MAX < numBins) ? BASS_MAX : numBins;
    for (int k = BASS_MIN; k < bassMax; k++) {
        float diff = logMag[k] - maxRef[k];
        if (diff > 0.0f) bassFlux_ += diff;
        bassCount++;
    }
    if (bassCount > 0) bassFlux_ /= bassCount;

    // Mid band: bins [MID_MIN, MID_MAX)
    midFlux_ = 0.0f;
    int midCount = 0;
    int midMax = (MID_MAX < numBins) ? MID_MAX : numBins;
    for (int k = MID_MIN; k < midMax; k++) {
        float diff = logMag[k] - maxRef[k];
        if (diff > 0.0f) midFlux_ += diff;
        midCount++;
    }
    if (midCount > 0) midFlux_ /= midCount;

    // High band: bins [HIGH_MIN, numBins)
    highFlux_ = 0.0f;
    int highCount = 0;
    for (int k = HIGH_MIN; k < numBins; k++) {
        float diff = logMag[k] - maxRef[k];
        if (diff > 0.0f) highFlux_ += diff;
        highCount++;
    }
    if (highCount > 0) highFlux_ /= highCount;
}

float BandWeightedFluxDetector::computeConfidence(float flux, float mean) const {
    // Confidence based on how far above the mean we are
    float ratio = flux / maxf(mean, 0.001f);

    // ratio of 2 = decent confidence, 4+ = high confidence
    float ratioConf = clamp01((ratio - 1.0f) / 3.0f);

    // Also consider absolute flux level (very low flux = low confidence)
    float absConf = clamp01(flux / 1.0f);  // Full confidence at flux=1.0

    // Combine
    float confidence = 0.7f * ratioConf + 0.3f * absConf;

    // Floor at 0.2 when detected (always some confidence)
    return clamp01(confidence * 0.8f + 0.2f);
}

void BandWeightedFluxDetector::setHiResBass(bool e) {
    hiResBassEnabled_ = e;
    // Reset bass history to avoid computing flux against stale data
    // from a previous session when re-enabling
    bassHistoryCount_ = 0;
    hiResBassFlux_ = 0.0f;
    memset(historyBassLogMag_, 0, sizeof(historyBassLogMag_));
}

// --- Hi-res bass helpers ---

void BandWeightedFluxDetector::computeHiResBassFlux(const float* bassLogMag, int numBins) {
    // Compute bass flux from 12 Goertzel bins (31.25 Hz/bin)
    // Uses 3-bin max-filter on reference (±31 Hz spread) to suppress spectral
    // wobble in sustained bass. Narrower than FFT path's ±62 Hz filter.
    // Normalizes by FFT-256 bass bin count (BASS_MAX - BASS_MIN = 6) so the
    // hi-res flux is scaled to match the FFT path for threshold compatibility.

    int n = (numBins < MAX_BASS_BINS) ? numBins : MAX_BASS_BINS;

    const float* bassRef = getBassReferenceFrame();

    float flux = 0.0f;
    for (int b = 0; b < n; b++) {
        // 3-bin max-filter on reference. At 31.25 Hz/bin this covers ±31 Hz,
        // inherently narrower than the FFT path's 3-bin filter (±62 Hz).
        // Suppresses spectral wobble in sustained bass.
        float refVal = bassRef[b];
        if (b > 0) refVal = fmaxf(refVal, bassRef[b - 1]);
        if (b < n - 1) refVal = fmaxf(refVal, bassRef[b + 1]);

        float diff = bassLogMag[b] - refVal;
        if (diff > 0.0f) flux += diff;
    }

    // Normalize by FFT-256 bass bin count (6), not actual bin count (12).
    // The 12 hi-res bins cover the same frequency range as 6 FFT bins;
    // dividing by 12 would halve the flux for the same physical kick.
    static constexpr int FFT_BASS_BIN_COUNT = BASS_MAX - BASS_MIN;  // 6
    static_assert(FFT_BASS_BIN_COUNT == 6, "Hi-res bass normalization assumes 6 FFT bass bins");
    hiResBassFlux_ = flux / FFT_BASS_BIN_COUNT;
}

void BandWeightedFluxDetector::updateBassPrevFrameState(const float* bassLogMag, int numBins) {
    int n = (numBins < MAX_BASS_BINS) ? numBins : MAX_BASS_BINS;

    // Shift history: move each frame back one slot
    for (int f = MAX_HISTORY_FRAMES - 1; f > 0; f--) {
        memcpy(historyBassLogMag_[f], historyBassLogMag_[f - 1], sizeof(float) * MAX_BASS_BINS);
    }

    // Store current frame as most recent
    for (int b = 0; b < n; b++) {
        historyBassLogMag_[0][b] = bassLogMag[b];
    }
    for (int b = n; b < MAX_BASS_BINS; b++) {
        historyBassLogMag_[0][b] = 0.0f;
    }

    if (bassHistoryCount_ < MAX_HISTORY_FRAMES) {
        bassHistoryCount_++;
    }
}

const float* BandWeightedFluxDetector::getBassReferenceFrame() const {
    // Same logic as getReferenceFrame() but for bass history
    int idx = diffFrames_ - 1;
    if (idx >= bassHistoryCount_) {
        idx = bassHistoryCount_ - 1;
    }
    if (idx < 0) idx = 0;
    return historyBassLogMag_[idx];
}
