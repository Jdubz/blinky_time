#pragma once
#include <math.h>
#include <stdint.h>

/**
 * Frame timing for animations that keep ABSOLUTE time (#259).
 *
 * A late frame must land where it belongs in time, not stretch the
 * animation. So a generator advances its clocks (noise time, drift, hue,
 * closed-form decays such as expf(-dt/tau)) by the real elapsed time, never
 * a clamped one. Explicit physics integration (forces, velocities, heat
 * diffusion, ripples) is the one thing a long step breaks, so it runs the
 * same elapsed time in bounded substeps instead (FrameTiming::physicsSteps).
 */
namespace FrameTiming {
    // Longest single physics step: what the old 100 ms dt clamp protected.
    constexpr float MAX_PHYSICS_STEP_S = 0.05f;

    // At most this much of a long gap is integrated (CPU budget: <= 20
    // steps). Particles live <= 2.55 s and ripples damp within ~1 s, so the
    // visible state after a longer stall is the last second's anyway; the
    // clocks above still advance by the whole gap.
    constexpr float MAX_PHYSICS_SPAN_S = 1.0f;

    struct Steps {
        int count;   // 0 when no time passed
        float dt;    // seconds per step; count * dt = min(frameDt, MAX_PHYSICS_SPAN_S)
    };

    inline Steps physicsSteps(float frameDt) {
        if (!(frameDt > 0.0f)) return {0, 0.0f};
        const float span = (frameDt < MAX_PHYSICS_SPAN_S) ? frameDt : MAX_PHYSICS_SPAN_S;
        int n = static_cast<int>(ceilf(span / MAX_PHYSICS_STEP_S));
        if (n < 1) n = 1;
        return {n, span / static_cast<float>(n)};
    }

    // The part of a frame that particle spawning sees: spawning runs once
    // per frame (it is edge-triggered on onsets/beats), over the same span
    // the physics integrates.
    inline float physicsSpan(float frameDt) {
        if (!(frameDt > 0.0f)) return 0.0f;
        return (frameDt < MAX_PHYSICS_SPAN_S) ? frameDt : MAX_PHYSICS_SPAN_S;
    }
}

/**
 * Real seconds between ticks. The first tick (after construction or
 * restart()) returns 0: nothing has elapsed yet, and an assumed 1/60 s would
 * be invented time. Never clamped (uint32 millis wrap is handled).
 */
class FrameClock {
public:
    float tick(uint32_t nowMs) {
        const float dt = started_ ? static_cast<float>(static_cast<uint32_t>(nowMs - lastMs_)) * 0.001f
                                  : 0.0f;
        lastMs_ = nowMs;
        started_ = true;
        return dt;
    }

    void restart() { started_ = false; }

private:
    uint32_t lastMs_ = 0;
    bool started_ = false;
};
