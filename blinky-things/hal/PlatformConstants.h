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
        // Battery voltage is divided by ~4 before reaching ADC (1510k:510k resistor divider)
        // Ratio = R2/(R1+R2) = 510/(1510+510) = 510/2020 ≈ 0.2525
        constexpr float DIVIDER_RATIO = 510.0f / (1510.0f + 510.0f);

        // ADC reference voltage (platform-dependent)
        // mbed core: Can set to 2.4V with AR_INTERNAL2V4
        // non-mbed Seeed nRF52 core: Stuck at hardware default (~2.76V empirically measured)
        #if defined(P0_31) || defined(AR_INTERNAL2V4)
        constexpr float VREF_2V4 = 2.4f;   // mbed core with configurable reference
        #else
        constexpr float VREF_2V4 = 2.76f;  // non-mbed core (empirically calibrated)
        #endif

        // LiPo voltage thresholds (chemistry-dependent, not device-dependent)
        constexpr float VOLTAGE_FULL = 4.20f;      // Fully charged (100%)
        constexpr float VOLTAGE_HIGH = 4.05f;      // Nearly full (92%)
        constexpr float VOLTAGE_GOOD = 3.90f;      // Good charge (75%)
        constexpr float VOLTAGE_NOMINAL = 3.70f;   // Nominal voltage (40%)
        constexpr float VOLTAGE_LOW = 3.50f;       // Low battery warning (10%)
        constexpr float VOLTAGE_CRITICAL = 3.30f;  // Critical - shutdown soon (0%)
        constexpr float VOLTAGE_EMPTY = 3.00f;     // Over-discharge protection

        // Percentage breakpoints for voltage-to-percent curve
        constexpr uint8_t PERCENT_FULL = 100;
        constexpr uint8_t PERCENT_HIGH = 92;
        constexpr uint8_t PERCENT_GOOD = 75;
        constexpr uint8_t PERCENT_NOMINAL = 40;
        constexpr uint8_t PERCENT_LOW = 10;
        constexpr uint8_t PERCENT_CRITICAL = 0;

        // Default thresholds for battery warnings
        constexpr float DEFAULT_LOW_THRESHOLD = VOLTAGE_LOW;
        constexpr float DEFAULT_CRITICAL_THRESHOLD = VOLTAGE_CRITICAL;

        // Battery connection detection range (valid LiPo operating range)
        constexpr float MIN_CONNECTED_VOLTAGE = 2.5f;  // Below this, battery is disconnected
        constexpr float MAX_CONNECTED_VOLTAGE = 4.3f;  // Above this, battery is disconnected

        // Voltage sanity check range (broader than operating range)
        // Readings outside this range indicate hardware/configuration errors
        constexpr float MIN_VALID_VOLTAGE = 2.0f;  // Minimum physically plausible reading
        constexpr float MAX_VALID_VOLTAGE = 5.0f;  // Maximum physically plausible reading

        // ADC settling time for voltage divider MOSFET switch (milliseconds)
        constexpr uint8_t ADC_SETTLE_TIME_MS = 20;  // Time to wait after enabling divider
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
        constexpr int DEFAULT_GAIN = 60;                  // Initial PDM gain (0-80)

        // Hardware gain limits (nRF52840 PDM hardware range, not user-configurable)
        constexpr int HW_GAIN_MIN = 0;
        constexpr int HW_GAIN_MAX = 80;
    }
}
