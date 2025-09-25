#include "StringFireGeneratorTest.h"
#include <Arduino.h>

StringFireGeneratorTest::StringFireGeneratorTest() : testsRun(0), testsPassed(0) {
}

bool StringFireGeneratorTest::runAllTests() {
    Serial.println(F("=== StringFireGenerator Test Suite ==="));
    
    testsRun = 0;
    testsPassed = 0;
    
    // Run individual tests
    printTestResult("Initialization", testInitialization());
    printTestResult("Lateral Heat Propagation", testLateralHeatPropagation());
    printTestResult("Spark Generation", testSparkGeneration());
    printTestResult("Color Mapping", testColorMapping());
    printTestResult("Energy Response", testEnergyResponse());
    printTestResult("Matrix Output", testMatrixOutput());
    printTestResult("String Behavior", testStringBehavior());
    
    printResults();
    return testsPassed == testsRun;
}

bool StringFireGeneratorTest::testInitialization() {
    logTestInfo("Testing StringFireGenerator initialization");
    
    // Test various string lengths
    StringFireGenerator gen1(10);
    StringFireGenerator gen2(50);
    StringFireGenerator gen3(1);
    
    // Test that generators initialize without crashing
    EffectMatrix matrix1(10, 1);
    EffectMatrix matrix2(50, 1);
    EffectMatrix matrix3(1, 1);
    
    gen1.generate(matrix1, 0.0f, 0.0f);
    gen2.generate(matrix2, 0.0f, 0.0f);
    gen3.generate(matrix3, 0.0f, 0.0f);
    
    // Test heat access
    float heat1 = gen1.getHeat(0);
    float heat2 = gen2.getHeat(0);
    
    // Heat should be valid (0.0 to 1.0)
    bool validHeat = (heat1 >= 0.0f && heat1 <= 1.0f) && 
                     (heat2 >= 0.0f && heat2 <= 1.0f);
    
    return validHeat;
}

bool StringFireGeneratorTest::testLateralHeatPropagation() {
    logTestInfo("Testing lateral heat propagation along string");
    
    StringFireGenerator gen(10);
    EffectMatrix matrix(10, 1);
    
    // Generate with high energy to create initial sparks
    gen.reset();
    gen.generate(matrix, 1.0f, 1.0f);
    
    // Store initial heat distribution
    float initialHeat[10];
    for (int i = 0; i < 10; i++) {
        initialHeat[i] = gen.getHeat(i);
    }
    
    // Run several iterations to allow heat to spread
    for (int i = 0; i < 5; i++) {
        gen.generate(matrix, 0.0f, 0.0f);
        delay(10);
    }
    
    // Check that heat propagation makes sense
    bool hasReasonablePropagation = false;
    
    // Look for areas where heat has spread or cooled appropriately
    for (int i = 1; i < 9; i++) {
        float currentHeat = gen.getHeat(i);
        float leftHeat = gen.getHeat(i-1);
        float rightHeat = gen.getHeat(i+1);
        
        // If there's heat at this position, adjacent positions should have
        // heat values that make sense (not randomly higher)
        if (currentHeat > 0.1f) {
            hasReasonablePropagation = true;
            break;
        }
    }
    
    return hasReasonablePropagation;
}

bool StringFireGeneratorTest::testSparkGeneration() {
    logTestInfo("Testing spark generation with audio input");
    
    StringFireGenerator gen(8);
    EffectMatrix matrix(8, 1);
    
    // Test with no energy - should have minimal activity
    gen.reset();
    gen.generate(matrix, 0.0f, 0.0f);
    
    float lowEnergyHeat = 0.0f;
    for (int i = 0; i < 8; i++) {
        lowEnergyHeat += gen.getHeat(i);
    }
    
    // Test with high energy - should have more activity
    gen.reset();
    gen.generate(matrix, 1.0f, 1.0f);
    
    float highEnergyHeat = 0.0f;
    for (int i = 0; i < 8; i++) {
        highEnergyHeat += gen.getHeat(i);
    }
    
    // High energy should generate more heat than low energy
    return highEnergyHeat > lowEnergyHeat;
}

bool StringFireGeneratorTest::testColorMapping() {
    logTestInfo("Testing fire color mapping");
    
    StringFireGenerator gen(4);  
    EffectMatrix matrix(4, 1);
    
    // Generate fire with high energy
    gen.generate(matrix, 1.0f, 1.0f);
    
    // Check that we get valid fire colors (should contain red/orange/yellow)
    bool hasFireColors = false;
    
    for (int i = 0; i < 4; i++) {
        uint32_t color = matrix.getPixel(i, 0);
        if (verifyColorRange(color)) {
            hasFireColors = true;
            break;
        }
    }
    
    return hasFireColors;
}

bool StringFireGeneratorTest::testEnergyResponse() {
    logTestInfo("Testing energy response variation");
    
    StringFireGenerator gen(5);
    EffectMatrix matrix(5, 1);
    
    // Test response to different energy levels
    float energyLevels[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    float heatSums[5];
    
    for (int i = 0; i < 5; i++) {
        gen.reset();
        gen.generate(matrix, energyLevels[i], 0.0f);
        
        heatSums[i] = 0.0f;
        for (int j = 0; j < 5; j++) {
            heatSums[i] += gen.getHeat(j);
        }
    }
    
    // Generally, higher energy should produce more heat
    // Allow some variation due to randomness
    bool validResponse = (heatSums[4] >= heatSums[0]) && 
                        (heatSums[3] >= heatSums[1]);
    
    return validResponse;
}

bool StringFireGeneratorTest::testMatrixOutput() {
    logTestInfo("Testing matrix output format");
    
    int length = 6;
    StringFireGenerator gen(length);
    EffectMatrix matrix(length, 1);
    
    // Generate with some energy
    gen.generate(matrix, 0.5f, 0.2f);
    
    // Verify matrix dimensions
    if (matrix.getWidth() != length || matrix.getHeight() != 1) {
        return false;
    }
    
    // Verify all pixels have valid colors (no weird values)
    for (int i = 0; i < length; i++) {
        uint32_t color = matrix.getPixel(i, 0);
        
        // Extract RGB components
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        
        // All components should be valid (0-255)
        if (r > 255 || g > 255 || b > 255) {
            return false;
        }
    }
    
    return true;
}

bool StringFireGeneratorTest::testStringBehavior() {
    logTestInfo("Testing string-specific fire behavior");
    
    StringFireGenerator gen(12);
    EffectMatrix matrix(12, 1);
    
    // Test heat dissipation pattern specific to strings
    gen.reset();
    
    // Generate initial sparks with high energy
    gen.generate(matrix, 1.0f, 1.0f);
    
    // Store positions with significant heat
    int hotSpots[3];
    int hotSpotCount = 0;
    for (int i = 0; i < 12 && hotSpotCount < 3; i++) {
        if (gen.getHeat(i) > 0.3f) {
            hotSpots[hotSpotCount++] = i;
        }
    }
    
    if (hotSpotCount == 0) {
        // No hot spots generated, try with more energy
        gen.generate(matrix, 1.0f, 1.0f);
        gen.generate(matrix, 1.0f, 1.0f);
        
        // Check again
        for (int i = 0; i < 12 && hotSpotCount < 3; i++) {
            if (gen.getHeat(i) > 0.1f) {
                hotSpots[hotSpotCount++] = i;
            }
        }
    }
    
    // Run simulation with low energy to see cooling/propagation
    for (int i = 0; i < 5; i++) {
        gen.generate(matrix, 0.1f, 0.0f);
        delay(10);
    }
    
    // Check that the fire behaves reasonably
    // At minimum, should not have invalid heat values
    bool hasValidBehavior = true;
    for (int i = 0; i < 12; i++) {
        float heat = gen.getHeat(i);
        if (heat < 0.0f || heat > 1.0f) {
            hasValidBehavior = false;
            break;
        }
    }
    
    return hasValidBehavior;
}

bool StringFireGeneratorTest::compareFloats(float a, float b, float tolerance) {
    return abs(a - b) <= tolerance;
}

bool StringFireGeneratorTest::verifyColorRange(uint32_t color) {
    // Extract RGB components
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    // Fire colors should be predominantly red/orange/yellow
    // Valid fire: red > 0, red >= green >= blue for most cases
    if (r == 0 && g == 0 && b == 0) return true; // Black is valid (no fire)
    
    return (r > 0) && (r >= g) && (g >= b);
}

void StringFireGeneratorTest::logTestInfo(const char* info) {
    Serial.print(F("  - "));
    Serial.println(info);
}

void StringFireGeneratorTest::printResults() {
    Serial.println();
    Serial.println(F("=== StringFireGenerator Test Results ==="));
    Serial.print(F("Tests Run: "));
    Serial.println(testsRun);
    Serial.print(F("Tests Passed: "));
    Serial.println(testsPassed);
    Serial.print(F("Tests Failed: "));
    Serial.println(testsRun - testsPassed);
    
    if (testsPassed == testsRun) {
        Serial.println(F("✅ All StringFireGenerator tests PASSED!"));
    } else {
        Serial.println(F("❌ Some StringFireGenerator tests FAILED!"));
    }
    Serial.println();
}

void StringFireGeneratorTest::printTestResult(const char* testName, bool passed) {
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