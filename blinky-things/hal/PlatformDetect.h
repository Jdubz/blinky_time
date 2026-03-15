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

// ESP32-S3 detection: prefer the IDF target macro (set per-SoC by ESP-IDF sdkconfig)
// or the Seeed XIAO board macros (set by arduino-esp32 board variant files).
// The bare ESP32 guard is intentionally last — it matches all ESP32 variants and
// would activate S3-specific peripherals (PDM driver, NVS) on unsupported SoCs.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || \
    defined(ARDUINO_SEEED_XIAO_ESP32S3) || defined(ARDUINO_XIAO_ESP32S3)
  #define BLINKY_PLATFORM_ESP32S3 1
#elif defined(ESP32)
  // Broad ESP32 fallback — only reached when no SoC-specific macro is available.
  // Assumes S3 because that is the only ESP32 target in this project.
  // Refine this guard if other ESP32 variants are added.
  #define BLINKY_PLATFORM_ESP32S3 1

#elif defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_NRF52) || \
      defined(NRF52) || defined(NRF52840_XXAA)
  #define BLINKY_PLATFORM_NRF52840 1

#else
  #error "Unsupported platform. Add detection to hal/PlatformDetect.h"
#endif
