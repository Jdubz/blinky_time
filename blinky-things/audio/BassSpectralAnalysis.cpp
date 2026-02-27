#include "BassSpectralAnalysis.h"
#include <string.h>

BassSpectralAnalysis::BassSpectralAnalysis()
    : sampleBuffer_{}
    , writeIndex_(0)
    , bufferPrimed_(false)
    , newSampleCount_(0)
    , magnitudes_{}
    , prevMagnitudes_{}
    , binRunningMax_{}
    , smoothedGainDb_(0.0f)
    , cachedAttackAlpha_(1.0f)
    , cachedReleaseAlpha_(1.0f)
    , lastAttackTau_(0.0f)
    , lastReleaseTau_(0.0f)
    , goertzelCoeff_{}
    , hammingWindow_{}
    , frameReady_(false)
    , hasPrevFrame_(false)
{
}

void BassSpectralAnalysis::begin() {
    reset();
}

void BassSpectralAnalysis::reset() {
    writeIndex_ = 0;
    bufferPrimed_ = false;
    newSampleCount_ = 0;
    frameReady_ = false;
    hasPrevFrame_ = false;
    smoothedGainDb_ = 0.0f;

    memset(sampleBuffer_, 0, sizeof(sampleBuffer_));
    memset(magnitudes_, 0, sizeof(magnitudes_));
    memset(prevMagnitudes_, 0, sizeof(prevMagnitudes_));
    memset(binRunningMax_, 0, sizeof(binRunningMax_));

    // Precompute Goertzel coefficients: 2*cos(2*pi*k/N) for bins 1-12
    for (int b = 0; b < BassConstants::NUM_BASS_BINS; b++) {
        int k = b + BassConstants::FIRST_BIN;
        goertzelCoeff_[b] = 2.0f * cosf(2.0f * 3.14159265f * k / BassConstants::WINDOW_SIZE);
    }

    // Precompute Hamming window
    const float alpha = 0.54f;
    const float beta = 0.46f;
    const float twoPiOverN = 2.0f * 3.14159265f / (BassConstants::WINDOW_SIZE - 1);
    for (int i = 0; i < BassConstants::WINDOW_SIZE; i++) {
        hammingWindow_[i] = alpha - beta * cosf(twoPiOverN * i);
    }

    // Precompute compressor EMA alphas
    static constexpr float framePeriod = (float)BassConstants::HOP_SIZE / BassConstants::SAMPLE_RATE;
    cachedAttackAlpha_ = (compAttackTau > 0.0f) ? (1.0f - expf(-framePeriod / compAttackTau)) : 1.0f;
    cachedReleaseAlpha_ = (compReleaseTau > 0.0f) ? (1.0f - expf(-framePeriod / compReleaseTau)) : 1.0f;
    lastAttackTau_ = compAttackTau;
    lastReleaseTau_ = compReleaseTau;
}

bool BassSpectralAnalysis::addSamples(const int16_t* samples, int count) {
    if (!enabled) return false;

    for (int i = 0; i < count; i++) {
        sampleBuffer_[writeIndex_] = samples[i];
        writeIndex_ = (writeIndex_ + 1) % BassConstants::WINDOW_SIZE;
        newSampleCount_++;
    }
    if (!bufferPrimed_ && (newSampleCount_ >= BassConstants::WINDOW_SIZE)) {
        bufferPrimed_ = true;
    }

    return newSampleCount_ >= BassConstants::HOP_SIZE;
}

void BassSpectralAnalysis::process() {
    if (!enabled) return;
    if (newSampleCount_ < BassConstants::HOP_SIZE) return;

    // Need at least a full window of samples before first valid frame
    if (!bufferPrimed_) {
        newSampleCount_ = 0;
        return;
    }

    // Save previous magnitudes before overwriting
    savePreviousFrame();

    // Step 1: Extract 512 samples from ring buffer into windowed float buffer
    // Read oldest-first: writeIndex_ points to the oldest sample in the ring
    // Static to avoid 2KB stack allocation on every call (embedded target).
    // WARNING: Only one BassSpectralAnalysis instance may call process() — see header.
    static float windowed[BassConstants::WINDOW_SIZE];

    for (int i = 0; i < BassConstants::WINDOW_SIZE; i++) {
        int idx = (writeIndex_ + i) % BassConstants::WINDOW_SIZE;
        windowed[i] = (sampleBuffer_[idx] / 32768.0f) * hammingWindow_[i];
    }

    // Step 2: Goertzel for bins 1-12 (using precomputed coefficients)
    for (int b = 0; b < BassConstants::NUM_BASS_BINS; b++) {
        float mag = goertzelMagnitude(windowed, BassConstants::WINDOW_SIZE, goertzelCoeff_[b]);
        magnitudes_[b] = safeIsFinite(mag) ? mag : 0.0f;
    }

    // Step 3: Compressor
    applyCompressor();

    // Step 4: Whitening
    whitenMagnitudes();

    // Mark frame ready
    frameReady_ = true;
    hasPrevFrame_ = true;

    // Reset hop counter for next frame
    newSampleCount_ = 0;
}

float BassSpectralAnalysis::goertzelMagnitude(const float* windowedSamples, int N, float coeff) {
    // Goertzel algorithm: computes |DFT[k]| for a single bin
    // coeff = 2*cos(2*pi*k/N), precomputed in reset()
    float s1 = 0.0f, s2 = 0.0f;

    for (int i = 0; i < N; i++) {
        float s0 = windowedSamples[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    // |X[k]|^2 = s1^2 + s2^2 - coeff*s1*s2
    float magSq = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return sqrtf(magSq > 0.0f ? magSq : 0.0f);
}

void BassSpectralAnalysis::savePreviousFrame() {
    for (int i = 0; i < BassConstants::NUM_BASS_BINS; i++) {
        prevMagnitudes_[i] = magnitudes_[i];
    }
}

void BassSpectralAnalysis::applyCompressor() {
    if (!compressorEnabled) {
        smoothedGainDb_ *= 0.9f;
        return;
    }

    // Compute RMS over bass bins
    float sumSq = 0.0f;
    for (int i = 0; i < BassConstants::NUM_BASS_BINS; i++) {
        sumSq += magnitudes_[i] * magnitudes_[i];
    }
    float rms = sqrtf(sumSq / BassConstants::NUM_BASS_BINS);

    const float floorLin = 1e-10f;
    if (rms < floorLin) rms = floorLin;
    float rmsDb = 20.0f * log10f(rms);

    // Soft-knee gain computation (same as SharedSpectralAnalysis)
    float gainDb = 0.0f;
    float halfKnee = compKneeDb * 0.5f;
    float diff = rmsDb - compThresholdDb;

    if (diff <= -halfKnee) {
        gainDb = 0.0f;
    } else if (diff >= halfKnee) {
        gainDb = (1.0f - 1.0f / compRatio) * (compThresholdDb - rmsDb);
    } else {
        float x = diff + halfKnee;
        gainDb = (1.0f / compRatio - 1.0f) * x * x / (2.0f * compKneeDb);
    }

    gainDb += compMakeupDb;

    // Asymmetric EMA smoothing (alphas precomputed in reset(), recomputed if tau changed)
    if (compAttackTau != lastAttackTau_ || compReleaseTau != lastReleaseTau_) {
        static constexpr float framePeriod = (float)BassConstants::HOP_SIZE / BassConstants::SAMPLE_RATE;
        cachedAttackAlpha_ = (compAttackTau > 0.0f) ? (1.0f - expf(-framePeriod / compAttackTau)) : 1.0f;
        cachedReleaseAlpha_ = (compReleaseTau > 0.0f) ? (1.0f - expf(-framePeriod / compReleaseTau)) : 1.0f;
        lastAttackTau_ = compAttackTau;
        lastReleaseTau_ = compReleaseTau;
    }

    float alpha = (gainDb < smoothedGainDb_) ? cachedAttackAlpha_ : cachedReleaseAlpha_;
    smoothedGainDb_ += alpha * (gainDb - smoothedGainDb_);

    float linearGain = powf(10.0f, smoothedGainDb_ / 20.0f);
    if (!safeIsFinite(linearGain)) linearGain = 1.0f;

    for (int i = 0; i < BassConstants::NUM_BASS_BINS; i++) {
        magnitudes_[i] *= linearGain;
    }
}

void BassSpectralAnalysis::whitenMagnitudes() {
    if (!whitenEnabled) return;

    for (int i = 0; i < BassConstants::NUM_BASS_BINS; i++) {
        float current = magnitudes_[i];

        float decayedMax = binRunningMax_[i] * whitenDecay;
        binRunningMax_[i] = (current > decayedMax) ? current : decayedMax;

        float maxVal = (binRunningMax_[i] > whitenFloor) ? binRunningMax_[i] : whitenFloor;
        magnitudes_[i] = current / maxVal;
    }
}
