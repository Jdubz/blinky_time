#pragma once

#include "AudioControl.h"
#include <math.h>

/**
 * FakeAudio - Synthetic 120 BPM 4/4 dance music simulator
 *
 * Generates realistic AudioControl values for visual design and debugging
 * without requiring a live audio feed.
 *
 * Activate via serial: "fakeaudio on" / "fakeaudio off"
 *
 * Pattern (120 BPM, 4/4 time):
 *   Beat 1 (t=0.00s): kick             — pulse=0.90, energy boost
 *   Beat 2 (t=0.50s): snare            — pulse=0.75
 *   and-of-2 (t=0.75s): synco kick     — pulse=0.45
 *   Beat 3 (t=1.00s): kick             — pulse=0.85
 *   Beat 4 (t=1.50s): snare            — pulse=0.70
 *   and-of-4 (t=1.75s): synco kick     — pulse=0.40
 *
 * All 6 AudioControl fields are set:
 *   energy        — base 0.45, boosted on each onset, decays with ~0.7s fall
 *   pulse         — fires on each onset, decays to 0 in ~0.1s
 *   phase         — 0→1 per beat, resets at each beat (120 BPM)
 *   plpPulse      — cosine pulse synced to beat phase
 *   rhythmStrength — fixed 0.85 (strong lock)
 *   onsetDensity  — fixed 4.0 (dance music range)
 */
class FakeAudio {
public:
    FakeAudio()
        : enabled_(false), measureTime_(0.0f),
          energy_(BASE_ENERGY), pulse_(0.0f) {}

    void enable() {
        enabled_       = true;
        measureTime_   = 0.0f;
        // Fire the first onset immediately so the display starts active
        energy_        = BASE_ENERGY + 0.75f;
        pulse_         = 0.90f;
    }

    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    void update(float dt) {
        if (!enabled_) return;

        float prevTime = measureTime_;
        measureTime_ += dt;
        bool wrapped = (measureTime_ >= MEASURE_DURATION);
        if (wrapped) measureTime_ -= MEASURE_DURATION;

        // Decay energy toward base: fast rise (handled in fireOnset), slow fall ~0.7s
        energy_  += (BASE_ENERGY - energy_) * (dt < 1.0f ? 1.4f * dt : 1.0f);
        pulse_    = pulse_   > 0.0f ? pulse_   - 10.0f * dt : 0.0f;
        if (pulse_    < 0.0f) pulse_    = 0.0f;

        // Fire onset events when measure time crosses their scheduled position
        const OnsetEvent* onsets = getOnsets();
        for (int i = 0; i < NUM_ONSETS; i++) {
            float t = onsets[i].measureTime;
            bool fired;
            if (!wrapped) {
                fired = (prevTime < t && measureTime_ >= t);
            } else {
                // Wrapped: fire if onset is in pre-wrap tail [prevTime, MEASURE_DURATION)
                // OR in post-wrap head [0, measureTime_]. These are disjoint ranges
                // because prevTime is near MEASURE_DURATION and measureTime_ is near 0.
                fired = (t > prevTime) || (t <= measureTime_);
            }
            if (fired) fireOnset(i);
        }

    }

    AudioControl getControl() const {
        AudioControl ac;
        ac.energy        = energy_;
        ac.pulse         = pulse_;
        // Phase: 0.0 on-beat → 1.0 just before next beat
        ac.phase         = fmodf(measureTime_, BEAT_DURATION) / BEAT_DURATION;
        // Cosine pulse: 1.0 at phase=0 (on-beat), 0.0 at phase=0.5 (off-beat)
        ac.plpPulse      = 0.5f * (1.0f + cosf(ac.phase * 2.0f * 3.14159265f));
        ac.rhythmStrength = 0.85f;
        ac.onsetDensity  = 4.0f;   // Typical dance music: 4 onsets/s
        return ac;
    }

private:
    static constexpr float BEAT_DURATION    = 0.5f;   // 120 BPM → 0.5s per beat
    static constexpr float MEASURE_DURATION = 2.0f;   // 4/4: 4 beats × 0.5s
    static constexpr float BASE_ENERGY      = 0.15f;  // Resting energy between hits (low for contrast)

    struct OnsetEvent {
        float measureTime;    // Position in measure (0–2.0s)
        float pulseStrength;  // Pulse output (0–1)
        float energyBoost;    // Added to base energy on hit
    };
    static const int NUM_ONSETS = 6;

    // Returns the onset schedule as a static local array (avoids .cpp requirement)
    static const OnsetEvent* getOnsets() {
        static const OnsetEvent onsets[NUM_ONSETS] = {
            {0.00f, 0.90f, 0.75f},  // Beat 1: kick
            {0.50f, 0.75f, 0.60f},  // Beat 2: snare
            {0.75f, 0.45f, 0.30f},  // and-of-2: syncopated kick
            {1.00f, 0.85f, 0.70f},  // Beat 3: kick
            {1.50f, 0.70f, 0.55f},  // Beat 4: snare
            {1.75f, 0.40f, 0.25f},  // and-of-4: syncopated kick
        };
        return onsets;
    }

    void fireOnset(int idx) {
        const OnsetEvent& e = getOnsets()[idx];
        pulse_  = e.pulseStrength;
        float newEnergy = BASE_ENERGY + e.energyBoost;
        if (newEnergy > energy_) energy_ = newEnergy;  // Only boost, never suppress
    }

    bool    enabled_;
    float   measureTime_;
    float   energy_;
    float   pulse_;
};
