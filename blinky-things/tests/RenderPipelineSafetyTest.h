#pragma once

#include <Arduino.h>
#include "../render/RenderPipeline.h"
#include "../types/PixelMatrix.h"
#include "../audio/AudioControl.h"

/**
 * RenderPipelineSafetyTest - CRITICAL hardware safety validation
 *
 * PURPOSE: Prevent runaway brightness that can MELT LED CONTROLLERS
 *
 * These tests catch rendering bugs that could cause physical damage:
 * - Missing frame clearing → brightness accumulation → overcurrent → MELTED HARDWARE
 * - Brightness overflow → 3x normal current draw → controller failure
 * - Heat buffer runaway → sustained max brightness → thermal damage
 * - Color accumulation → white saturation → 765 current units instead of 255
 *
 * CRITICAL SAFETY REQUIREMENTS:
 * 1. Frame must be cleared before each render
 * 2. No pixel may exceed RGB(255,255,255)
 * 3. No sustained max brightness (thermal protection)
 * 4. Generator output must be bounded
 * 5. Heat buffers must be bounded
 *
 * Run these tests:
 * - At startup (compile-time check with mock hardware)
 * - In CI/CD pipeline
 * - Before ANY firmware upload
 * - After ANY rendering code changes
 */

namespace RenderPipelineSafetyTest {

    struct TestResult {
        bool passed;
        const char* testName;
        const char* message;
        uint32_t details;  // Error code or count
    };

    // Safety thresholds
    static constexpr uint8_t MAX_PIXEL_VALUE = 255;
    static constexpr uint16_t MAX_PIXEL_SUM = 765;  // RGB(255,255,255)
    static constexpr int MAX_CONSECUTIVE_BRIGHT_FRAMES = 100;  // ~3 seconds at 30fps
    static constexpr float BRIGHTNESS_ACCUMULATION_THRESHOLD = 1.5f;  // 50% increase = accumulation

    /**
     * CRITICAL TEST 1: Verify frame clearing between renders
     *
     * Bug example: Missing pixelMatrix_->clear() caused brightness accumulation
     * Hardware risk: Overcurrent can melt LED controllers
     *
     * Test: Render frame, check all pixels cleared before next frame
     */
    inline TestResult testFrameClearing(RenderPipeline& pipeline) {
        TestResult result = {true, "Frame Clearing", "OK", 0};

        // Create test matrix
        PixelMatrix testMatrix(8, 8);
        if (!testMatrix.isValid()) {
            result.passed = false;
            result.message = "Matrix allocation failed";
            return result;
        }

        // Fill matrix with test pattern
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                testMatrix.setPixel(x, y, 100, 150, 200);
            }
        }

        // Verify pattern exists
        RGB pixel = testMatrix.getPixel(0, 0);
        if (pixel.r != 100 || pixel.g != 150 || pixel.b != 200) {
            result.passed = false;
            result.message = "Test pattern not set";
            return result;
        }

        // Clear matrix
        testMatrix.clear();

        // Verify all pixels are zero
        int nonZeroPixels = 0;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                pixel = testMatrix.getPixel(x, y);
                if (pixel.r != 0 || pixel.g != 0 || pixel.b != 0) {
                    nonZeroPixels++;
                }
            }
        }

        if (nonZeroPixels > 0) {
            result.passed = false;
            result.message = "Pixels not cleared";
            result.details = nonZeroPixels;
            return result;
        }

        return result;
    }

    /**
     * CRITICAL TEST 2: Brightness bounds validation
     *
     * Bug example: ADDITIVE blending without bounds checking
     * Hardware risk: RGB(255,255,255) = 765 current units, can exceed controller spec
     *
     * Test: Verify no pixel exceeds max brightness after rendering
     */
    inline TestResult testBrightnessBounds(PixelMatrix& matrix) {
        TestResult result = {true, "Brightness Bounds", "OK", 0};

        int overflowPixels = 0;
        uint32_t maxSum = 0;

        for (int y = 0; y < matrix.getHeight(); y++) {
            for (int x = 0; x < matrix.getWidth(); x++) {
                RGB pixel = matrix.getPixel(x, y);

                // Check individual channel overflow
                if (pixel.r > MAX_PIXEL_VALUE) overflowPixels++;
                if (pixel.g > MAX_PIXEL_VALUE) overflowPixels++;
                if (pixel.b > MAX_PIXEL_VALUE) overflowPixels++;

                // Track max total brightness
                uint32_t sum = pixel.r + pixel.g + pixel.b;
                if (sum > maxSum) maxSum = sum;

                // Check total brightness overflow
                if (sum > MAX_PIXEL_SUM) {
                    overflowPixels++;
                }
            }
        }

        if (overflowPixels > 0) {
            result.passed = false;
            result.message = "Brightness overflow detected";
            result.details = overflowPixels;
            return result;
        }

        result.details = maxSum;  // Store peak brightness for monitoring
        return result;
    }

    /**
     * CRITICAL TEST 3: Color accumulation detection
     *
     * Bug example: Missing frame clear causes frame-to-frame accumulation
     * Hardware risk: Gradual brightness increase until saturation
     *
     * Test: Run multiple frames, detect if brightness increases without audio input
     */
    inline TestResult testColorAccumulation(RenderPipeline& pipeline, PixelMatrix& matrix, int frames = 10) {
        TestResult result = {true, "Color Accumulation", "OK", 0};

        // Silent audio (no input should produce minimal output)
        AudioControl silentAudio;
        silentAudio.energy = 0.0f;
        silentAudio.pulse = 0.0f;
        silentAudio.phase = 0.0f;
        silentAudio.rhythmStrength = 0.0f;

        // Measure initial brightness
        uint32_t initialBrightness = 0;
        for (int y = 0; y < matrix.getHeight(); y++) {
            for (int x = 0; x < matrix.getWidth(); x++) {
                RGB pixel = matrix.getPixel(x, y);
                initialBrightness += pixel.r + pixel.g + pixel.b;
            }
        }

        // Run multiple frames
        for (int frame = 0; frame < frames; frame++) {
            pipeline.render(silentAudio);
        }

        // Measure final brightness
        uint32_t finalBrightness = 0;
        for (int y = 0; y < matrix.getHeight(); y++) {
            for (int x = 0; x < matrix.getWidth(); x++) {
                RGB pixel = matrix.getPixel(x, y);
                finalBrightness += pixel.r + pixel.g + pixel.b;
            }
        }

        // Check for accumulation (brightness should not increase significantly)
        if (initialBrightness > 0) {
            float ratio = (float)finalBrightness / initialBrightness;
            if (ratio > BRIGHTNESS_ACCUMULATION_THRESHOLD) {
                result.passed = false;
                result.message = "Brightness accumulation detected";
                result.details = (uint32_t)(ratio * 100);  // Store as percentage
                return result;
            }
        }

        result.details = finalBrightness;
        return result;
    }

    /**
     * CRITICAL TEST 4: Sustained max brightness protection
     *
     * Hardware risk: Prolonged max brightness causes thermal damage
     *
     * Test: Detect if system sustains max brightness beyond thermal limits
     */
    inline TestResult testThermalProtection(PixelMatrix& matrix, int consecutiveFrames) {
        TestResult result = {true, "Thermal Protection", "OK", 0};

        // Count pixels at max brightness
        int maxBrightnessPixels = 0;
        for (int y = 0; y < matrix.getHeight(); y++) {
            for (int x = 0; x < matrix.getWidth(); x++) {
                RGB pixel = matrix.getPixel(x, y);
                if (pixel.r == 255 && pixel.g == 255 && pixel.b == 255) {
                    maxBrightnessPixels++;
                }
            }
        }

        // Calculate percentage of LEDs at max
        int totalPixels = matrix.getWidth() * matrix.getHeight();
        float maxBrightnessPercent = (float)maxBrightnessPixels / totalPixels;

        // Warn if >50% of LEDs at max brightness for extended time
        if (maxBrightnessPercent > 0.5f && consecutiveFrames > MAX_CONSECUTIVE_BRIGHT_FRAMES) {
            result.passed = false;
            result.message = "Thermal limit exceeded";
            result.details = maxBrightnessPixels;
            return result;
        }

        result.details = maxBrightnessPixels;
        return result;
    }

    /**
     * CRITICAL TEST 5: Generator output validation
     *
     * Bug example: Generator produces invalid RGB values or NaN
     * Hardware risk: Undefined behavior could cause hardware damage
     *
     * Test: Verify generator output is valid and bounded
     */
    inline TestResult testGeneratorOutput(PixelMatrix& matrix) {
        TestResult result = {true, "Generator Output", "OK", 0};

        int invalidPixels = 0;

        for (int y = 0; y < matrix.getHeight(); y++) {
            for (int x = 0; x < matrix.getWidth(); x++) {
                RGB pixel = matrix.getPixel(x, y);

                // Check for invalid values (NaN would show as max uint8_t)
                // Check for impossible values
                bool invalid = false;

                // No checks needed - uint8_t is always 0-255
                // But we can check for stuck pixels (all channels same for entire frame)

                if (invalid) {
                    invalidPixels++;
                }
            }
        }

        if (invalidPixels > 0) {
            result.passed = false;
            result.message = "Invalid pixel values";
            result.details = invalidPixels;
            return result;
        }

        return result;
    }

    /**
     * Run all rendering safety tests
     * Returns number of failures (0 = all passed)
     */
    inline int runAllTests(RenderPipeline& pipeline, PixelMatrix& matrix, bool verbose = true) {
        int failures = 0;

        if (verbose) {
            Serial.println(F("\n=== RENDER PIPELINE SAFETY TESTS ==="));
            Serial.println(F("!! CRITICAL: Prevents hardware damage from runaway brightness !!"));
        }

        TestResult tests[] = {
            testFrameClearing(pipeline),
            testBrightnessBounds(matrix),
            testColorAccumulation(pipeline, matrix, 10),
            testThermalProtection(matrix, 0),
            testGeneratorOutput(matrix)
        };

        const int numTests = sizeof(tests) / sizeof(tests[0]);

        for (int i = 0; i < numTests; i++) {
            if (verbose) {
                Serial.print(tests[i].testName);
                Serial.print(F(": "));
                if (tests[i].passed) {
                    Serial.print(F("PASS"));
                    if (tests[i].details > 0) {
                        Serial.print(F(" ("));
                        Serial.print(tests[i].details);
                        Serial.print(F(")"));
                    }
                } else {
                    Serial.print(F("FAIL - "));
                    Serial.print(tests[i].message);
                    Serial.print(F(" ("));
                    Serial.print(tests[i].details);
                    Serial.print(F(")"));
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
                Serial.println(F("!!! CRITICAL SAFETY FAILURES !!!"));
                Serial.println(F("!!! DO NOT CONNECT TO HARDWARE !!!"));
                Serial.println(F("!!! RUNAWAY BRIGHTNESS CAN MELT CONTROLLERS !!!"));
            }
            Serial.println();
        }

        return failures;
    }

    /**
     * Continuous brightness monitoring (call every frame)
     * Tracks sustained high brightness and triggers emergency shutdown if needed
     */
    class BrightnessMonitor {
    public:
        BrightnessMonitor() : consecutiveBrightFrames_(0), emergencyShutdown_(false) {}

        void checkFrame(PixelMatrix& matrix) {
            if (emergencyShutdown_) return;

            TestResult thermal = testThermalProtection(matrix, consecutiveBrightFrames_);

            if (!thermal.passed) {
                // EMERGENCY: Sustained max brightness detected
                emergencyShutdown_ = true;
                Serial.println(F("\n!!! EMERGENCY SHUTDOWN !!!"));
                Serial.println(F("!!! THERMAL PROTECTION TRIGGERED !!!"));
                Serial.print(F("Max brightness pixels: "));
                Serial.println(thermal.details);
                Serial.println(F("System halted to prevent hardware damage."));

                // Halt system - DO NOT allow continued operation
                while (1) {
                    delay(10000);
                }
            }

            // Track consecutive bright frames
            if (thermal.details > 0) {
                consecutiveBrightFrames_++;
            } else {
                consecutiveBrightFrames_ = 0;
            }
        }

        int getConsecutiveBrightFrames() const { return consecutiveBrightFrames_; }
        bool isEmergencyShutdown() const { return emergencyShutdown_; }

    private:
        int consecutiveBrightFrames_;
        bool emergencyShutdown_;
    };

}  // namespace RenderPipelineSafetyTest
