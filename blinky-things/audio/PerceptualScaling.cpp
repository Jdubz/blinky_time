#include "PerceptualScaling.h"
#include <math.h>

// ============================================================================
// PerceptualScaling Implementation
// ============================================================================

float PerceptualScaling::scale(float raw, bool loudMode) {
    // Automatic mode selection: linear in normal mode, compressed in loud mode
    return loudMode ? scaleLog(raw) : scaleLinear(raw);
}

float PerceptualScaling::scaleLinear(float x) {
    // Simple clamp to [0,1]
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

float PerceptualScaling::scaleLog(float x) {
    // Logarithmic compression: log(1 + kx) / log(1 + k)
    // This compresses high values while preserving low values
    if (x <= 0.0f) return 0.0f;

    // Safety check: prevent division by zero if k is corrupted
    if (logCompressionK < 0.1f) return scaleLinear(x);

    // Apply logarithmic compression
    float compressed = logf(1.0f + logCompressionK * x) / logf(1.0f + logCompressionK);

    // Apply makeup gain to restore average brightness
    float result = compressed * logMakeupGain;

    // Clamp to [0,1]
    return (result > 1.0f) ? 1.0f : result;
}

float PerceptualScaling::scaleTransient(float raw, float energy, bool loudMode) {
    // In normal mode: linear passthrough
    if (!loudMode) return scaleLinear(raw);

    // In loud mode: blend compression based on energy to preserve attack
    // High energy (loud section) → more compression on transients
    // Low energy (quiet section) → preserve full transient dynamics
    if (energy > 0.8f) {
        // High energy: blend between linear and compressed based on how loud it is
        float compressionAmount = (energy - 0.8f) / 0.2f;  // 0-1 for energy 0.8-1.0
        float compressed = scaleLog(raw);
        float linear = scaleLinear(raw);
        // Blend: at energy=0.8, 0% compression; at energy=1.0, 50% compression
        return linear * (1.0f - compressionAmount * 0.5f) + compressed * (compressionAmount * 0.5f);
    }

    // Low/medium energy: use logarithmic compression
    return scaleLog(raw);
}
