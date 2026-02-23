#include "EffectRendererTest.h"
#include <Arduino.h>

EffectRendererTest::EffectRendererTest() : testsRun(0), testsPassed(0) {
}

bool EffectRendererTest::runAllTests() {
    Serial.println(F("=== EffectRenderer Test Suite ==="));

    testsRun = 0;
    testsPassed = 0;

    // Run individual tests
    printTestResult("Initialization", testInitialization());
    printTestResult("Matrix Rendering", testMatrixRendering());
    printTestResult("Color Output", testColorOutput());
    printTestResult("Brightness Control", testBrightnessControl());
    printTestResult("Different Sizes", testDifferentSizes());
    printTestResult("Edge Cases", testEdgeCases());

    printResults();
    return testsPassed == testsRun;
}

bool EffectRendererTest::testInitialization() {
    logTestInfo("Testing EffectRenderer initialization");

    // Test initialization with different parameters
    EffectRenderer renderer1(10);
    EffectRenderer renderer2(50);
    EffectRenderer renderer3(1);

    // Test that renderers can be used without crashing
    PixelMatrix matrix1(10, 1);
    PixelMatrix matrix2(50, 1);
    PixelMatrix matrix3(1, 1);

    // Fill matrices with test data
    matrix1.setPixel(0, 0, createColor(255, 0, 0));
    matrix2.setPixel(0, 0, createColor(0, 255, 0));
    matrix3.setPixel(0, 0, createColor(0, 0, 255));

    // Render matrices (should not crash)
    renderer1.render(matrix1);
    renderer2.render(matrix2);
    renderer3.render(matrix3);

    return true; // If we get here without crashing, test passes
}

bool EffectRendererTest::testMatrixRendering() {
    logTestInfo("Testing matrix to LED mapping");

    EffectRenderer renderer(9); // 3x3 matrix
    PixelMatrix matrix(3, 3);

    // Fill matrix with known pattern
    const uint32_t testColors[] = {
        createColor(255, 0, 0),    // Red
        createColor(0, 255, 0),    // Green
        createColor(0, 0, 255),    // Blue
        createColor(255, 255, 0),  // Yellow
        createColor(255, 0, 255),  // Magenta
        createColor(0, 255, 255),  // Cyan
        createColor(128, 128, 128),// Gray
        createColor(255, 128, 0),  // Orange
        createColor(128, 0, 128)   // Purple
    };

    int colorIndex = 0;
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            matrix.setPixel(x, y, testColors[colorIndex]);
            colorIndex++;
        }
    }

    // Render the matrix
    renderer.render(matrix);

    // For this test, we just verify it doesn't crash
    // In a real hardware test, we'd verify the LED outputs
    return true;
}

bool EffectRendererTest::testColorOutput() {
    logTestInfo("Testing color accuracy");

    EffectRenderer renderer(4);
    PixelMatrix matrix(2, 2);

    // Test primary colors
    uint32_t red = createColor(255, 0, 0);
    uint32_t green = createColor(0, 255, 0);
    uint32_t blue = createColor(0, 0, 255);
    uint32_t white = createColor(255, 255, 255);

    matrix.setPixel(0, 0, red);
    matrix.setPixel(1, 0, green);
    matrix.setPixel(0, 1, blue);
    matrix.setPixel(1, 1, white);

    // Render and verify colors are preserved
    renderer.render(matrix);

    // Verify the colors in the matrix are still correct
    bool colorsPreserved = (matrix.getPixel(0, 0) == red) &&
                          (matrix.getPixel(1, 0) == green) &&
                          (matrix.getPixel(0, 1) == blue) &&
                          (matrix.getPixel(1, 1) == white);

    return colorsPreserved;
}

bool EffectRendererTest::testBrightnessControl() {
    logTestInfo("Testing brightness scaling");

    EffectRenderer renderer(2);
    PixelMatrix matrix(1, 2);

    // Test full brightness color
    uint32_t fullColor = createColor(200, 100, 50);
    matrix.setPixel(0, 0, fullColor);
    matrix.setPixel(0, 1, fullColor);

    // Test brightness scaling if available
    // (Note: Actual brightness control implementation may vary)
    renderer.render(matrix);

    // For this test, we verify rendering doesn't crash with brightness
    // In real hardware, we'd test actual LED brightness levels
    return true;
}

bool EffectRendererTest::testDifferentSizes() {
    logTestInfo("Testing various matrix sizes");

    // Test different common LED configurations
    struct TestCase {
        int width, height, ledCount;
    };

    TestCase testCases[] = {
        {1, 1, 1},      // Single LED
        {8, 1, 8},      // LED strip
        {1, 8, 8},      // Vertical strip
        {4, 4, 16},     // Small matrix
        {8, 8, 64},     // Medium matrix
        {16, 1, 16},    // Long strip
    };

    for (int i = 0; i < 6; i++) {
        TestCase& tc = testCases[i];

        EffectRenderer renderer(tc.ledCount);
        PixelMatrix matrix(tc.width, tc.height);

        // Fill with gradient pattern
        for (int y = 0; y < tc.height; y++) {
            for (int x = 0; x < tc.width; x++) {
                uint8_t intensity = (uint8_t)((x + y) * 255 / (tc.width + tc.height - 2));
                matrix.setPixel(x, y, createColor(intensity, intensity/2, intensity/4));
            }
        }

        // Render (should not crash)
        renderer.render(matrix);
    }

    return true;
}

bool EffectRendererTest::testEdgeCases() {
    logTestInfo("Testing edge cases and error conditions");

    EffectRenderer renderer(4);
    PixelMatrix matrix(2, 2);

    // Test with all black (should work)
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            matrix.setPixel(x, y, createColor(0, 0, 0));
        }
    }
    renderer.render(matrix);

    // Test with all white (should work)
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            matrix.setPixel(x, y, createColor(255, 255, 255));
        }
    }
    renderer.render(matrix);

    // Test with random colors (should work)
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            uint8_t r = random(256);
            uint8_t g = random(256);
            uint8_t b = random(256);
            matrix.setPixel(x, y, createColor(r, g, b));
        }
    }
    renderer.render(matrix);

    // Test rapid successive renders (should work)
    for (int i = 0; i < 10; i++) {
        renderer.render(matrix);
        delay(1);
    }

    return true;
}

uint32_t EffectRendererTest::createColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void EffectRendererTest::extractRGB(uint32_t color, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

void EffectRendererTest::logTestInfo(const char* info) {
    Serial.print(F("  - "));
    Serial.println(info);
}

void EffectRendererTest::printResults() {
    Serial.println();
    Serial.println(F("=== EffectRenderer Test Results ==="));
    Serial.print(F("Tests Run: "));
    Serial.println(testsRun);
    Serial.print(F("Tests Passed: "));
    Serial.println(testsPassed);
    Serial.print(F("Tests Failed: "));
    Serial.println(testsRun - testsPassed);

    if (testsPassed == testsRun) {
        Serial.println(F("✅ All EffectRenderer tests PASSED!"));
    } else {
        Serial.println(F("❌ Some EffectRenderer tests FAILED!"));
    }
    Serial.println();
}

void EffectRendererTest::printTestResult(const char* testName, bool passed) {
    testsRun++;
    if (passed) {
        testsPassed++;
        Serial.print(F("✅ "));
    } else {
        Serial.print(F("❌ "));
    }
    Serial.print(testName);
    Serial.println(passed ? F(" - PASSED") : F(" - FAILED"));
}
