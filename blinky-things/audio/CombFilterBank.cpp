#include "CombFilterBank.h"
#include <math.h>

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
