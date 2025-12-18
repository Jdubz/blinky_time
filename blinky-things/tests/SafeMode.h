#pragma once

#include <Arduino.h>

/**
 * SafeMode - Crash recovery system
 *
 * Detects consecutive crashes and enters safe mode to allow firmware upload.
 * Uses RAM that survives soft reset to count boots.
 *
 * How it works:
 * 1. On boot, increment boot counter
 * 2. If counter > threshold, enter safe mode (USB only, LED blink)
 * 3. After stable running (5 seconds), clear counter
 * 4. Safe mode keeps USB alive so IDE can upload new firmware
 *
 * Usage:
 *   void setup() {
 *     SafeMode::check();  // MUST be first line in setup()
 *     // ... rest of setup
 *     SafeMode::markStable();  // Call after successful init
 *   }
 */

namespace SafeMode {

    // Configuration
    static constexpr uint8_t CRASH_THRESHOLD = 3;      // Enter safe mode after this many crashes
    static constexpr uint32_t STABLE_DELAY_MS = 5000;  // Time before marking boot as stable
    static constexpr uint32_t BLINK_INTERVAL_MS = 200; // LED blink rate in safe mode

    // Magic value to detect valid boot counter (not random RAM)
    // Reads as "BOOT CODE" in hex - helps identify valid data vs uninitialized RAM
    static constexpr uint32_t MAGIC = 0xB007C0DE;

    // Boot counter structure - placed in no-init RAM section
    // This survives soft reset but not power cycle
    struct BootData {
        uint32_t magic;
        uint8_t crashCount;
        uint8_t reserved[3];
    };

    // Platform-specific no-init RAM section
    // For nRF52840 with mbed, we use a specific RAM section
    #if defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_NRF52)
        static BootData bootData __attribute__((section(".noinit")));
    #else
        // Fallback for other platforms - may not survive reset
        static BootData bootData;
    #endif

    /**
     * Safe mode loop - minimal functionality, just USB and LED blink
     * Never returns - user must upload new firmware
     */
    inline void enterSafeMode() {
        // Initialize serial for upload capability
        Serial.begin(115200);

        // Try to initialize built-in LED
        #ifdef LED_BUILTIN
        pinMode(LED_BUILTIN, OUTPUT);
        #endif

        // Also try pin 11 (red LED on XIAO) and pin 13
        pinMode(11, OUTPUT);
        pinMode(13, OUTPUT);

        Serial.println();
        Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
        Serial.println(F("!       SAFE MODE ACTIVATED            !"));
        Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
        Serial.println();
        Serial.println(F("Device detected multiple consecutive crashes."));
        Serial.println(F("Running in safe mode with minimal functionality."));
        Serial.println();
        Serial.println(F("To recover:"));
        Serial.println(F("  1. Open Arduino IDE"));
        Serial.println(F("  2. Upload a working sketch"));
        Serial.println();
        Serial.print(F("Crash count: "));
        Serial.println(bootData.crashCount);
        Serial.println();
        Serial.println(F("USB is active - ready for upload."));
        Serial.println();

        // Reset crash counter so next boot starts fresh
        bootData.crashCount = 0;

        // Infinite loop - just blink LED and keep USB alive
        uint32_t lastBlink = 0;
        bool ledState = false;

        while (true) {
            // Keep USB alive by checking serial
            if (Serial.available()) {
                // Echo any input to show we're alive
                char c = Serial.read();
                Serial.print(c);
            }

            // Blink LED to indicate safe mode
            if (millis() - lastBlink > BLINK_INTERVAL_MS) {
                lastBlink = millis();
                ledState = !ledState;

                #ifdef LED_BUILTIN
                digitalWrite(LED_BUILTIN, ledState);
                #endif
                digitalWrite(11, ledState);  // Red LED on XIAO
                digitalWrite(13, ledState);
            }

            // Small delay to prevent tight loop
            delay(10);
        }
    }

    /**
     * Check boot counter and enter safe mode if needed
     * MUST be called as the FIRST thing in setup()
     */
    inline void check() {
        // Validate magic - if invalid, this is first boot or power cycle
        if (bootData.magic != MAGIC) {
            bootData.magic = MAGIC;
            bootData.crashCount = 0;
        }

        // Increment crash counter
        bootData.crashCount++;

        // Check if we've crashed too many times
        if (bootData.crashCount > CRASH_THRESHOLD) {
            enterSafeMode();  // Never returns
        }

        // Note: Don't call Serial.begin() here - let main setup() do it
        // The boot count will be logged after Serial is properly initialized
    }

    /**
     * Mark boot as stable - call after successful initialization
     * Resets the crash counter
     */
    inline void markStable() {
        bootData.crashCount = 0;
        Serial.println(F("[BOOT] Marked stable - crash counter reset"));
    }

    /**
     * Get current crash count (for debugging)
     */
    inline uint8_t getCrashCount() {
        if (bootData.magic != MAGIC) return 0;
        return bootData.crashCount;
    }

    /**
     * Force safe mode (for testing)
     */
    inline void forceSafeMode() {
        bootData.crashCount = CRASH_THRESHOLD + 1;
        enterSafeMode();
    }

}  // namespace SafeMode
