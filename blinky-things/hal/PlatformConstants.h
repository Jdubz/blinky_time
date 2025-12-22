#pragma once
#include <stdint.h>

/**
 * PlatformConstants - Hardware-level constants for XIAO BLE Sense (nRF52840)
 *
 * These are properties of the hardware platform itself, not device configuration.
 * All devices using the same hardware share these values.
 */
namespace Platform {

    // Battery hardware constants (XIAO BLE Sense with typical LiPo)
    namespace Battery {
        // Voltage divider ratio on XIAO BLE boards
        // Battery voltage is divided by ~3 before reaching ADC (1510:510 resistor divider)
        constexpr float DIVIDER_RATIO = 1.0f / 3.0f;

        // ADC reference voltage when using AR_INTERNAL2V4
        constexpr float VREF_2V4 = 2.4f;

        // LiPo voltage thresholds (chemistry-dependent, not device-dependent)
        constexpr float VOLTAGE_FULL = 4.20f;      // Fully charged
        constexpr float VOLTAGE_NOMINAL = 3.70f;   // Nominal voltage (~50%)
        constexpr float VOLTAGE_LOW = 3.50f;       // Low battery warning (~10-20%)
        constexpr float VOLTAGE_CRITICAL = 3.30f;  // Critical - shutdown soon (~0%)
        constexpr float VOLTAGE_EMPTY = 3.00f;     // Over-discharge protection

        // Default thresholds for battery warnings
        constexpr float DEFAULT_LOW_THRESHOLD = VOLTAGE_LOW;
        constexpr float DEFAULT_CRITICAL_THRESHOLD = VOLTAGE_CRITICAL;
    }

    // Charging hardware constants
    namespace Charging {
        // HICHG pin behavior on XIAO BLE
        constexpr bool HICHG_ACTIVE_LOW = true;   // LOW = 100mA, HIGH = 50mA

        // CHG status pin behavior
        constexpr bool CHG_ACTIVE_LOW = true;     // LOW while charging
    }

    // ADC configuration
    namespace Adc {
        constexpr uint8_t DEFAULT_RESOLUTION = 12;  // 12-bit ADC (0-4095)
        constexpr uint8_t DEFAULT_SAMPLES = 8;      // Oversampling count
    }

    // Microphone configuration
    namespace Microphone {
        constexpr uint32_t DEFAULT_SAMPLE_RATE = 16000;  // 16 kHz
        constexpr int DEFAULT_GAIN = 32;                  // Initial PDM gain (0-64)
    }
}
