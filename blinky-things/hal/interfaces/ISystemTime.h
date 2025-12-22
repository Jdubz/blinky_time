#pragma once
#include <stdint.h>

/**
 * ISystemTime - Abstract interface for system timing
 *
 * Used by multiple components for timing operations.
 * Enables unit testing with controllable time.
 */
class ISystemTime {
public:
    virtual ~ISystemTime() = default;

    // Timing queries (const - reading time doesn't modify state)
    virtual uint32_t millis() const = 0;
    virtual uint32_t micros() const = 0;

    // Delays
    virtual void delay(uint32_t ms) = 0;
    virtual void delayMicroseconds(uint32_t us) = 0;

    // Interrupt control
    virtual void noInterrupts() = 0;
    virtual void interrupts() = 0;
};
