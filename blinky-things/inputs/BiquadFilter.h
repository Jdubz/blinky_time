#pragma once
#include <math.h>

/**
 * Biquad Filter - 2nd order IIR filter using Direct Form II Transposed
 *
 * Used for frequency-selective onset detection (bass band filtering).
 * Direct Form II minimizes numerical issues on ARM Cortex-M4.
 *
 * Reference: Audio EQ Cookbook by Robert Bristow-Johnson
 */
class BiquadFilter {
public:
    // Filter coefficients
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    // Filter state (Direct Form II)
    float z1 = 0.0f, z2 = 0.0f;

    /**
     * Process a single sample through the filter
     */
    float process(float input) {
        float output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return output;
    }

    /**
     * Reset filter state (call when changing parameters)
     */
    void reset() {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    /**
     * Configure as lowpass filter
     * @param fc Cutoff frequency (Hz)
     * @param fs Sample rate (Hz)
     * @param Q Quality factor (0.5 = Butterworth, higher = sharper)
     */
    void setLowpass(float fc, float fs, float Q) {
        float w0 = 2.0f * 3.14159265f * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);
        float alpha = sinw0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = ((1.0f - cosw0) / 2.0f) / a0;
        b1 = (1.0f - cosw0) / a0;
        b2 = b0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;

        reset();
    }

    /**
     * Configure as highpass filter
     * @param fc Cutoff frequency (Hz)
     * @param fs Sample rate (Hz)
     * @param Q Quality factor
     */
    void setHighpass(float fc, float fs, float Q) {
        float w0 = 2.0f * 3.14159265f * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);
        float alpha = sinw0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = ((1.0f + cosw0) / 2.0f) / a0;
        b1 = -(1.0f + cosw0) / a0;
        b2 = b0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;

        reset();
    }

    /**
     * Configure as bandpass filter (constant skirt gain)
     * @param fc Center frequency (Hz)
     * @param fs Sample rate (Hz)
     * @param Q Quality factor (bandwidth = fc/Q)
     */
    void setBandpass(float fc, float fs, float Q) {
        float w0 = 2.0f * 3.14159265f * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);
        float alpha = sinw0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = alpha / a0;
        b1 = 0.0f;
        b2 = -alpha / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;

        reset();
    }
};
