#pragma once

#include <math.h>

/**
 * AudioControl - Unified audio control signal for visual generators
 *
 * Synthesizes all audio analysis into 7 parameters.
 * Generators receive this struct and don't need to know about:
 * - Microphone processing
 * - FFT/spectral analysis
 * - BPM detection algorithms
 * - Beat tracking internals
 *
 * Memory: 28 bytes (6 floats + 1 uint8_t + padding)
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

    // === DOWNBEAT ===
    // Downbeat activation (0.0 - 1.0), synchronized with beat detection.
    // Only fires on actual beats (not between beats). Smoothed from NN output.
    // Only meaningful when nnBeatActivation is enabled with a multi-output FrameBeatNN model.
    // Use for: Extra-dramatic effects every 4 beats (e.g., burst of sparks on bar 1)
    float downbeat = 0.0f;

    // === BEAT IN MEASURE ===
    // Position in the current measure (1-4 for 4/4 time, 0 = unknown/no rhythm).
    // Reset to 1 when downbeat detected. Increments each beat. Wraps at 5→1.
    // Only meaningful when rhythmStrength > 0.5 and downbeat model is available.
    // Use for: Syncopation patterns, accent beats 1 and 3, etc.
    uint8_t beatInMeasure = 0;

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
