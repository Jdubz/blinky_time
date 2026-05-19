#pragma once
#include <Arduino.h>
#include "../render/RenderPipeline.h"

/**
 * GeneratorButton — debounced GPIO button that cycles the active visual
 * generator on each press.
 *
 * Wiring assumption: button between the GPIO pin and GND. Pin is configured
 * INPUT_PULLUP, so the line idles HIGH and a press pulls it LOW. This is
 * the most common breadboard / through-hole tactile-button arrangement and
 * avoids needing an external pull resistor.
 *
 * Debounce strategy (two-layer, post-2026-05-19 phantom-press hardening):
 *
 *   1. **Sustained-level filter.** A candidate level must be observed for
 *      ``STABLE_SAMPLE_COUNT`` consecutive ``poll()`` calls before it
 *      becomes the new stable level. At the main loop's ~60 Hz tick,
 *      3 samples ≈ 50 ms of sustained level required. Short EMI spikes
 *      (sub-50 ms LOW pulses induced by the nearby WS2812B data line
 *      coupling into a long, unshielded button wire — see OPEN_ISSUES
 *      §1.4 / big_bucket 2026-05-19) are absorbed by the count filter.
 *
 *   2. **Time-spacing window.** Once a stable level transition is
 *      recognized, no further edge is accepted until ``DEBOUNCE_MS`` has
 *      elapsed. This bounds the maximum accepted press rate (2 Hz at
 *      500 ms) and absorbs any residual contact bounce that survived
 *      the sustained-level filter.
 *
 * The pre-2026-05-19 implementation used only layer 2 with DEBOUNCE_MS=200.
 * EMI pulses longer than 200 ms (which the big_bucket wire was producing
 * intermittently) were registering as real presses. Layer 1 closes that
 * gap; layer 2 was bumped 200→500 ms to widen the operator-press cadence
 * floor too. Workaround #2 from OPEN_ISSUES §1.4.
 *
 * Pin 0 in the config means "no button"; this class is a no-op in that
 * case so devices without a button compile and run identically.
 */
class GeneratorButton {
public:
    // Initialize for a given GPIO pin. Pass 0 to disable (no-op mode).
    // Safe to call begin() multiple times if the config is reloaded — the
    // pinMode call is idempotent on Arduino cores.
    void begin(uint8_t pin) {
        pin_ = pin;
        if (pin_ == 0) return;
        pinMode(pin_, INPUT_PULLUP);
        // Seed `lastStableHigh_` from the current pin state so we don't
        // register a phantom edge on the first poll (e.g. if the operator
        // is already holding the button at boot). Also seed the candidate
        // tracker to the same value so the count filter starts from a
        // consistent baseline.
        bool initialHigh = (digitalRead(pin_) == HIGH);
        lastStableHigh_ = initialHigh;
        candidateHigh_ = initialHigh;
        consecutiveSamples_ = STABLE_SAMPLE_COUNT;  // already stable at boot
        lastEdgeMs_ = millis();
    }

    // Poll the button. Returns true if a falling edge (press) was just
    // accepted after BOTH the sustained-level filter (layer 1) and the
    // time-spacing window (layer 2). Caller takes its action (cycle
    // generator) on a true return.
    //
    // Call cadence: every main-loop iteration. The sustained-level filter
    // depends on regular polling; a poll() rate slower than ~20 Hz would
    // require STABLE_SAMPLE_COUNT to shrink (or the perceived button
    // latency would balloon to >150 ms).
    bool poll() {
        if (pin_ == 0) return false;
        bool currentHigh = (digitalRead(pin_) == HIGH);
        uint32_t now = millis();

        // Layer 1: sustained-level filter. Require STABLE_SAMPLE_COUNT
        // consecutive samples agreeing with the candidate before we treat
        // it as a stable level. A flicker (EMI pulse shorter than
        // STABLE_SAMPLE_COUNT polls) resets the candidate and discards
        // the count.
        if (currentHigh == candidateHigh_) {
            if (consecutiveSamples_ < STABLE_SAMPLE_COUNT) {
                consecutiveSamples_++;
            }
        } else {
            candidateHigh_ = currentHigh;
            consecutiveSamples_ = 1;
            return false;  // candidate just changed; not stable yet
        }
        if (consecutiveSamples_ < STABLE_SAMPLE_COUNT) return false;

        // Candidate is now stable. If it differs from the last accepted
        // stable level AND the time-spacing window has elapsed, accept it.
        if (currentHigh == lastStableHigh_) return false;

        // Layer 2: time-spacing window. Reject edges arriving sooner than
        // DEBOUNCE_MS after the prior accepted edge.
        if ((now - lastEdgeMs_) < DEBOUNCE_MS) return false;

        lastStableHigh_ = currentHigh;
        lastEdgeMs_ = now;
        // Press = HIGH→LOW (release of pull-up by closing to GND).
        return !currentHigh;
    }

    // Cycle the active generator on the pipeline by one slot. Uses
    // RenderPipeline's index-based generator API so we don't have to
    // hand-maintain a list of GeneratorType values here.
    static void cycleGenerator(RenderPipeline* pipeline) {
        if (!pipeline) return;
        int curIdx = (int)pipeline->getGeneratorType();
        int nextIdx = (curIdx + 1) % RenderPipeline::NUM_GENERATORS;
        pipeline->setGenerator(RenderPipeline::getGeneratorTypeByIndex(nextIdx));
    }

private:
    // Layer 2 time-spacing window. Bumped 200→500 ms after the 2026-05-19
    // big_bucket phantom-press observation: 200 ms was wide enough to
    // accept EMI-driven multi-press cascades. 500 ms is comfortably
    // longer than any operator-press cadence.
    static constexpr uint32_t DEBOUNCE_MS = 500;
    // Layer 1 sustained-level count. At a ~60 Hz main-loop poll rate,
    // 3 samples ≈ 50 ms of sustained level required before an edge is
    // recognized. Short EMI pulses (sub-50 ms) get absorbed.
    static constexpr uint8_t STABLE_SAMPLE_COUNT = 3;

    uint8_t pin_ = 0;
    bool lastStableHigh_ = true;
    // Candidate level being observed for stability. Distinct from
    // lastStableHigh_: candidateHigh_ can flip every poll; lastStableHigh_
    // only updates after the count threshold is met.
    bool candidateHigh_ = true;
    uint8_t consecutiveSamples_ = 0;
    uint32_t lastEdgeMs_ = 0;
};
