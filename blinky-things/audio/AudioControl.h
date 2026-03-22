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
 * Memory: 32 bytes (7 floats + 1 uint8_t + padding)
 */
struct AudioControl {
    // === ENERGY ===
    // Overall audio energy level, smoothed and normalized (0.0 - 1.0)
    // Combines: mic level, bass mel energy, ODF peak-hold
    // Use for: Baseline intensity, brightness, activity level
    float energy = 0.0f;

    // === PULSE ===
    // Transient/hit intensity (0.0 - 1.0)
    // Raw NN onset strength with pattern prediction boost
    // 0.0 = no transient, 1.0 = strong transient
    // Use for: Sparks, flashes, bursts, event triggers
    float pulse = 0.0f;

    // === PHASE ===
    // Beat phase position (0.0 - 1.0)
    // PLP-driven: free-running at detected tempo, corrected by pattern cross-correlation
    // 0.0 = start of pattern cycle, 1.0 = next cycle (wraps to 0)
    // Use for: Beat wrap detection, direct phase-based calculations
    float phase = 0.0f;

    // === PLP PULSE ===
    // Extracted dominant energy pattern value at current phase position (0.0 - 1.0)
    // The actual repeating energy shape (sharp kick attacks, fast decays — not a
    // synthesized sinusoid). Extracted via epoch-folding at detected tempo period.
    // Falls back to cosine pulse when PLP confidence is low.
    // Use for: Pulsing effects, breathing animations, beat-synced modulation
    float plpPulse = 0.0f;

    // === RHYTHM STRENGTH ===
    // Confidence in detected rhythm pattern (0.0 - 1.0)
    // Max of ACF periodicity strength and PLP PMR-based confidence
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
    // Downbeat activation (0.0 - 1.0). Always 0 — not tracked by current model.
    // Reserved for future multi-output model.
    float downbeat = 0.0f;

    // === BEAT IN MEASURE ===
    // Position in the current measure. Always 0 — not tracked by current model.
    uint8_t beatInMeasure = 0;

    // === CONVENIENCE METHODS ===

    /**
     * Get beat-synced pulse intensity for breathing/pulsing effects.
     * Returns the PLP extracted pattern value — the actual repeating energy
     * shape of the music. Peaks at rhythmically periodic positions.
     * When PLP confidence is low, falls back to cosine pulse.
     */
    inline float phaseToPulse() const {
        return plpPulse;
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
