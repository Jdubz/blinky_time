#pragma once

/**
 * PerceptualScaling - Automatic perceptual compression for loud environments
 *
 * Provides logarithmic and power-law compression that activates automatically
 * in loud mode to preserve dynamic range when hardware gain is at minimum.
 *
 * In normal mode: linear passthrough (no overhead)
 * In loud mode: logarithmic compression to prevent saturation
 *
 * Memory: 16 bytes (4 float parameters)
 */
class PerceptualScaling {
public:
    // === TUNING PARAMETERS ===

    // Logarithmic compression (used for energy/pulse in loud mode)
    float logCompressionK = 6.0f;      // Compression strength (1-10, higher = more aggressive)
    float logMakeupGain = 1.2f;        // Post-compression makeup gain

    // Power law compression (reserved for future use)
    float powerExponent = 0.75f;       // Power law exponent (0.5-0.9, lower = more compression)
    float powerMakeupGain = 1.3f;      // Power makeup gain

    // === MAIN SCALING ===

    /**
     * Scale a value with automatic compression based on loud mode
     * @param raw Raw input value (typically 0-1+ range)
     * @param loudMode True to apply compression, false for linear passthrough
     * @return Scaled value (0-1 range)
     */
    float scale(float raw, bool loudMode);

    /**
     * Scale transient with special handling to preserve attack
     * @param raw Raw transient value
     * @param energy Current energy level (used to blend compression)
     * @param loudMode True to apply compression
     * @return Scaled transient value (0-1 range)
     */
    float scaleTransient(float raw, float energy, bool loudMode);

private:
    /**
     * Linear clamp to [0,1] - used in normal mode
     */
    float scaleLinear(float x);

    /**
     * Logarithmic compression - used in loud mode
     * Formula: log(1 + k*x) / log(1 + k)
     */
    float scaleLog(float x);
};
