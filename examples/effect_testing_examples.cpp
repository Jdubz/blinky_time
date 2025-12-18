/**
 * Effect Testing Examples - How to use GeneralEffectTests
 *
 * This example shows how to test any effect using the universal
 * test suite that applies to all effects.
 */

#include "effects/tests/GeneralEffectTests.h"
#include "effects/HueRotationEffect.h"
#include "effects/NoOpEffect.h"

// Example: Test all effects with general tests
void testAllEffects() {
    Serial.println(F("=== Testing All Effects ==="));

    // Create effect instances
    HueRotationEffect hueEffect;
    NoOpEffect noOpEffect;

    // Test each effect with general tests
    Effect* effects[] = {
        &hueEffect,
        &noOpEffect
    };

    const char* effectNames[] = {
        "HueRotation",
        "NoOp"
    };

    int numEffects = 2;
    int totalPassed = 0;
    int totalFailed = 0;

    for (int i = 0; i < numEffects; i++) {
        Serial.print(F("\n--- Testing "));
        Serial.print(effectNames[i]);
        Serial.println(F(" Effect ---"));

        GeneralEffectTests tests(effects[i], 8, 8); // Test with 8x8 matrix
        tests.runAllTests();

        totalPassed += tests.getTestsPassed();
        totalFailed += tests.getTestsFailed();
    }

    // Summary
    Serial.println(F("\n=== Overall Test Summary ==="));
    Serial.print(F("Total tests passed: "));
    Serial.println(totalPassed);
    Serial.print(F("Total tests failed: "));
    Serial.println(totalFailed);

    if (totalFailed == 0) {
        Serial.println(F("🎉 All effects PASSED general tests!"));
    } else {
        Serial.println(F("⚠️  Some effects failed general tests"));
    }
}

// Example: Test specific effect functionality
void testSpecificEffect(Effect* effect, const char* name) {
    Serial.print(F("Testing "));
    Serial.print(name);
    Serial.println(F(" effect..."));

    GeneralEffectTests tests(effect, 4, 4);

    // Run only basic tests
    tests.runBasicTests();

    if (tests.allTestsPassed()) {
        Serial.print(name);
        Serial.println(F(" passed basic tests ✅"));
    } else {
        Serial.print(name);
        Serial.println(F(" failed basic tests ❌"));
    }
}

// Example: Performance testing
void performanceTestEffects() {
    Serial.println(F("=== Effect Performance Testing ==="));

    HueRotationEffect hueEffect;
    NoOpEffect noOpEffect;

    // Test with larger matrix for performance
    GeneralEffectTests hueTests(&hueEffect, 16, 16);
    GeneralEffectTests noOpTests(&noOpEffect, 16, 16);

    Serial.println(F("Testing HueRotation performance:"));
    hueTests.testPerformanceConstraints();

    Serial.println(F("Testing NoOp performance:"));
    noOpTests.testPerformanceConstraints();
}

// Example: Integration with main loop
void runEffectValidation() {
    Serial.println(F("Starting effect validation..."));

    // Quick validation of all effects
    NoOpEffect noOp;
    GeneralEffectTests quickTests(&noOp, 2, 2);

    if (quickTests.testBasicInterface() &&
        quickTests.testMatrixSafety() &&
        quickTests.testResetFunctionality()) {
        Serial.println(F("✅ Effect system operational"));
    } else {
        Serial.println(F("❌ Effect system has issues"));
    }
}

/*
 * USAGE NOTES:
 *
 * 1. General tests apply to ALL effects:
 *    - Interface compliance (name, apply, reset methods)
 *    - Matrix safety (no crashes, valid data)
 *    - Null handling (graceful failure)
 *    - Performance constraints (reasonable execution time)
 *
 * 2. NoOp effect is perfect for:
 *    - Testing generator output directly
 *    - Baseline performance comparison
 *    - Debugging when effects cause issues
 *    - Simple pass-through scenarios
 *
 * 3. Add specific effect tests by:
 *    - Creating new test classes that inherit from GeneralEffectTests
 *    - Adding effect-specific test methods
 *    - Testing unique behaviors of each effect
 *
 * 4. Integration:
 *    - Call testAllEffects() in setup() for full validation
 *    - Use runEffectValidation() for quick checks
 *    - Add #ifdef ENABLE_TESTING guards for production builds
 */
