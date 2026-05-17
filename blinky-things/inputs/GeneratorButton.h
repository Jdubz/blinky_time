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
 * Debounce: a long 200ms window. Per the per-device config request, this is
 * "longer than a typical 50ms debounce" — it absorbs even quite noisy
 * contacts and rules out double-presses from finger bounce. There is no
 * long-press semantic: every press == one generator advance.
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
        // is already holding the button at boot).
        lastStableHigh_ = (digitalRead(pin_) == HIGH);
        lastEdgeMs_ = millis();
    }

    // Poll the button. Returns true if a falling edge (press) was just
    // accepted after the debounce window, so the caller can take its
    // action (cycle generator). The state machine here is intentionally
    // simple: if the observed level differs from the last STABLE level and
    // the debounce window has elapsed since the last accepted edge, accept
    // the change. This makes the next accepted edge at least DEBOUNCE_MS
    // after the previous one — finger bounce within that window is absorbed.
    bool poll() {
        if (pin_ == 0) return false;
        bool currentHigh = (digitalRead(pin_) == HIGH);
        uint32_t now = millis();
        if (currentHigh == lastStableHigh_) return false;
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
    static constexpr uint32_t DEBOUNCE_MS = 200;

    uint8_t pin_ = 0;
    bool lastStableHigh_ = true;
    uint32_t lastEdgeMs_ = 0;
};
