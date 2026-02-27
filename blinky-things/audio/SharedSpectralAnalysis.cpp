#include "SharedSpectralAnalysis.h"
#include "../types/BlinkyAssert.h"
#include <arduinoFFT.h>
#include <math.h>

// Pre-computed mel filterbank bin boundaries
// Generated for 26 mel bands from 60-8000 Hz at 16kHz/256-point FFT
// Each band is a triangular filter spanning [start, center, end] bins
static const MelBandDef MEL_BANDS[SpectralConstants::NUM_MEL_BANDS] = {
    // Band 0-5: Low bass (60-200 Hz)
    { 1,  1,  2},  // 0: 62.5 Hz center
    { 1,  2,  3},  // 1: 125 Hz center
    { 2,  3,  4},  // 2: 187.5 Hz center
    { 3,  4,  5},  // 3: 250 Hz center
    { 4,  5,  6},  // 4: 312.5 Hz center
    { 5,  6,  8},  // 5: 375 Hz center

    // Band 6-11: Mid-bass to low-mid (400-800 Hz)
    { 6,  8, 10},  // 6: 500 Hz center
    { 8, 10, 12},  // 7: 625 Hz center
    {10, 12, 14},  // 8: 750 Hz center
    {12, 14, 17},  // 9: 875 Hz center
    {14, 17, 20},  // 10: 1062 Hz center
    {17, 20, 24},  // 11: 1250 Hz center

    // Band 12-17: Mid frequencies (1.5-3 kHz)
    {20, 24, 28},  // 12: 1500 Hz center
    {24, 28, 33},  // 13: 1750 Hz center
    {28, 33, 39},  // 14: 2062 Hz center
    {33, 39, 46},  // 15: 2437 Hz center
    {39, 46, 54},  // 16: 2875 Hz center
    {46, 54, 63},  // 17: 3375 Hz center

    // Band 18-21: High-mid frequencies (3.9-6.2 kHz)
    {54, 63, 74},  // 18: 3937 Hz center
    {63, 74, 86},  // 19: 4625 Hz center
    {74, 86, 100}, // 20: 5375 Hz center
    {86, 100, 116},// 21: 6250 Hz center

    // Band 22-25: High frequencies (7-8 kHz)
    {100, 116, 127},// 22: 7250 Hz center
    {116, 127, 127},// 23: 8000 Hz center (at Nyquist)
    {116, 127, 127},// 24: (extended)
    {116, 127, 127} // 25: (extended)
};

SharedSpectralAnalysis::SharedSpectralAnalysis()
    : sampleBuffer_{}
    , sampleCount_(0)
    , writeIndex_(0)
    , vReal_{}
    , vImag_{}
    , magnitudes_{}
    , phases_{}
    , prevMagnitudes_{}
    , melBands_{}
    , prevMelBands_{}
    , melRunningMax_{}
    , binRunningMax_{}
    , smoothedGainDb_(0.0f)
    , frameRmsDb_(-200.0f)
    , totalEnergy_(0.0f)
    , spectralCentroid_(0.0f)
    , frameReady_(false)
    , hasPrevFrame_(false)
{
}

void SharedSpectralAnalysis::begin() {
    reset();
}

void SharedSpectralAnalysis::reset() {
    sampleCount_ = 0;
    writeIndex_ = 0;
    frameReady_ = false;
    hasPrevFrame_ = false;
    totalEnergy_ = 0.0f;
    spectralCentroid_ = 0.0f;

    // Clear all buffers
    for (int i = 0; i < SpectralConstants::FFT_SIZE; i++) {
        sampleBuffer_[i] = 0;
        vReal_[i] = 0.0f;
        vImag_[i] = 0.0f;
    }
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        magnitudes_[i] = 0.0f;
        phases_[i] = 0.0f;
        prevMagnitudes_[i] = 0.0f;
    }
    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        melBands_[i] = 0.0f;
        prevMelBands_[i] = 0.0f;
        melRunningMax_[i] = 0.0f;
    }
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        binRunningMax_[i] = 0.0f;
    }
    smoothedGainDb_ = 0.0f;
    frameRmsDb_ = -200.0f;
}

bool SharedSpectralAnalysis::addSamples(const int16_t* samples, int count) {
    for (int i = 0; i < count && sampleCount_ < SpectralConstants::FFT_SIZE; i++) {
        sampleBuffer_[writeIndex_] = samples[i];
        writeIndex_ = (writeIndex_ + 1) % SpectralConstants::FFT_SIZE;
        sampleCount_++;
    }
    return sampleCount_ >= SpectralConstants::FFT_SIZE;
}

void SharedSpectralAnalysis::process() {
    if (sampleCount_ < SpectralConstants::FFT_SIZE) {
        return;  // Not enough samples
    }

    // Save previous frame data before overwriting
    savePreviousFrame();

    // Copy samples to vReal_, starting from oldest sample in ring buffer
    for (int i = 0; i < SpectralConstants::FFT_SIZE; i++) {
        int idx = (writeIndex_ + i) % SpectralConstants::FFT_SIZE;
        // Normalize int16 to float (-1.0 to 1.0)
        vReal_[i] = sampleBuffer_[idx] / 32768.0f;
        vImag_[i] = 0.0f;
    }

    // Apply windowing
    applyHammingWindow();

    // Compute FFT
    computeFFT();

    // Extract magnitudes and phases from FFT output
    computeMagnitudesAndPhases();

    // Frame-level soft-knee compression (normalizes gross signal level)
    applyCompressor();

    // Compute derived features (energy, centroid) from compressed magnitudes
    // NOTE: totalEnergy_ and spectralCentroid_ reflect compressed-but-not-whitened
    // magnitudes. getMagnitudes() returns whitened values after whitenMagnitudes() below.
    computeDerivedFeatures();

    // --- Pipeline ordering rationale ---
    // Mel bands are computed BEFORE per-bin whitening, intentionally:
    //   1. Mel bands use compressed-but-not-whitened magnitudes as input
    //   2. Mel bands then get their own whitening (whitenMelBands)
    //   3. Per-bin whitening runs last, modifying magnitudes_ in-place
    //
    // Why: Mel bands aggregate multiple FFT bins into perceptual bands.
    // Whitening the 128 bins first, then computing mel bands from whitened values,
    // would lose the relative energy information between bins within a band.
    // Instead, each domain gets its own whitening tuned to its resolution:
    //   - 26 mel bands: per-band running max (coarse, perceptual)
    //   - 128 FFT bins: per-bin running max (fine, for BandFlux transient detection)
    computeMelBands();
    whitenMelBands();
    whitenMagnitudes();

    // Mark frame as ready
    frameReady_ = true;
    hasPrevFrame_ = true;

    // Reset sample buffer for next frame
    sampleCount_ = 0;
}

void SharedSpectralAnalysis::applyHammingWindow() {
    // Hamming window: w(n) = 0.54 - 0.46 * cos(2*pi*n/(N-1))
    const float alpha = 0.54f;
    const float beta = 0.46f;
    const float twoPiOverN = 2.0f * 3.14159265f / (SpectralConstants::FFT_SIZE - 1);

    for (int i = 0; i < SpectralConstants::FFT_SIZE; i++) {
        float window = alpha - beta * cosf(twoPiOverN * i);
        vReal_[i] *= window;
    }
}

void SharedSpectralAnalysis::computeFFT() {
    // ArduinoFFT requires buffer references at construction time
    ArduinoFFT<float> fft(vReal_, vImag_, SpectralConstants::FFT_SIZE,
                          SpectralConstants::SAMPLE_RATE);
    fft.compute(FFTDirection::Forward);
}

void SharedSpectralAnalysis::computeMagnitudesAndPhases() {
    // Compute magnitude and phase for each frequency bin
    // Only need first half (bins 0 to NUM_BINS-1) due to symmetry
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        float real = vReal_[i];
        float imag = vImag_[i];

        // Safety: Check for NaN/Inf from FFT output
        if (!safeIsFinite(real)) real = 0.0f;
        if (!safeIsFinite(imag)) imag = 0.0f;

        // Magnitude: sqrt(real^2 + imag^2)
        float mag = sqrtf(real * real + imag * imag);
        magnitudes_[i] = safeIsFinite(mag) ? mag : 0.0f;

        // Phase: atan2(imag, real) -> [-pi, pi]
        float phase = atan2f(imag, real);
        phases_[i] = safeIsFinite(phase) ? phase : 0.0f;
    }
}

void SharedSpectralAnalysis::computeMelBands() {
    // Apply triangular mel filterbank to magnitude spectrum
    // Each mel band is computed by weighting magnitudes with triangular filter

    for (int band = 0; band < SpectralConstants::NUM_MEL_BANDS; band++) {
        const MelBandDef& def = MEL_BANDS[band];
        float sum = 0.0f;
        float weightSum = 0.0f;

        // Rising edge: start to center
        for (int bin = def.startBin; bin <= def.centerBin && bin < SpectralConstants::NUM_BINS; bin++) {
            // Weight increases linearly from 0 at start to 1 at center
            float weight = (def.centerBin > def.startBin)
                ? (float)(bin - def.startBin) / (def.centerBin - def.startBin)
                : 1.0f;
            sum += magnitudes_[bin] * weight;
            weightSum += weight;
        }

        // Falling edge: center to end
        for (int bin = def.centerBin + 1; bin <= def.endBin && bin < SpectralConstants::NUM_BINS; bin++) {
            // Weight decreases linearly from 1 at center to 0 at end
            float weight = (def.endBin > def.centerBin)
                ? 1.0f - (float)(bin - def.centerBin) / (def.endBin - def.centerBin)
                : 1.0f;
            sum += magnitudes_[bin] * weight;
            weightSum += weight;
        }

        // Normalize and apply log compression
        float bandEnergy = (weightSum > 0) ? sum / weightSum : 0.0f;

        // FIX: Special-case silence to ensure mel bands are truly zero
        // Use threshold matching typical noise floor (~1e-6)
        const float silenceThreshold = 1e-6f;
        if (bandEnergy < silenceThreshold) {
            melBands_[band] = 0.0f;
            continue;
        }

        // Log compression: 10 * log10(energy + epsilon)
        // This matches human perception (dB scale)
        const float epsilon = 1e-10f;
        float logEnergy = 10.0f * log10f(bandEnergy + epsilon);

        // Clamp to reasonable range (-100 dB to 0 dB) and normalize to 0-1
        // -60 dB is quiet, -20 dB is moderate, 0 dB is loud
        logEnergy = (logEnergy + 60.0f) / 60.0f;  // Map [-60, 0] to [0, 1]

        melBands_[band] = safeIsFinite(logEnergy) ? clamp01(logEnergy) : 0.0f;
    }
}

void SharedSpectralAnalysis::whitenMelBands() {
    // Adaptive whitening on mel bands (Stowell & Plumbley 2007 adapted)
    //
    // Each mel band is normalized by its recent running maximum.
    // This makes change-based detectors (SpectralFlux, Novelty) invariant
    // to sustained spectral content: a pad holding a chord will have
    // whitened mel bands near 1.0, and only NEW spectral changes will spike.
    //
    // Applied to mel bands (not raw magnitudes) because:
    // - HFC/ComplexDomain need raw magnitudes for absolute energy metrics
    // - Mel bands are perceptually meaningful (26 bands vs 128 bins)
    // - Log-compression is already applied, so whitening operates in dB space

    const float decay = 0.97f;   // Running max decay per FFT frame (~1s at 30 fps)
    const float floor = 0.01f;   // Floor at -54dB normalized (avoids amplifying noise)

    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        float current = melBands_[i];

        // Update running max: max(decayed previous, current)
        float decayedMax = melRunningMax_[i] * decay;
        melRunningMax_[i] = (current > decayedMax) ? current : decayedMax;

        // Whiten: normalize by running max
        float maxVal = (melRunningMax_[i] > floor) ? melRunningMax_[i] : floor;
        melBands_[i] = current / maxVal;
    }
}

void SharedSpectralAnalysis::applyCompressor() {
    // Always compute frame RMS for debug monitoring
    float sumSq = 0.0f;
    for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {
        sumSq += magnitudes_[i] * magnitudes_[i];
    }
    float rms = sqrtf(sumSq / (SpectralConstants::NUM_BINS - 1));

    // Floor to avoid log10f(0). Triggers during true silence (all-zero FFT).
    const float floorLin = 1e-10f;
    if (rms < floorLin) rms = floorLin;
    float rmsDb = 20.0f * log10f(rms);
    frameRmsDb_ = rmsDb;  // Store for debug access

    if (!compressorEnabled) {
        // Fade smoothed gain toward 0 rather than hard-reset, so toggling
        // mid-session doesn't cause an abrupt level jump
        smoothedGainDb_ *= 0.9f;
        return;
    }

    // Soft-knee gain computation (Giannoulis/Massberg/Reiss 2012)
    float gainDb = 0.0f;
    float halfKnee = compKneeDb * 0.5f;
    float diff = rmsDb - compThresholdDb;

    if (diff <= -halfKnee) {
        // Below knee: no compression
        gainDb = 0.0f;
    } else if (diff >= halfKnee) {
        // Above knee: full ratio compression (also handles hard knee when compKneeDb == 0)
        gainDb = (1.0f - 1.0f / compRatio) * (compThresholdDb - rmsDb);
    } else {
        // Within soft knee: quadratic interpolation (only reachable when compKneeDb > 0)
        float x = diff + halfKnee;
        gainDb = (1.0f / compRatio - 1.0f) * x * x / (2.0f * compKneeDb);
    }

    // Add makeup gain
    gainDb += compMakeupDb;

    // Asymmetric EMA smoothing (fast attack, slow release)
    // Frame period = FFT_SIZE / SAMPLE_RATE = 256/16000 = 16ms (~62.5 fps)
    // This is correct because hop size = FFT_SIZE (no overlap)
    static constexpr float framePeriod = (float)SpectralConstants::FFT_SIZE / SpectralConstants::SAMPLE_RATE;
    float attackAlpha = (compAttackTau > 0.0f) ? (1.0f - expf(-framePeriod / compAttackTau)) : 1.0f;
    float releaseAlpha = (compReleaseTau > 0.0f) ? (1.0f - expf(-framePeriod / compReleaseTau)) : 1.0f;

    float alpha = (gainDb < smoothedGainDb_) ? attackAlpha : releaseAlpha;
    smoothedGainDb_ += alpha * (gainDb - smoothedGainDb_);

    // Apply linear gain to all magnitudes
    float linearGain = powf(10.0f, smoothedGainDb_ / 20.0f);
    if (!safeIsFinite(linearGain)) linearGain = 1.0f;

    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        magnitudes_[i] *= linearGain;
    }
}

void SharedSpectralAnalysis::whitenMagnitudes() {
    // NOTE: Whitening modifies magnitudes_ in-place. Detectors requiring
    // absolute energy levels (HFC, ComplexDomain) must retune thresholds
    // if re-enabled after whitening is active.
    if (!whitenEnabled) return;

    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        float current = magnitudes_[i];

        // Update running max: max(current, decayed previous)
        float decayedMax = binRunningMax_[i] * whitenDecay;
        binRunningMax_[i] = (current > decayedMax) ? current : decayedMax;

        // Normalize by running max (with floor to avoid amplifying noise)
        float maxVal = (binRunningMax_[i] > whitenFloor) ? binRunningMax_[i] : whitenFloor;
        magnitudes_[i] = current / maxVal;
    }
}

void SharedSpectralAnalysis::computeDerivedFeatures() {
    // Total spectral energy
    float energy = 0.0f;
    float weightedSum = 0.0f;
    float magSum = 0.0f;

    for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {  // Skip DC
        float mag = magnitudes_[i];
        energy += mag * mag;
        weightedSum += i * mag;
        magSum += mag;
    }

    totalEnergy_ = safeIsFinite(energy) ? energy : 0.0f;

    // Spectral centroid (center of mass, in Hz)
    if (magSum > 0.0f && safeIsFinite(weightedSum)) {
        float centroidBin = weightedSum / magSum;
        spectralCentroid_ = centroidBin * SpectralConstants::BIN_FREQ_HZ;
    } else {
        spectralCentroid_ = 0.0f;
    }
}

void SharedSpectralAnalysis::savePreviousFrame() {
    // Called at the TOP of process(), before new FFT overwrites magnitudes_.
    // At this point magnitudes_ still holds the PREVIOUS frame's final state
    // (compressed + whitened), so prevMagnitudes_ gets the same processing
    // state as the upcoming frame's magnitudes_ will have after whitenMagnitudes().
    // This keeps spectral flux computations consistent (both frames whitened).
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevMagnitudes_[i] = magnitudes_[i];
    }
    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        prevMelBands_[i] = melBands_[i];
    }
}


