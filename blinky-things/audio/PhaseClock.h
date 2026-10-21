#pragma once
#include <math.h>
#include <stdint.h>
#include "SharedSpectralAnalysis.h"

/**
 * The rhythm phase keeps ABSOLUTE time on the mic's sample clock.
 *
 * #261: the PLP phase used to advance one spectral frame per PROCESSED
 * frame. When the PDM ring overran, the dropped samples advanced nothing,
 * so the beat fell behind real time until the next phase correction. The
 * mic's read index counts every sample, dropped ones included, so the phase
 * now advances by the samples that actually elapsed.
 *
 * #260: renderers read the phase when they draw, up to one frame (plus the
 * PDM block not yet delivered) after the last processed frame. outputPhase()
 * extrapolates it to the moment of drawing.
 */
namespace PhaseClock {
    // Spectral frames are FFT_SIZE samples with no overlap (62.5 Hz at 16 kHz).
    constexpr float SAMPLES_PER_FRAME = static_cast<float>(SpectralConstants::FFT_SIZE);

    // Extrapolation never crosses an accent the tracker hasn't counted
    // (beatCount and the pattern stay consistent): it holds just below it
    // until the next frame advances the phase properly.
    constexpr float HOLD_BELOW_ACCENT = 0.9999f;

    // A render-time extrapolation covers the samples since the last frame
    // (< 1 frame) plus one undelivered PDM block (<= 512 samples). Beyond
    // that the audio path has stalled, and the phase should wait for it.
    constexpr uint32_t MAX_EXTRAPOLATE_SAMPLES = 4 * SpectralConstants::FFT_SIZE;

    // A frame can't really span more than this: only an overrun stretches
    // it, by the time the consumer was stalled. A larger span means the
    // sample index itself jumped (a mic restart), not that time passed.
    constexpr uint32_t MAX_FRAME_SPAN_SAMPLES =
        static_cast<uint32_t>(60.0f * SpectralConstants::SAMPLE_RATE);  // 60 s

    /**
     * Phase after advancing `phase` by `frames` at `periodFrames` per beat:
     * the wrapped phase in [0, 1) and how many accents were crossed. Constant
     * work for any span (a long overrun can cross several beats).
     */
    struct Advance {
        float phase;
        uint32_t accents;
    };

    inline Advance advance(float phase, float frames, float periodFrames) {
        float p = phase + frames / periodFrames;
        if (!(p >= 1.0f)) return {p, 0u};
        const float wraps = floorf(p);
        p -= wraps;
        if (p >= 1.0f) p = 0.0f;  // float rounding at an exact multiple
        return {p, static_cast<uint32_t>(wraps)};
    }

    /** Spectral frames' worth of time in `samples` mic samples. */
    inline float framesFromSamples(uint32_t samples) {
        return static_cast<float>(samples) / SAMPLES_PER_FRAME;
    }

    /**
     * Phase at render time: `phase` (as of the last processed frame) plus
     * `samplesAhead` samples at `periodFrames` frames per beat. Monotonic in
     * samplesAhead, capped at MAX_EXTRAPOLATE_SAMPLES and held below 1.
     */
    inline float outputPhase(float phase, uint32_t samplesAhead, float periodFrames) {
        if (!(periodFrames > 1.0f)) return phase;
        if (samplesAhead > MAX_EXTRAPOLATE_SAMPLES) samplesAhead = MAX_EXTRAPOLATE_SAMPLES;
        const float p = phase + framesFromSamples(samplesAhead) / periodFrames;
        return (p < HOLD_BELOW_ACCENT) ? p : HOLD_BELOW_ACCENT;
    }
}
