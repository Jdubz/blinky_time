#include "SharedSpectralAnalysis.h"
#include "../types/BlinkyAssert.h"
#include <math.h>

// CMSIS-DSP Radix-4 FFT — ~2x faster than ArduinoFFT's Radix-2 on Cortex-M4F.
// The library is already linked by the Seeeduino BSP (-larm_cortexM4lf_math).
#ifdef BLINKY_PLATFORM_NRF52840
#include <arm_math.h>
static arm_rfft_fast_instance_f32 rfftInstance;
static bool rfftInitialized = false;
#else
#include <arduinoFFT.h>
#endif

// Band-weighted spectral flux constants.
// Weights emphasize rhythmically informative frequencies for BPM estimation.
// Each band's raw flux is normalized by its bin count before weighting,
// so these weights represent actual emphasis (not bin-count-scaled contribution).
// Band flux weights moved to SharedSpectralAnalysis public members (v74) for tuning.
// Previous hardcoded values: BASS=0.5, MID=0.2, HIGH=0.3
static constexpr float BASS_BIN_COUNT = static_cast<float>(
    SpectralConstants::BASS_MAX_BIN - SpectralConstants::BASS_MIN_BIN + 1);  // 6
static constexpr float MID_BIN_COUNT = static_cast<float>(
    SpectralConstants::MID_MAX_BIN - SpectralConstants::MID_MIN_BIN + 1);    // 26
static constexpr float HIGH_BIN_COUNT = static_cast<float>(
    SpectralConstants::NUM_BINS - SpectralConstants::HIGH_MIN_BIN);           // 95

// Mel filterbank bin boundaries: 30 bands, 40-4000 Hz, 16 kHz / FFT-256.
// Focused on kick (40-200 Hz, 3 bands) + snare (200-4000 Hz, 27 bands).
// Hi-hat frequencies (4-8 kHz) excluded — saves bandwidth for onset-relevant bands.
// MUST match librosa.filters.mel(sr=16000, n_fft=256, n_mels=30,
//   fmin=40, fmax=4000, htk=True, norm=None).
static const MelBandDef MEL_BANDS[SpectralConstants::NUM_MEL_BANDS] = {
    {  1,   1,   2},  // 0: 85 Hz center
    {  1,   2,   3},  // 1: 134 Hz center
    {  2,   3,   4},  // 2: 185 Hz center
    {  3,   4,   5},  // 3: 239 Hz center
    {  4,   5,   6},  // 4: 297 Hz center
    {  5,   6,   7},  // 5: 358 Hz center
    {  6,   7,   8},  // 6: 423 Hz center
    {  7,   8,   9},  // 7: 492 Hz center
    {  8,   9,  10},  // 8: 566 Hz center
    {  9,  10,  12},  // 9: 643 Hz center
    { 10,  12,  13},  // 10: 726 Hz center
    { 12,  13,  15},  // 11: 814 Hz center
    { 13,  15,  16},  // 12: 907 Hz center
    { 15,  16,  18},  // 13: 1005 Hz center
    { 16,  18,  20},  // 14: 1110 Hz center
    { 18,  20,  21},  // 15: 1221 Hz center
    { 20,  21,  23},  // 16: 1339 Hz center
    { 21,  23,  26},  // 17: 1465 Hz center
    { 23,  26,  28},  // 18: 1598 Hz center
    { 26,  28,  30},  // 19: 1739 Hz center
    { 28,  30,  33},  // 20: 1889 Hz center
    { 30,  33,  35},  // 21: 2048 Hz center
    { 33,  35,  38},  // 22: 2217 Hz center
    { 35,  38,  41},  // 23: 2396 Hz center
    { 38,  41,  45},  // 24: 2586 Hz center
    { 41,  45,  48},  // 25: 2788 Hz center
    { 45,  48,  52},  // 26: 3003 Hz center
    { 48,  52,  56},  // 27: 3230 Hz center
    { 52,  56,  60},  // 28: 3472 Hz center
    { 56,  60,  64},  // 29: 3728 Hz center
};

SharedSpectralAnalysis::SharedSpectralAnalysis()
    : sampleBuffer_{}
    , sampleCount_(0)
    , writeIndex_(0)
    , vReal_{}
    , vImag_{}
    , magnitudes_{}
    , preWhitenMagnitudes_{}
    , phases_{}
    , prevMagnitudes_{}
    , prevRawMagnitudes_{}
    , melBands_{}
    , rawMelBands_{}
    , linearMelBands_{}
    , linearMelInvWeightSum_{}
    , melRunningMax_{}
    , binRunningMax_{}
    , smoothedGainDb_(0.0f)
    , frameRmsDb_(-200.0f)
    , totalEnergy_(0.0f)
    , spectralCentroid_(0.0f)
    , spectralFlux_(0.0f)
    , spectralFlatness_(0.0f)
    , bassFlux_(0.0f)
    , rawSpectralFlux_(0.0f)
    , rawCentroid_(0.0f)
    , rawCrest_(0.0f)
    , rawRolloff_(0.0f)
    , rawHFC_(0.0f)
    , frameReady_(false)
    , hasPrevFrame_(false)
    , frameCount_(0)
{
}

void SharedSpectralAnalysis::begin() {
    // Precompute inverse weight sums for linear mel band computation (PCEN).
    // Weights are constant (triangular filterbank from MEL_BANDS[]).
    for (int band = 0; band < SpectralConstants::NUM_MEL_BANDS; band++) {
        const MelBandDef& def = MEL_BANDS[band];
        float weightSum = 0.0f;
        for (int bin = def.startBin; bin <= def.endBin && bin < SpectralConstants::NUM_BINS; bin++) {
            float weight;
            if (bin <= def.centerBin) {
                weight = (def.centerBin > def.startBin)
                    ? (float)(bin - def.startBin) / (def.centerBin - def.startBin) : 1.0f;
            } else {
                weight = (def.endBin > def.centerBin)
                    ? 1.0f - (float)(bin - def.centerBin) / (def.endBin - def.centerBin) : 1.0f;
            }
            weightSum += weight;
        }
        linearMelInvWeightSum_[band] = (weightSum > 0) ? 1.0f / weightSum : 0.0f;
    }
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
        preWhitenMagnitudes_[i] = 0.0f;
        phases_[i] = 0.0f;
        prevMagnitudes_[i] = 0.0f;
        prevRawMagnitudes_[i] = 0.0f;
    }
    for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
        melBands_[i] = 0.0f;
        rawMelBands_[i] = 0.0f;
        melRunningMax_[i] = 0.0f;
    }
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        binRunningMax_[i] = 0.0f;
    }
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        smoothedPower_[i] = 0.0f;
        noiseFloorEst_[i] = 0.0f;
    }
    smoothedGainDb_ = 0.0f;
    frameRmsDb_ = -200.0f;
    spectralFlux_ = 0.0f;
    bassFlux_ = 0.0f;
    rawSpectralFlux_ = 0.0f;
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

    // Noise floor estimation + spectral subtraction (Martin 2001)
    // Applied BEFORE preWhitenMagnitudes snapshot so both BandFlux and NN paths
    // see noise-subtracted magnitudes. This removes gain-dependent MEMS noise.
    estimateAndSubtractNoise();

    // Save noise-subtracted magnitudes BEFORE compressor/whitening for detectors
    // that handle their own normalization (BandFlux uses log(1+gamma*mag))
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        preWhitenMagnitudes_[i] = magnitudes_[i];
    }

    // Compute raw mel bands from pre-compressor magnitudes (for NN inference + calibration).
    // Must happen BEFORE applyCompressor() modifies magnitudes_ in-place.
    computeRawMelBands();

    // Compute raw SuperFlux spectral flux from pre-compressor magnitudes.
    // This matches the training pipeline's STFT-based flux (no compressor gain).
    // Used exclusively for NN hybrid input — ACF tempo uses compressed flux below.
    //
    // NOTE: keep this loop structurally in sync with the compressed flux loop
    // below (same 3-wide max filter, same bass/mid/high split, same weighting).
    // If you change one, update the other or the NN input will drift from what
    // ACF uses for tempo analysis.
    if (hasPrevFrame_) {
        float rawBassFlux = 0.0f, rawMidFlux = 0.0f, rawHighFlux = 0.0f;
        for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {
            float ref = prevRawMagnitudes_[i];
            if (i > 1) ref = fmaxf(ref, prevRawMagnitudes_[i - 1]);
            if (i < SpectralConstants::NUM_BINS - 1) ref = fmaxf(ref, prevRawMagnitudes_[i + 1]);
            float diff = preWhitenMagnitudes_[i] - ref;
            if (diff > 0.0f) {
                if (i <= SpectralConstants::BASS_MAX_BIN)
                    rawBassFlux += diff;
                else if (i <= SpectralConstants::MID_MAX_BIN)
                    rawMidFlux += diff;
                else
                    rawHighFlux += diff;
            }
        }
        rawSpectralFlux_ = bassFluxWeight * (rawBassFlux / BASS_BIN_COUNT)
                         + midFluxWeight * (rawMidFlux / MID_BIN_COUNT)
                         + highFluxWeight * (rawHighFlux / HIGH_BIN_COUNT);
    } else {
        rawSpectralFlux_ = 0.0f;
    }
    // Save pre-compressor magnitudes for next frame's raw flux computation
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevRawMagnitudes_[i] = preWhitenMagnitudes_[i];
    }

    // Phase 2a shape features (centroid, crest, rolloff, HFC) on pre-compressor
    // magnitudes. Must run BEFORE applyCompressor() so values match the Python
    // reference (ml-training/analysis/features.py), which computes on raw STFT.
    computeShapeFeaturesRaw();

    // Frame-level soft-knee compression (normalizes gross signal level)
    applyCompressor();

    // Compute derived features (energy, centroid) from compressed magnitudes
    // NOTE: totalEnergy_ and spectralCentroid_ reflect compressed-but-not-whitened
    // magnitudes. getMagnitudes() returns whitened values after whitenMagnitudes() below.
    computeDerivedFeatures();

    // Compute band-weighted spectral flux from compressed-but-not-whitened magnitudes.
    // Using compressed (not whitened) magnitudes preserves absolute transient contrast.
    // Per-bin whitening (running max, 5s decay) would normalize away the peaks
    // that ACF needs for periodicity detection.
    //
    // Band weighting emphasizes rhythmically important frequencies:
    //   Bass (bins 1-6, 62-375 Hz): kicks — strongest periodic signal
    //   High (bins 33-127, 2-8 kHz): snares, hi-hats — transient markers
    //   Mid (bins 7-32, 437-2000 Hz): vocals, pads — less rhythmic
    if (hasPrevFrame_) {
        float bassFlux = 0.0f, midFlux = 0.0f, highFlux = 0.0f;
        for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {  // skip DC
            // SuperFlux (Bock & Widmer 2013): 3-wide frequency max filter on
            // previous frame absorbs slow harmonic shifts (vibrato, chord
            // changes) into the reference. Only sharp broadband transients
            // (kicks, snares) produce positive flux.
            float ref = prevMagnitudes_[i];
            if (i > 1) ref = fmaxf(ref, prevMagnitudes_[i - 1]);
            if (i < SpectralConstants::NUM_BINS - 1) ref = fmaxf(ref, prevMagnitudes_[i + 1]);
            float diff = magnitudes_[i] - ref;
            if (diff > 0.0f) {
                if (i <= SpectralConstants::BASS_MAX_BIN)
                    bassFlux += diff;
                else if (i <= SpectralConstants::MID_MAX_BIN)
                    midFlux += diff;
                else
                    highFlux += diff;
            }
        }
        // Normalize each band by its bin count so the weights reflect
        // actual emphasis rather than bin-count dominance (bass=6, mid=26,
        // high=95 bins — without normalization, high band would dominate).
        bassFlux_ = bassFlux / BASS_BIN_COUNT;
        float normMidFlux = midFlux / MID_BIN_COUNT;
        float normHighFlux = highFlux / HIGH_BIN_COUNT;
        spectralFlux_ = bassFluxWeight * bassFlux_
                       + midFluxWeight * normMidFlux
                       + highFluxWeight * normHighFlux;
    } else {
        spectralFlux_ = 0.0f;
        bassFlux_ = 0.0f;
    }

    // Save compressed magnitudes for next frame's flux computation.
    // Must happen AFTER flux computation, BEFORE whitenMagnitudes().
    savePrevCompressedMagnitudes();

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
    frameCount_++;
    hasPrevFrame_ = true;

    // Reset sample buffer for next frame
    sampleCount_ = 0;
}

// Precomputed Hamming window (avoids cosf() per sample every frame)
static float hammingWindow_[SpectralConstants::FFT_SIZE];
static bool hammingInitialized_ = false;

static void initHammingWindow() {
    if (hammingInitialized_) return;
    const float alpha = 0.54f;
    const float beta = 0.46f;
    const float twoPiOverN = 2.0f * 3.14159265f / (SpectralConstants::FFT_SIZE - 1);
    for (int i = 0; i < SpectralConstants::FFT_SIZE; i++) {
        hammingWindow_[i] = alpha - beta * cosf(twoPiOverN * i);
    }
    hammingInitialized_ = true;
}

void SharedSpectralAnalysis::applyHammingWindow() {
    initHammingWindow();
    for (int i = 0; i < SpectralConstants::FFT_SIZE; i++) {
        vReal_[i] *= hammingWindow_[i];
    }
}

void SharedSpectralAnalysis::computeFFT() {
#ifdef BLINKY_PLATFORM_NRF52840
    // CMSIS-DSP Radix-4 real FFT — hardware-optimized for Cortex-M4F.
    // arm_rfft_fast_f32 operates in-place: input is real samples in vReal_,
    // output is interleaved complex [Re0, Re(N/2), Re1, Im1, Re2, Im2, ...].
    if (!rfftInitialized) {
        arm_rfft_fast_init_f32(&rfftInstance, SpectralConstants::FFT_SIZE);
        rfftInitialized = true;
    }
    // rfft needs a separate output buffer (cannot be truly in-place for real FFT)
    float fftOutput[SpectralConstants::FFT_SIZE];
    arm_rfft_fast_f32(&rfftInstance, vReal_, fftOutput, 0);

    // Unpack interleaved complex output into separate real/imag arrays.
    // CMSIS format: [DC_real, Nyquist_real, Re1, Im1, Re2, Im2, ...]
    vReal_[0] = fftOutput[0];   // DC component (real only)
    vImag_[0] = 0.0f;
    for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {
        vReal_[i] = fftOutput[2 * i];
        vImag_[i] = fftOutput[2 * i + 1];
    }
#else
    // Fallback: ArduinoFFT (ESP32-S3 and other platforms)
    ArduinoFFT<float> fft(vReal_, vImag_, SpectralConstants::FFT_SIZE,
                          SpectralConstants::SAMPLE_RATE);
    fft.compute(FFTDirection::Forward);
#endif
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

void SharedSpectralAnalysis::estimateAndSubtractNoise() {
    if (!noiseEstEnabled) return;

    for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {  // Skip DC
        float mag = magnitudes_[i];
        float power = mag * mag;

        // Smooth power estimate (exponential moving average)
        smoothedPower_[i] = noiseSmoothAlpha * smoothedPower_[i]
                          + (1.0f - noiseSmoothAlpha) * power;

        // Noise floor: tracks minimum of smoothed power with slow release
        // Instant attack to new minimums, exponential release upward
        if (smoothedPower_[i] < noiseFloorEst_[i] || noiseFloorEst_[i] == 0.0f) {
            noiseFloorEst_[i] = smoothedPower_[i];
        } else {
            noiseFloorEst_[i] = noiseReleaseFactor * noiseFloorEst_[i]
                              + (1.0f - noiseReleaseFactor) * smoothedPower_[i];
        }

        // Spectral subtraction: remove estimated noise magnitude
        float noiseMag = noiseOversubtract * sqrtf(noiseFloorEst_[i]);
        float floor = noiseFloorRatio * mag;  // Spectral floor prevents zero-out
        float clean = mag - noiseMag;
        magnitudes_[i] = (clean > floor) ? clean : floor;
    }
}

void SharedSpectralAnalysis::computeMelBandsFrom(const float* inputMagnitudes, float* outputMelBands) {
    // Apply triangular mel filterbank to magnitude spectrum and log-compress.
    // Shared implementation for both compressed (melBands_) and raw (rawMelBands_) paths.

    for (int band = 0; band < SpectralConstants::NUM_MEL_BANDS; band++) {
        const MelBandDef& def = MEL_BANDS[band];
        float sum = 0.0f;
        float weightSum = 0.0f;

        // Rising edge: start to center
        // Note: band 0 has startBin==centerBin (degenerate). The guard
        // (centerBin > startBin) falls through to weight=1.0f, giving the
        // single center bin full weight. No division by zero.
        for (int bin = def.startBin; bin <= def.centerBin && bin < SpectralConstants::NUM_BINS; bin++) {
            float weight = (def.centerBin > def.startBin)
                ? (float)(bin - def.startBin) / (def.centerBin - def.startBin)
                : 1.0f;
            sum += inputMagnitudes[bin] * weight;
            weightSum += weight;
        }

        // Falling edge: center to end
        for (int bin = def.centerBin + 1; bin <= def.endBin && bin < SpectralConstants::NUM_BINS; bin++) {
            float weight = (def.endBin > def.centerBin)
                ? 1.0f - (float)(bin - def.centerBin) / (def.endBin - def.centerBin)
                : 1.0f;
            sum += inputMagnitudes[bin] * weight;
            weightSum += weight;
        }

        float bandEnergy = (weightSum > 0) ? sum / weightSum : 0.0f;

        // Special-case silence to ensure mel bands are truly zero.
        // Note: this 1e-6 threshold is not replicated in the Python training
        // pipeline, but only affects near-silent frames (negligible impact).
        const float silenceThreshold = 1e-6f;
        if (bandEnergy < silenceThreshold) {
            outputMelBands[band] = 0.0f;
            continue;
        }

        // Log compression: map [-MEL_DB_RANGE, 0] dB to [0, 1]
        const float epsilon = 1e-7f;  // Above ARM Cortex-M4 denormal range
        float logEnergy = 10.0f * log10f(bandEnergy + epsilon);
        logEnergy = (logEnergy + SpectralConstants::MEL_DB_RANGE)
                  / SpectralConstants::MEL_DB_RANGE;

        outputMelBands[band] = safeIsFinite(logEnergy) ? clamp01(logEnergy) : 0.0f;
    }
}

void SharedSpectralAnalysis::computeMelBands() {
    computeMelBandsFrom(magnitudes_, melBands_);
}

void SharedSpectralAnalysis::computeRawMelBands() {
    // Compute mel bands from raw (pre-compressor) magnitudes.
    // Uses preWhitenMagnitudes_ saved before applyCompressor().
    computeMelBandsFrom(preWhitenMagnitudes_, rawMelBands_);

    // Also store linear mel energy (before log compression) for PCEN.
    // Uses precomputed invWeightSum_ to avoid per-frame weight sum + division.
    for (int band = 0; band < SpectralConstants::NUM_MEL_BANDS; band++) {
        const MelBandDef& def = MEL_BANDS[band];
        float sum = 0.0f;
        for (int bin = def.startBin; bin <= def.endBin && bin < SpectralConstants::NUM_BINS; bin++) {
            float weight;
            if (bin <= def.centerBin) {
                weight = (def.centerBin > def.startBin)
                    ? (float)(bin - def.startBin) / (def.centerBin - def.startBin) : 1.0f;
            } else {
                weight = (def.endBin > def.centerBin)
                    ? 1.0f - (float)(bin - def.centerBin) / (def.endBin - def.centerBin) : 1.0f;
            }
            sum += preWhitenMagnitudes_[bin] * weight;
        }
        linearMelBands_[band] = sum * linearMelInvWeightSum_[band];
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

        // Bass bypass: skip whitening for bins 1-6 (62-375 Hz) to preserve kick contrast
        if (whitenBassBypass && i >= SpectralConstants::BASS_MIN_BIN && i <= SpectralConstants::BASS_MAX_BIN) {
            continue;  // Keep compressed-only magnitude for bass bins
        }

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

    // Spectral flatness (Wiener entropy): geometric_mean / arithmetic_mean.
    // Range 0-1: 0 = pure tone (energy in one bin), 1 = white noise (equal energy).
    // Drums are noise-like (broadband transient), pitched instruments are tonal.
    // Used as a deterministic NN input feature for drum-vs-harmonic discrimination.
    // Both means use the same set of bins (mag > 1e-10, skipping DC) for consistency.
    // Uses logf/expf instead of log10f/powf — same result (log base cancels in
    // geo/ari ratio) but ~2-3× faster on Cortex-M4F (no hardware power instruction).
    float logSum = 0.0f;
    float flatMagSum = 0.0f;
    int validBins = 0;
    for (int i = 1; i < SpectralConstants::NUM_BINS; i++) {
        float mag = magnitudes_[i];
        if (mag > 1e-10f) {
            logSum += logf(mag);
            flatMagSum += mag;
            validBins++;
        }
    }
    if (validBins > 0 && flatMagSum > 1e-10f) {
        float geoMean = expf(logSum / validBins);
        float ariMean = flatMagSum / validBins;
        float flat = geoMean / ariMean;
        spectralFlatness_ = (flat < 0.0f) ? 0.0f : (flat > 1.0f) ? 1.0f : flat;
    } else {
        spectralFlatness_ = 0.0f;
    }
}

void SharedSpectralAnalysis::computeShapeFeaturesRaw() {
    // Phase 2a: four "shape" features computed from preWhitenMagnitudes_ to
    // match the Python reference in ml-training/analysis/features.py. Uses
    // bin indices 1..NUM_BINS-1 (DC skipped), same framing as Python.
    //
    // Centroid   = Σ(k · |X[k]|) / Σ|X[k]|              — bin-index weighted
    // Crest      = max|X[k]| / sqrt(Σ|X[k]|² / N)       — peak / RMS
    // HFC        = Σ(k · |X[k]|²)                       — Masri 1996
    // Rolloff85  = smallest k where Σ|X[0..k]|² ≥ 0.85 · total_energy
    //
    // All four scan the same 127 bins once (skipping DC), so total cost is a
    // single pass with simple accumulators — well under the 0.5 ms target.
    constexpr int FIRST = 1;  // skip DC
    constexpr int LAST = SpectralConstants::NUM_BINS;
    constexpr int N = LAST - FIRST;  // 127 for NUM_BINS=128

    float weightedSum = 0.0f;   // Σ k · |X|
    float magSum = 0.0f;        // Σ |X|
    float energySum = 0.0f;     // Σ |X|²
    float hfcSum = 0.0f;        // Σ k · |X|²
    float peakMag = 0.0f;       // max |X|
    for (int i = FIRST; i < LAST; i++) {
        float mag = preWhitenMagnitudes_[i];
        float e = mag * mag;
        weightedSum += i * mag;
        magSum += mag;
        energySum += e;
        hfcSum += i * e;
        if (mag > peakMag) peakMag = mag;
    }

    rawCentroid_ = (magSum > 1e-10f && safeIsFinite(weightedSum))
        ? (weightedSum / magSum)
        : 0.0f;

    if (energySum > 1e-10f && safeIsFinite(energySum)) {
        float rms = sqrtf(energySum / static_cast<float>(N));
        rawCrest_ = (rms > 1e-10f) ? (peakMag / rms) : 0.0f;
    } else {
        rawCrest_ = 0.0f;
    }

    rawHFC_ = safeIsFinite(hfcSum) ? hfcSum : 0.0f;

    // Rolloff: first bin where cumulative |X|² ≥ 0.85 * total energy.
    // Second pass — cheap since we know energySum; avoids a temp cumulative buffer.
    if (energySum > 1e-10f) {
        float threshold = 0.85f * energySum;
        float cum = 0.0f;
        int rolloffBin = 0;
        for (int i = FIRST; i < LAST; i++) {
            float e = preWhitenMagnitudes_[i] * preWhitenMagnitudes_[i];
            cum += e;
            if (cum >= threshold) {
                rolloffBin = i - FIRST;  // Python indexing: 0..N-1 excluding DC
                break;
            }
        }
        rawRolloff_ = static_cast<float>(rolloffBin);
    } else {
        rawRolloff_ = 0.0f;
    }
}

void SharedSpectralAnalysis::savePrevCompressedMagnitudes() {
    // Save compressed-but-not-whitened magnitudes for spectral flux computation.
    // Called AFTER applyCompressor(), BEFORE whitenMagnitudes().
    // Flux = current_compressed - prev_compressed, so both frames are in the
    // same domain (compressor-normalized but not whitened). This preserves
    // absolute transient contrast that per-bin whitening would remove.
    for (int i = 0; i < SpectralConstants::NUM_BINS; i++) {
        prevMagnitudes_[i] = magnitudes_[i];
    }
}


