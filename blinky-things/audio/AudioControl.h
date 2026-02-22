#pragma once

#include <math.h>

/**
 * AudioControl - Unified audio control signal for visual generators
 *
 * Synthesizes all audio analysis into 5 simple parameters.
 * Generators receive this struct and don't need to know about:
 * - Microphone processing
 * - FFT/spectral analysis
 * - BPM detection algorithms
 * - Beat tracking internals
 *
 * Memory: 24 bytes (5 floats + 1 bool + 3 bytes padding)
 */
struct AudioControl {
    // === ENERGY ===
    // Overall audio energy level, smoothed and normalized (0.0 - 1.0)
    // Combines: mic level, beat likelihood boost, rhythmic gating
    // Use for: Baseline intensity, brightness, activity level
    float energy = 0.0f;

    // === PULSE ===
    // Transient/hit intensity with rhythmic context (0.0 - 1.0)
    // Combines: mic transient detection, beat alignment boost/suppress
    // 0.0 = no transient, 1.0 = strong on-beat transient
    // Use for: Sparks, flashes, bursts, event triggers
    float pulse = 0.0f;

    // === PHASE ===
    // Beat phase position (0.0 - 1.0)
    // 0.0 = on-beat moment, 0.5 = off-beat, 1.0 = next beat (wraps to 0)
    // Only meaningful when rhythmStrength > 0.5
    // Use for: Pulsing effects, wave timing, breathing animations
    float phase = 0.0f;

    // === RHYTHM STRENGTH ===
    // Confidence in detected rhythm pattern (0.0 - 1.0)
    // 0.0 = no rhythm detected (use organic behavior)
    // 1.0 = strong rhythm locked (use beat-synced behavior)
    // Use for: Choosing between music mode vs organic mode behavior
    float rhythmStrength = 0.0f;

    // === ONSET DENSITY ===
    // Smoothed onsets per second (EMA, typical range 0-10+)
    // Dance music: 2-6/s, ambient: 0-1/s, complex: 4-10/s
    // Use for: Content classification, organic/music mode blending
    float onsetDensity = 0.0f;

    // === LOUD MODE ===
    // True when hardware gain is at minimum and signal is saturated
    // Generators use this to enable adaptive particle budgets and non-linear mappings
    bool loudMode = false;

    // === CONVENIENCE METHODS ===

    /**
     * Convert phase to pulse intensity.
     * Returns 1.0 at phase=0 (on-beat), 0.0 at phase=0.5 (off-beat).
     * Useful for breathing/pulsing effects synchronized to beat.
     */
    inline float phaseToPulse() const {
        return 0.5f + 0.5f * cosf(phase * 6.28318530718f);
    }

    /**
     * Get phase distance from nearest beat.
     * Returns 0.0 when on-beat (phase near 0 or 1), 0.5 when off-beat.
     */
    inline float distanceFromBeat() const {
        float d = phase < 0.5f ? phase : (1.0f - phase);
        return d;
    }
};
