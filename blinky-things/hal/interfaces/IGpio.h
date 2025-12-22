#pragma once
#include <stdint.h>

/**
 * IGpio - Abstract interface for GPIO operations
 *
 * Used by BatteryMonitor for pin control.
 * Enables unit testing with mocks.
 */
class IGpio {
public:
    virtual ~IGpio() = default;

    // Pin mode constants (matching Arduino semantics)
    static constexpr uint8_t INPUT_MODE = 0;
    static constexpr uint8_t OUTPUT_MODE = 1;
    static constexpr uint8_t INPUT_PULLUP_MODE = 2;

    // Logic levels
    static constexpr uint8_t LOW_LEVEL = 0;
    static constexpr uint8_t HIGH_LEVEL = 1;

    // Operations
    virtual void pinMode(int pin, uint8_t mode) = 0;
    virtual void digitalWrite(int pin, uint8_t value) = 0;
    virtual int digitalRead(int pin) const = 0;
};
