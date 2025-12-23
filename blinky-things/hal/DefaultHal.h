#pragma once

#include "hardware/ArduinoHal.h"
#include "hardware/Nrf52PdmMic.h"

/**
 * DefaultHal - Pre-configured HAL instances for production use
 *
 * Provides global singleton instances of all HAL implementations.
 * Use these in the main sketch for minimal code changes.
 *
 * Usage:
 *   #include "hal/DefaultHal.h"
 *   BatteryMonitor battery(DefaultHal::gpio(), DefaultHal::adc(), DefaultHal::time());
 */
namespace DefaultHal {

    // Global singleton instances
    inline ArduinoGpio& gpio() {
        static ArduinoGpio instance;
        return instance;
    }

    inline ArduinoAdc& adc() {
        static ArduinoAdc instance;
        return instance;
    }

    inline ArduinoSystemTime& time() {
        static ArduinoSystemTime instance;
        return instance;
    }

    inline Nrf52PdmMic& pdm() {
        static Nrf52PdmMic instance;
        return instance;
    }

}  // namespace DefaultHal
