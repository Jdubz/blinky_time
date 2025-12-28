#include "SpectralFlux.h"
#include <arduinoFFT.h>
#include <math.h>

using namespace SpectralFluxConstants;

SpectralFlux::SpectralFlux()
    : sampleCount_(0)
    , writeIndex_(0)
    , currentFlux_(0.0f)
    , averageFlux_(0.0f)
    , hasPrevFrame_(false)
    , minBin_(DEFAULT_MIN_BIN)
    , maxBin_(DEFAULT_MAX_BIN)
{
}

void SpectralFlux::begin() {
    reset();
}

void SpectralFlux::reset() {
    sampleCount_ = 0;
    writeIndex_ = 0;
    currentFlux_ = 0.0f;
    averageFlux_ = 0.0f;
    hasPrevFrame_ = false;

    // Clear buffers
    for (int i = 0; i < FFT_SIZE; i++) {
        sampleBuffer_[i] = 0;
        vReal_[i] = 0.0f;
        vImag_[i] = 0.0f;
    }
    for (int i = 0; i < NUM_BINS; i++) {
        prevMagnitude_[i] = 0.0f;
    }
}

bool SpectralFlux::addSamples(const int16_t* samples, int count) {
    for (int i = 0; i < count && sampleCount_ < FFT_SIZE; i++) {
        sampleBuffer_[writeIndex_] = samples[i];
        writeIndex_ = (writeIndex_ + 1) % FFT_SIZE;
        sampleCount_++;
    }
    return sampleCount_ >= FFT_SIZE;
}

void SpectralFlux::applyHammingWindow() {
    // Hamming window: w(n) = 0.54 - 0.46 * cos(2πn/(N-1))
    // Pre-computed coefficients would be faster but use more memory
    const float alpha = 0.54f;
    const float beta = 0.46f;
    const float twoPiOverN = 2.0f * 3.14159265f / (FFT_SIZE - 1);

    for (int i = 0; i < FFT_SIZE; i++) {
        float window = alpha - beta * cosf(twoPiOverN * i);
        vReal_[i] *= window;
    }
}

void SpectralFlux::computeMagnitudes() {
    // Compute magnitude for each frequency bin
    // Only need first half (bins 0 to NUM_BINS-1) due to symmetry
    for (int i = 0; i < NUM_BINS; i++) {
        float real = vReal_[i];
        float imag = vImag_[i];

        // SAFETY: Check for NaN/Inf from FFT output
        if (!isfinite(real)) real = 0.0f;
        if (!isfinite(imag)) imag = 0.0f;

        // Store in vReal_ temporarily (we'll copy to prevMagnitude_ after flux calc)
        float mag = sqrtf(real * real + imag * imag);

        // SAFETY: Ensure magnitude is valid
        vReal_[i] = isfinite(mag) ? mag : 0.0f;
    }
}

float SpectralFlux::computeFlux() {
    // Half-wave rectified spectral flux:
    // Only count positive differences (increases in energy)
    // This detects onsets better than full flux (which also catches offsets)
    float flux = 0.0f;

    int minB = (minBin_ < 0) ? 0 : minBin_;
    int maxB = (maxBin_ > NUM_BINS) ? NUM_BINS : maxBin_;

    for (int i = minB; i < maxB; i++) {
        float diff = vReal_[i] - prevMagnitude_[i];
        if (diff > 0.0f && isfinite(diff)) {
            flux += diff;
        }
    }

    // Normalize by number of bins analyzed
    int numBins = maxB - minB;
    if (numBins > 0) {
        flux /= numBins;
    }

    // SAFETY: Final NaN/Inf check
    return isfinite(flux) ? flux : 0.0f;
}

float SpectralFlux::process() {
    if (sampleCount_ < FFT_SIZE) {
        return 0.0f;  // Not enough samples
    }

    // Copy samples to vReal_, starting from the oldest sample in ring buffer
    // Since we always fill exactly FFT_SIZE samples before processing,
    // writeIndex_ points to the oldest sample
    for (int i = 0; i < FFT_SIZE; i++) {
        int idx = (writeIndex_ + i) % FFT_SIZE;
        // Normalize int16 to float (-1.0 to 1.0)
        vReal_[i] = sampleBuffer_[idx] / 32768.0f;
        vImag_[i] = 0.0f;
    }

    // Apply Hamming window
    applyHammingWindow();

    // Compute FFT in place
    // ArduinoFFT requires buffer references at construction time
    // We create a temporary instance each frame (small overhead, but safe)
    ArduinoFFT<float> fft(vReal_, vImag_, FFT_SIZE, 16000.0f);
    fft.compute(FFTDirection::Forward);

    // Compute magnitudes (stored back in vReal_[0..NUM_BINS-1])
    computeMagnitudes();

    // Compute spectral flux
    if (hasPrevFrame_) {
        currentFlux_ = computeFlux();

        // Update running average (exponential moving average, ~0.5s time constant at 60fps)
        const float alpha = 0.03f;  // ~33 frames to reach 63%
        averageFlux_ += alpha * (currentFlux_ - averageFlux_);

        // SAFETY: Reset if averageFlux becomes corrupted
        if (!isfinite(averageFlux_)) {
            averageFlux_ = 0.0f;
        }
    } else {
        currentFlux_ = 0.0f;
        hasPrevFrame_ = true;
    }

    // Save current magnitudes for next frame
    for (int i = 0; i < NUM_BINS; i++) {
        prevMagnitude_[i] = vReal_[i];
    }

    // Reset sample buffer for next frame
    sampleCount_ = 0;
    // writeIndex_ continues from where it was (ring buffer style)

    return currentFlux_;
}

void SpectralFlux::setAnalysisRange(int minBin, int maxBin) {
    minBin_ = (minBin < 0) ? 0 : minBin;
    maxBin_ = (maxBin > NUM_BINS) ? NUM_BINS : maxBin;
    if (minBin_ >= maxBin_) {
        minBin_ = DEFAULT_MIN_BIN;
        maxBin_ = DEFAULT_MAX_BIN;
    }
}
