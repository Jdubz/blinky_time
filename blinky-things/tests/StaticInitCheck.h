/**
 * StaticInitCheck.h - Static Initialization Order Fiasco Prevention
 *
 * This file documents the dangerous patterns that can brick an embedded device
 * and provides guidance on safe initialization practices.
 *
 * THE PROBLEM:
 * Global C++ objects with constructors are initialized BEFORE main() runs.
 * On Arduino/embedded platforms, this happens BEFORE hardware is ready.
 * If a global object's constructor calls hardware APIs (GPIO, ADC, Serial, etc.),
 * the device will crash immediately on boot - often before USB enumeration,
 * making the device appear "bricked".
 *
 * DANGEROUS PATTERNS (DO NOT USE):
 *
 *   // BAD: Constructor runs before setup(), hardware not ready
 *   AdaptiveMic mic(DefaultHal::pdm(), DefaultHal::time());
 *   BatteryMonitor battery(DefaultHal::gpio(), DefaultHal::adc(), DefaultHal::time());
 *   Adafruit_NeoPixel strip(60, D6, NEO_GRB);
 *
 * SAFE PATTERNS (USE THESE):
 *
 *   // GOOD: Pointer initialized to null, object created in setup()
 *   AdaptiveMic* mic = nullptr;
 *   BatteryMonitor* battery = nullptr;
 *
 *   void setup() {
 *       mic = new AdaptiveMic(DefaultHal::pdm(), DefaultHal::time());
 *       battery = new BatteryMonitor(DefaultHal::gpio(), DefaultHal::adc(), DefaultHal::time());
 *   }
 *
 *   // GOOD: Default constructor with begin() method
 *   IMUHelper imu;  // Default constructor does nothing
 *
 *   void setup() {
 *       imu.begin();  // Hardware access happens here, after runtime is ready
 *   }
 *
 *   // GOOD: Static local (Meyers Singleton) - initialized on first use
 *   Nrf52Gpio& getGpio() {
 *       static Nrf52Gpio instance;  // Created on first call, not at startup
 *       return instance;
 *   }
 *
 * SYMPTOMS OF STATIC INIT CRASHES:
 * - Device doesn't appear on USB/COM port after upload
 * - LEDs stuck in partial state (some on, some off)
 * - No serial output at all
 * - Device appears completely unresponsive
 * - Recovery requires SWD/JTAG programmer to flash bootloader
 *
 * HOW TO CHECK YOUR CODE:
 * Run the static analysis script:
 *   python scripts/check_static_init.py blinky-things/
 *
 * This script detects global declarations with constructor arguments.
 *
 * RECOVERY FROM BRICKED DEVICE:
 * If you brick a device, you need an SWD programmer:
 * - J-Link
 * - Raspberry Pi Pico as debug probe
 * - ST-Link
 *
 * Use the programmer to flash the original bootloader:
 * https://wiki.seeedstudio.com/XIAO_BLE/#restore-factory-bootloader
 *
 * RELATED FILES:
 * - hal/DefaultHal.h: Uses Meyers Singleton for safe global HAL access
 * - tests/SafeMode.h: Detects crash loops at runtime (after static init)
 *
 * Author: Blinky Time Project
 * Created after bricking incident from HAL refactoring, December 2024
 */

#pragma once

// This header is documentation-only. No code to compile.
// Include it where you need a reminder about safe initialization.

// Compile-time reminder macro
#define STATIC_INIT_WARNING \
    "WARNING: Global objects with constructor arguments can brick the device. " \
    "Use pointers and initialize in setup() instead. See tests/StaticInitCheck.h"
