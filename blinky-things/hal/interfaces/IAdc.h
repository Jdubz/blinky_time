#pragma once
#include <stdint.h>

/**
 * IAdc - Abstract interface for ADC operations
 *
 * Used by BatteryMonitor for voltage measurement.
 * Enables unit testing with mocks.
 */
class IAdc {
public:
    virtual ~IAdc() = default;

    // Reference voltage constants
    static constexpr uint8_t REF_DEFAULT = 0;
    static constexpr uint8_t REF_INTERNAL_2V4 = 1;  // nRF52840 2.4V internal reference

    // Configuration
    virtual void setResolution(uint8_t bits) = 0;
    virtual void setReference(uint8_t reference) = 0;

    // Operations
    virtual uint16_t analogRead(int pin) = 0;
};
