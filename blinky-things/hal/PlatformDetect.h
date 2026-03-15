#pragma once

/**
 * PlatformDetect.h - Centralised compile-time platform selection
 *
 * Translates Arduino core macros into clean BLINKY_PLATFORM_* defines.
 * Include this header wherever platform-conditional code is needed instead
 * of repeating the raw Arduino macro checks everywhere.
 *
 * Supported platforms:
 *   BLINKY_PLATFORM_NRF52840  — Seeed XIAO nRF52840 Sense (mbed or Adafruit/Seeed nRF52 core)
 *   BLINKY_PLATFORM_ESP32S3   — Seeed XIAO ESP32-S3 Sense (arduino-esp32 >= 2.0)
 */

#if defined(ESP32)
  #define BLINKY_PLATFORM_ESP32S3 1

#elif defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_NRF52) || \
      defined(NRF52) || defined(NRF52840_XXAA)
  #define BLINKY_PLATFORM_NRF52840 1

#else
  #error "Unsupported platform. Add detection to hal/PlatformDetect.h"
#endif
