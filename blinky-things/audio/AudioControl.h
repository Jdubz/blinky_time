#pragma once

#include <math.h>

/**
 * AudioControl - Unified audio control signal for visual generators
 *
 * Synthesizes all audio analysis into a handful of parameters.
 * Generators receive this struct and don't need to know about:
 * - Microphone processing
 * - FFT/spectral analysis
 * - BPM detection algorithms
 * - Beat tracking internals
 *
 * Memory: 32 bytes
 */
struct AudioControl {
    // === ENERGY ===
    // Overall audio energy level, smoothed and normalized (0.0 - 1.0)
    // Combines: mic level, bass mel energy, ODF peak-hold
    // Use for: Baseline intensity, brightness, activity level
    float energy = 0.0f;

    // === PULSE ===
    // Transient/hit intensity (0.0 - 1.0)
    // Spectral flux auto-normalized, modulated by NN onset activation.
    // When NN is discriminative: onsets (kicks/snares) produce strong pulses,
    // non-onset spectral changes are suppressed. Self-tuning via nnConf.
    // 0.0 = no transient, 1.0 = strong transient
    // Use for: Sparks, flashes, bursts, event triggers
    float pulse = 0.0f;

    // === ONSET ===
    // Discrete onset event: strength (0-1) of the onset fired during this
    // update, 0 when none fired. Exactly one non-zero value per detected
    // percussive event (the detector has its own cooldown), so generators
    // can edge-trigger on `onset > 0` without thresholding the decaying
    // `pulse` envelope (which never re-arms during dense hi-hat passages).
    // Use for: Bursts, flashes — one visual event per drum hit
    float onset = 0.0f;

    // === PHASE ===
    // Rhythm-cycle phase (0.0 - 1.0), 0 = accent. Advances once per audio
    // frame at the detected (fractional) period and is pulled toward the
    // measured accent position; small corrections can move it backward.
    // Use for: Continuous phase-based modulation. For "a beat happened",
    // use beatCount — phase thresholds misfire when phase is corrected.
    float phase = 0.0f;

    // === BEAT COUNT ===
    // Increments exactly once per rhythm cycle (phase wrap), never
    // decrements, even across backward phase corrections.
    // Use for: Beat-synced events: `if (audio.beatCount != lastBeatCount)`
    uint16_t beatCount = 0;

    // === PLP PULSE ===
    // Extracted dominant energy pattern value at current phase position (0.0 - 1.0)
    // The actual repeating energy shape (sharp kick attacks, fast decays — not a
    // synthesized sinusoid). Extracted via epoch-folding at detected tempo period.
    // Falls back to cosine pulse when PLP confidence is low.
    // Use for: Pulsing effects, breathing animations, beat-synced modulation
    float plpPulse = 0.0f;

    // === RHYTHM STRENGTH ===
    // Confidence in detected rhythm pattern (0.0 - 1.0)
    // Max of ACF periodicity strength and PLP confidence
    // 0.0 = no rhythm detected (use organic behavior)
    // 1.0 = strong rhythm locked (use beat-synced behavior)
    // Use for: Choosing between music mode vs organic mode behavior
    float rhythmStrength = 0.0f;

    // === ONSET DENSITY ===
    // Smoothed onsets per second (EMA, typical range 0-10+)
    // Dance music: 2-6/s, ambient: 0-1/s, complex: 4-10/s
    // Use for: Content classification, organic/music mode blending
    float onsetDensity = 0.0f;

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
};
