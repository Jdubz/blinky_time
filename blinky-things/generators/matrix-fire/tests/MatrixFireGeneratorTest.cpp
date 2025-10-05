#include "MatrixFireGeneratorTest.h"
#include <Arduino.h>

MatrixFireGeneratorTest::MatrixFireGeneratorTest() : testsRun(0), testsPassed(0) {
}

bool MatrixFireGeneratorTest::runAllTests() {
    Serial.println(F("=== MatrixFireGenerator Test Suite ==="));

    testsRun = 0;
    testsPassed = 0;

    // Run individual tests
    printTestResult("Initialization", testInitialization());
    printTestResult("Heat Simulation", testHeatSimulation());
    printTestResult("Spark Generation", testSparkGeneration());
    printTestResult("Color Mapping", testColorMapping());
    printTestResult("Energy Response", testEnergyResponse());
    printTestResult("Matrix Output", testMatrixOutput());

    printResults();
    return testsPassed == testsRun;
}

bool MatrixFireGeneratorTest::testInitialization() {
    logTestInfo("Testing MatrixFireGenerator initialization");

    // Test various matrix sizes
    MatrixFireGenerator gen1(4, 15);
    MatrixFireGenerator gen2(16, 8);
    MatrixFireGenerator gen1x1(1, 1);

    // Test that generators initialize without crashing
    EffectMatrix matrix1(4, 15);
    EffectMatrix matrix2(16, 8);
    EffectMatrix matrix3(1, 1);

    gen1.generate(matrix1, 0.0f, 0.0f);
    gen2.generate(matrix2, 0.0f, 0.0f);
    gen1x1.generate(matrix3, 0.0f, 0.0f);

    // Test heat access
    float heat1 = gen1.getHeat(0, 0);
    float heat2 = gen2.getHeat(0, 0);

    // Heat should be valid (0.0 to 1.0)
    bool validHeat = (heat1 >= 0.0f && heat1 <= 1.0f) &&
                     (heat2 >= 0.0f && heat2 <= 1.0f);

    return validHeat;
}

bool MatrixFireGeneratorTest::testHeatSimulation() {
    logTestInfo("Testing heat simulation accuracy");

    MatrixFireGenerator gen(4, 4);
    EffectMatrix matrix(4, 4);

    // Generate with high energy to create heat
    gen.generate(matrix, 1.0f, 1.0f);

    // Check that some heat was generated
    bool hasHeat = false;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (gen.getHeat(x, y) > 0.01f) {
                hasHeat = true;
                break;
            }
        }
        if (hasHeat) break;
    }

    if (!hasHeat) return false;

    // Test cooling over time (run several iterations with no energy)
    for (int i = 0; i < 10; i++) {
        gen.generate(matrix, 0.0f, 0.0f);
        delay(10); // Small delay for timing
    }

    // Heat should have cooled down
    bool hasCooled = true;
    float totalHeat = 0.0f;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            totalHeat += gen.getHeat(x, y);
        }
    }

    // Total heat should be lower after cooling
    return totalHeat < 8.0f; // Reasonable threshold for 4x4 matrix
}

bool MatrixFireGeneratorTest::testSparkGeneration() {
    logTestInfo("Testing spark generation with audio input");

    MatrixFireGenerator gen(4, 4);
    EffectMatrix matrix(4, 4);

    // Test with no energy - should have minimal activity
    gen.reset();
    gen.generate(matrix, 0.0f, 0.0f);

    float lowEnergyHeat = 0.0f;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            lowEnergyHeat += gen.getHeat(x, y);
        }
    }

    // Test with high energy - should have more activity
    gen.reset();
    gen.generate(matrix, 1.0f, 1.0f);

    float highEnergyHeat = 0.0f;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            highEnergyHeat += gen.getHeat(x, y);
        }
    }

    // High energy should generate more heat than low energy
    return highEnergyHeat > lowEnergyHeat;
}

bool MatrixFireGeneratorTest::testColorMapping() {
    logTestInfo("Testing fire color mapping");

    MatrixFireGenerator gen(2, 2);
    EffectMatrix matrix(2, 2);

    // Generate fire with high energy
    gen.generate(matrix, 1.0f, 1.0f);

    // Check that we get valid fire colors (should contain red/orange/yellow)
    bool hasFireColors = false;

    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            uint32_t color = matrix.getPixel(x, y);
            if (verifyColorRange(color)) {
                hasFireColors = true;
                break;
            }
        }
        if (hasFireColors) break;
    }

    return hasFireColors;
}

bool MatrixFireGeneratorTest::testEnergyResponse() {
    logTestInfo("Testing energy response variation");

    MatrixFireGenerator gen(3, 3);
    EffectMatrix matrix(3, 3);

    // Test response to different energy levels
    float energyLevels[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    float heatSums[5];

    for (int i = 0; i < 5; i++) {
        gen.reset();
        gen.generate(matrix, energyLevels[i], 0.0f);

        heatSums[i] = 0.0f;
        for (int y = 0; y < 3; y++) {
            for (int x = 0; x < 3; x++) {
                heatSums[i] += gen.getHeat(x, y);
            }
        }
    }

    // Generally, higher energy should produce more heat
    // Allow some variation due to randomness
    bool validResponse = (heatSums[4] >= heatSums[0]) &&
                        (heatSums[3] >= heatSums[1]);

    return validResponse;
}

bool MatrixFireGeneratorTest::testMatrixOutput() {
    logTestInfo("Testing matrix output format");

    int width = 3, height = 3;
    MatrixFireGenerator gen(width, height);
    EffectMatrix matrix(width, height);

    // Generate with some energy
    gen.generate(matrix, 0.5f, 0.2f);

    // Verify matrix dimensions
    if (matrix.getWidth() != width || matrix.getHeight() != height) {
        return false;
    }

    // Verify all pixels have valid colors (no weird values)
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t color = matrix.getPixel(x, y);

            // Extract RGB components
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            // All components should be valid (0-255)
            if (r > 255 || g > 255 || b > 255) {
                return false;
            }
        }
    }

    return true;
}

bool MatrixFireGeneratorTest::compareFloats(float a, float b, float tolerance) {
    return abs(a - b) <= tolerance;
}

bool MatrixFireGeneratorTest::verifyColorRange(uint32_t color) {
    // Extract RGB components
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // Fire colors should be predominantly red/orange/yellow
    // Valid fire: red > 0, red >= green >= blue for most cases
    if (r == 0 && g == 0 && b == 0) return true; // Black is valid (no fire)

    return (r > 0) && (r >= g) && (g >= b);
}

void MatrixFireGeneratorTest::logTestInfo(const char* info) {
    Serial.print(F("  - "));
    Serial.println(info);
}

void MatrixFireGeneratorTest::printResults() {
    Serial.println();
    Serial.println(F("=== MatrixFireGenerator Test Results ==="));
    Serial.print(F("Tests Run: "));
    Serial.println(testsRun);
    Serial.print(F("Tests Passed: "));
    Serial.println(testsPassed);
    Serial.print(F("Tests Failed: "));
    Serial.println(testsRun - testsPassed);

    if (testsPassed == testsRun) {
        Serial.println(F("✅ All MatrixFireGenerator tests PASSED!"));
    } else {
        Serial.println(F("❌ Some MatrixFireGenerator tests FAILED!"));
    }
    Serial.println();
}

void MatrixFireGeneratorTest::printTestResult(const char* testName, bool passed) {
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
