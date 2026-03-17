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
    writeIdx_ = 0;

    // Clear resonator state
    for (int i = 0; i < NUM_FILTERS; i++) {
        resonatorOutput_[i] = 0.0f;
        resonatorEnergy_[i] = 0.0f;
    }

    // Reset results
    peakBPM_ = 120.0f;
    peakConfidence_ = 0.0f;
    peakFilterIdx_ = NUM_FILTERS / 2;  // Start near middle (120 BPM)
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

    // 5. Compute confidence (peak-to-mean energy ratio)
    float totalEnergy = 0.0f;
    for (int i = 0; i < NUM_FILTERS; i++) {
        totalEnergy += resonatorEnergy_[i];
    }
    float meanEnergy = totalEnergy / NUM_FILTERS;
    float ratio = resonatorEnergy_[bestIdx] / (meanEnergy + 0.001f) - 1.0f;
    peakConfidence_ = ratio > 0.0f ? (ratio > 1.0f ? 1.0f : ratio) : 0.0f;
}
