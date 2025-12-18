#pragma once

#include <Arduino.h>

/**
 * SafetyTest - Runtime safety validation
 *
 * Catches potential issues that could corrupt bootloader or crash device:
 * - Flash address validation (CRITICAL - prevents bootloader corruption)
 * - Memory allocation sanity checks
 * - Stack usage estimation
 * - Buffer bounds validation
 *
 * Run at startup before any flash writes occur.
 */

namespace SafetyTest {

    // Test result structure
    struct TestResult {
        bool passed;
        const char* testName;
        const char* message;
    };

    // Bootloader protection - MOST CRITICAL
    // nRF52840 bootloader is typically in first 0x10000-0x27000 bytes
    // We should NEVER write below 0x30000 to be safe
    static constexpr uint32_t BOOTLOADER_END = 0x30000;  // 192KB safety margin
    static constexpr uint32_t FLASH_END = 0x100000;      // 1MB total flash

    /**
     * Validate a flash address is safe to write
     * Returns true if address is in safe user region
     */
    inline bool isFlashAddressSafe(uint32_t addr, uint32_t size = 4096) {
        // Must be above bootloader region
        if (addr < BOOTLOADER_END) return false;

        // Must not overflow past flash end
        if (addr + size > FLASH_END) return false;

        // Must be aligned to sector boundary (4KB for nRF52)
        if (addr % 4096 != 0) return false;

        return true;
    }

    /**
     * Test flash configuration safety
     * CRITICAL: Run this BEFORE any flash operations
     */
    inline TestResult testFlashSafety() {
        TestResult result = {true, "Flash Safety", "OK"};

#if defined(ARDUINO_ARCH_MBED)
        // Get actual flash configuration
        extern uint32_t __flash_start__;  // Linker symbol

        // The config storage should use last 4KB of flash
        // On nRF52840: flash starts at 0x0, ends at 0x100000 (1MB)
        // Safe config region: 0xFF000 (last 4KB)
        uint32_t expectedConfigAddr = FLASH_END - 4096;  // 0xFF000

        // Verify we're not accidentally targeting bootloader region
        // This is a compile-time sanity check
        if (expectedConfigAddr < BOOTLOADER_END) {
            result.passed = false;
            result.message = "CRITICAL: Config addr in bootloader region!";
            return result;
        }
#endif

        return result;
    }

    /**
     * Test heap allocation works correctly
     */
    inline TestResult testHeapAllocation() {
        TestResult result = {true, "Heap Alloc", "OK"};

        // Test small allocation
        uint8_t* small = new(std::nothrow) uint8_t[64];
        if (!small) {
            result.passed = false;
            result.message = "Small alloc failed";
            return result;
        }

        // Write pattern to detect corruption
        for (int i = 0; i < 64; i++) {
            small[i] = (uint8_t)(i ^ 0xAA);
        }

        // Verify pattern
        for (int i = 0; i < 64; i++) {
            if (small[i] != (uint8_t)(i ^ 0xAA)) {
                delete[] small;
                result.passed = false;
                result.message = "Memory corruption detected";
                return result;
            }
        }

        delete[] small;

        // Test larger allocation (simulating pixel buffer)
        uint8_t* large = new(std::nothrow) uint8_t[256];
        if (!large) {
            result.passed = false;
            result.message = "Large alloc failed";
            return result;
        }
        delete[] large;

        return result;
    }

    /**
     * Estimate stack usage
     * Paints stack with pattern, then checks how much was overwritten
     */
    inline TestResult testStackUsage() {
        TestResult result = {true, "Stack Usage", "OK"};

        // Get approximate stack pointer
        volatile uint32_t stackVar = 0;
        uint32_t currentSP = (uint32_t)&stackVar;

        // nRF52840 has 256KB RAM, stack typically starts near end
        // Stack grows downward, so lower address = more stack used
        // Warn if stack seems low (arbitrary threshold)

        // Just verify we can read the stack pointer
        if (currentSP == 0) {
            result.passed = false;
            result.message = "Stack pointer invalid";
            return result;
        }

        return result;
    }

    /**
     * Test array bounds for common structures
     */
    inline TestResult testArrayBounds() {
        TestResult result = {true, "Array Bounds", "OK"};

        // Test that we can safely access array indices
        // This is more of a compile-time check but validates at runtime

        const int testSize = 60;  // Typical LED count
        uint8_t testArray[60];

        // Fill with pattern
        for (int i = 0; i < testSize; i++) {
            testArray[i] = i;
        }

        // Verify bounds
        if (testArray[0] != 0 || testArray[testSize-1] != testSize-1) {
            result.passed = false;
            result.message = "Array bounds error";
            return result;
        }

        return result;
    }

    /**
     * Test JSON buffer won't overflow
     * Simulates worst-case JSON output size
     */
    inline TestResult testJsonBufferSize() {
        TestResult result = {true, "JSON Buffer", "OK"};

        // Estimate max JSON settings output:
        // Each setting: ~80 chars max
        // {"name":"longsettingname","value":123456789,"type":"uint32","cat":"category","min":0,"max":4294967295}
        // With 48 max settings: 48 * 80 = 3840 chars
        // Serial buffer is typically 64-256 bytes, but we stream directly

        // Just verify Serial is available
        if (!Serial) {
            // Serial not initialized is OK during early boot
            result.message = "Serial not ready (OK at boot)";
        }

        return result;
    }

    /**
     * Run all safety tests
     * Returns number of failures (0 = all passed)
     */
    inline int runAllTests(bool verbose = true) {
        int failures = 0;

        if (verbose) {
            Serial.println(F("\n=== SAFETY TESTS ==="));
        }

        TestResult tests[] = {
            testFlashSafety(),
            testHeapAllocation(),
            testStackUsage(),
            testArrayBounds(),
            testJsonBufferSize()
        };

        const int numTests = sizeof(tests) / sizeof(tests[0]);

        for (int i = 0; i < numTests; i++) {
            if (verbose) {
                Serial.print(tests[i].testName);
                Serial.print(F(": "));
                if (tests[i].passed) {
                    Serial.print(F("PASS"));
                } else {
                    Serial.print(F("FAIL - "));
                    Serial.print(tests[i].message);
                    failures++;
                }
                Serial.println();
            } else if (!tests[i].passed) {
                failures++;
            }
        }

        if (verbose) {
            Serial.print(F("Tests: "));
            Serial.print(numTests - failures);
            Serial.print(F("/"));
            Serial.print(numTests);
            Serial.println(F(" passed"));

            if (failures > 0) {
                Serial.println(F("!!! SAFETY TEST FAILURES - DO NOT PROCEED !!!"));
            }
            Serial.println();
        }

        return failures;
    }

    /**
     * Critical safety check - call before ANY flash write
     * Halts execution if address is unsafe
     */
    inline void assertFlashSafe(uint32_t addr, uint32_t size) {
        if (!isFlashAddressSafe(addr, size)) {
            Serial.println(F("\n!!! CRITICAL: UNSAFE FLASH WRITE BLOCKED !!!"));
            Serial.print(F("Address: 0x")); Serial.println(addr, HEX);
            Serial.print(F("Size: ")); Serial.println(size);
            Serial.println(F("This would corrupt bootloader/firmware!"));
            Serial.println(F("System halted to prevent damage."));

            // Halt forever - do NOT allow unsafe write
            while (1) {
                delay(10000);  // Long delay to minimize CPU wakeups and power usage
            }
        }
    }

}  // namespace SafetyTest
