#pragma once

#include "PlatformDetect.h"
#include "hardware/ArduinoHal.h"

#ifdef BLINKY_PLATFORM_NRF52840
  #include "hardware/Nrf52PdmMic.h"
#elif defined(BLINKY_PLATFORM_ESP32S3)
  #include "hardware/Esp32PdmMic.h"
#endif

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

#ifdef BLINKY_PLATFORM_NRF52840
    inline Nrf52PdmMic& pdm() {
        static Nrf52PdmMic instance;
        return instance;
    }
#elif defined(BLINKY_PLATFORM_ESP32S3)
    inline Esp32PdmMic& pdm() {
        static Esp32PdmMic instance;
        return instance;
    }
#endif

}  // namespace DefaultHal
