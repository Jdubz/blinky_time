#pragma once
#include <math.h>

/**
 * Biquad Filter - 2nd order IIR filter using Direct Form II Transposed
 *
 * Used for frequency-selective onset detection (bass band filtering).
 * Direct Form II minimizes numerical issues on ARM Cortex-M4.
 *
 * SAFETY FEATURES:
 * - All setter methods validate parameters and return false on failure
 * - Division-by-zero protection at every division
 * - NaN/Inf detection prevents corruption propagation
 * - Passthrough fallback when filter setup fails
 * - State variables cleared on any NaN detection
 *
 * Reference: Audio EQ Cookbook by Robert Bristow-Johnson
 */
class BiquadFilter {
public:
    // Filter coefficients (passthrough by default: output = input)
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;

    // Filter state (Direct Form II)
    float z1 = 0.0f, z2 = 0.0f;

    /**
     * Process a single sample through the filter
     * Returns input unchanged if NaN/Inf detected
     */
    float process(float input) {
        // SAFETY: Reject NaN/Inf input
        if (!isfinite(input)) {
            input = 0.0f;
        }

        float output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;

        // SAFETY: Check for NaN/Inf in state, reset if corrupted
        if (!isfinite(z1) || !isfinite(z2) || !isfinite(output)) {
            reset();
            return input;  // Passthrough on corruption
        }

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
     * Set to passthrough mode (output = input)
     * Safe fallback when filter setup fails
     */
    void setPassthrough() {
        b0 = 1.0f;
        b1 = 0.0f;
        b2 = 0.0f;
        a1 = 0.0f;
        a2 = 0.0f;
        reset();
    }

    /**
     * Configure as lowpass filter
     * @param fc Cutoff frequency (Hz)
     * @param fs Sample rate (Hz)
     * @param Q Quality factor (0.707 = Butterworth, higher = sharper)
     * @return true if successful, false if parameters invalid (passthrough set)
     */
    bool setLowpass(float fc, float fs, float Q) {
        // SAFETY: Validate all parameters before any calculation
        if (fs <= 0.0f || Q <= 0.0f || fc <= 0.0f) {
            setPassthrough();
            return false;
        }

        // Nyquist limit: fc must be < fs/2
        if (fc >= fs * 0.5f) {
            fc = fs * 0.49f;  // Clamp to just below Nyquist
        }

        float w0 = 2.0f * 3.14159265f * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);

        // SAFETY: Check sinw0 before division
        if (fabsf(sinw0) < 1e-10f) {
            setPassthrough();
            return false;
        }

        float alpha = sinw0 / (2.0f * Q);

        // SAFETY: Check a0 before division
        float a0 = 1.0f + alpha;
        if (fabsf(a0) < 1e-10f) {
            setPassthrough();
            return false;
        }

        b0 = ((1.0f - cosw0) / 2.0f) / a0;
        b1 = (1.0f - cosw0) / a0;
        b2 = b0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;

        // SAFETY: Verify no NaN/Inf in coefficients
        if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) ||
            !isfinite(a1) || !isfinite(a2)) {
            setPassthrough();
            return false;
        }

        reset();
        return true;
    }

    /**
     * Configure as highpass filter
     * @param fc Cutoff frequency (Hz)
     * @param fs Sample rate (Hz)
     * @param Q Quality factor
     * @return true if successful, false if parameters invalid
     */
    bool setHighpass(float fc, float fs, float Q) {
        // SAFETY: Validate parameters
        if (fs <= 0.0f || Q <= 0.0f || fc <= 0.0f) {
            setPassthrough();
            return false;
        }

        if (fc >= fs * 0.5f) {
            fc = fs * 0.49f;
        }

        float w0 = 2.0f * 3.14159265f * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);

        if (fabsf(sinw0) < 1e-10f) {
            setPassthrough();
            return false;
        }

        float alpha = sinw0 / (2.0f * Q);
        float a0 = 1.0f + alpha;

        if (fabsf(a0) < 1e-10f) {
            setPassthrough();
            return false;
        }

        b0 = ((1.0f + cosw0) / 2.0f) / a0;
        b1 = -(1.0f + cosw0) / a0;
        b2 = b0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;

        if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) ||
            !isfinite(a1) || !isfinite(a2)) {
            setPassthrough();
            return false;
        }

        reset();
        return true;
    }

    /**
     * Configure as bandpass filter (constant skirt gain)
     * @param fc Center frequency (Hz)
     * @param fs Sample rate (Hz)
     * @param Q Quality factor (bandwidth = fc/Q)
     * @return true if successful, false if parameters invalid
     */
    bool setBandpass(float fc, float fs, float Q) {
        // SAFETY: Validate parameters
        if (fs <= 0.0f || Q <= 0.0f || fc <= 0.0f) {
            setPassthrough();
            return false;
        }

        if (fc >= fs * 0.5f) {
            fc = fs * 0.49f;
        }

        float w0 = 2.0f * 3.14159265f * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);

        if (fabsf(sinw0) < 1e-10f) {
            setPassthrough();
            return false;
        }

        float alpha = sinw0 / (2.0f * Q);
        float a0 = 1.0f + alpha;

        if (fabsf(a0) < 1e-10f) {
            setPassthrough();
            return false;
        }

        b0 = alpha / a0;
        b1 = 0.0f;
        b2 = -alpha / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;

        if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) ||
            !isfinite(a1) || !isfinite(a2)) {
            setPassthrough();
            return false;
        }

        reset();
        return true;
    }
};
