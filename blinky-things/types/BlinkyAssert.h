#pragma once

/**
 * BlinkyAssert - Non-fatal runtime assertions for embedded safety.
 *
 * Philosophy: On a controlled embedded system, unexpected conditions are bugs.
 * Silent fallbacks hide bugs. BLINKY_ASSERT makes them visible without bricking.
 *
 * Behavior:
 * - Logs error message via Serial (when connected)
 * - Increments global error counter (visible via "show errors" serial command)
 * - NEVER halts the CPU - device continues running
 * - The caller still provides safe fallback behavior (preventing UB/bricking)
 *   but now the error is visible for debugging
 *
 * Usage:
 *   BLINKY_ASSERT(index >= 0 && index < MAX, "OOB index in getConfig");
 *   // Then handle gracefully (return fallback, clamp, etc.)
 */

#include <Arduino.h>

namespace BlinkyAssert {
    // Global error counter - monotonically increasing, never reset except by reboot
    extern volatile uint16_t failCount;

    // Called when assertion fails. Logs message, increments counter.
    void onFail(const __FlashStringHelper* msg);
}

// BLINKY_ASSERT(condition, "message") - logs and counts if condition is false
#define BLINKY_ASSERT(cond, msg) \
    do { if (!(cond)) ::BlinkyAssert::onFail(F(msg)); } while(0)
