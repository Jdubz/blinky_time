#include "BandWeightedFluxDetector.h"
#include <math.h>

BandWeightedFluxDetector::BandWeightedFluxDetector()
    : prevCombinedFlux_(0.0f)
    , hasPrevFrame_(false)
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
    , confirmCountdown_(0)
    , candidateFlux_(0.0f)
    , minFluxDuringWindow_(0.0f)
    , cachedResult_(DetectionResult::none())
{
    for (int i = 0; i < MAX_STORED_BINS; i++) {
        prevLogMag_[i] = 0.0f;
    }
}

void BandWeightedFluxDetector::resetImpl() {
    hasPrevFrame_ = false;
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

    for (int i = 0; i < MAX_STORED_BINS; i++) {
        prevLogMag_[i] = 0.0f;
    }
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

    // If no previous frame, store as reference and return
    if (!hasPrevFrame_) {
        for (int k = 0; k < effectiveMax; k++) {
            prevLogMag_[k] = logMag[k];
        }
        hasPrevFrame_ = true;
        return DetectionResult::none();
    }

    // Step 2: Build 3-bin max-filtered reference (SuperFlux vibrato suppression)
    float maxRef[MAX_STORED_BINS] = {0};
    for (int k = 0; k < effectiveMax; k++) {
        float left  = (k > 0) ? prevLogMag_[k - 1] : prevLogMag_[k];
        float center = prevLogMag_[k];
        float right = (k < effectiveMax - 1) ? prevLogMag_[k + 1] : prevLogMag_[k];
        maxRef[k] = maxf(maxf(left, center), right);
    }

    // Step 3: Compute per-band flux (half-wave rectified)
    computeBandFlux(logMag, maxRef, effectiveMax);

    // Step 4: Combined weighted ODF
    combinedFlux_ = bassWeight_ * bassFlux_ + midWeight_ * midFlux_ + highWeight_ * highFlux_;

    // Store for debug
    lastRawValue_ = combinedFlux_;

    // Update running mean (EMA with slow adaptation)
    frameCount_++;
    if (frameCount_ < 10) {
        // Cold start: fast adaptation
        averageFlux_ += 0.2f * (combinedFlux_ - averageFlux_);
    } else {
        averageFlux_ += 0.02f * (combinedFlux_ - averageFlux_);
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
                return cachedResult_;
            }
            // Flux never dipped — sustained sound (pad/chord), reject
        }
        // Still waiting or rejected — update reference and return none.
        // Note: updateThresholdBuffer is called here (during window frames) but NOT on
        // the original onset frame (asymmetric design) — this is intentional to prevent
        // loud onsets from inflating the running average while still adapting to post-onset flux.
        updatePrevFrameState(logMag, effectiveMax);
        updateThresholdBuffer(combinedFlux_);
        return DetectionResult::none();
    }

    // Step 6: Hi-hat rejection gate
    // Suppress if ONLY the high band has flux (no bass or mid energy)
    bool hiHatOnly = (highFlux_ > 0.01f) && (bassFlux_ < 0.005f) && (midFlux_ < 0.005f);

    // Detection
    bool detected = (combinedFlux_ > effectiveThreshold) && !hiHatOnly;

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

    return result;
}

void BandWeightedFluxDetector::updatePrevFrameState(const float* logMag, int effectiveMax) {
    prevCombinedFlux_ = combinedFlux_;
    for (int k = 0; k < effectiveMax; k++) {
        prevLogMag_[k] = logMag[k];
    }
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
