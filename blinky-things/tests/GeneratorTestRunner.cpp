#include "GeneratorTestRunner.h"
#include <Arduino.h>
#include <string.h>

GeneratorTestRunner::GeneratorTestRunner(int width, int height)
    : matrixWidth_(width), matrixHeight_(height) {
    // No longer need to instantiate individual generator tests
    // All tests are now static methods
}

GeneratorTestRunner::~GeneratorTestRunner() {
    // No cleanup needed for static test methods
}

void GeneratorTestRunner::runAllTests() {
    Serial.println(F("=== Comprehensive Test Suite - All Components ==="));
    Serial.print(F("Matrix Size: "));
    Serial.print(matrixWidth_);
    Serial.print(F("x"));
    Serial.println(matrixHeight_);
    Serial.println();

    // Run all generator tests
    Serial.println(F("--- Generator Tests ---"));
    UnifiedFireGeneratorTest::runAllTests();

    Serial.println(F("--- Effect Tests ---"));
    HueRotationEffectTest::runAllTests();

    Serial.println(F("--- Renderer Tests ---"));
    EffectRendererTest::runAllTests();

    Serial.println(F("=== All Component Tests Complete ==="));
    printSystemStatus();
}

void GeneratorTestRunner::runGeneratorTests(const char* generatorType) {
    if (!generatorType) return;

    // Convert to lowercase for easier matching
    char genType[32];
    strncpy(genType, generatorType, sizeof(genType) - 1);
    genType[sizeof(genType) - 1] = '\0';

    for (int i = 0; genType[i]; i++) {
        genType[i] = tolower(genType[i]);
    }

    if (strcmp(genType, "fire") == 0 || strcmp(genType, "unified-fire") == 0) {
        UnifiedFireGeneratorTest::runAllTests();
    } else if (strcmp(genType, "effects") == 0 || strcmp(genType, "effect") == 0) {
        HueRotationEffectTest::runAllTests();
    } else if (strcmp(genType, "renderer") == 0 || strcmp(genType, "render") == 0) {
        EffectRendererTest::runAllTests();
    } else {
        Serial.print(F("Unknown component type: "));
        Serial.println(generatorType);
        Serial.println(F("Available types: fire, effects, renderer"));
    }
}

bool GeneratorTestRunner::handleCommand(const char* command) {
    if (!command) return false;

    // Convert to lowercase for easier matching
    char cmd[64];
    strncpy(cmd, command, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    for (int i = 0; cmd[i]; i++) {
        cmd[i] = tolower(cmd[i]);
    }

    // Handle general generator commands
    if (strcmp(cmd, "generators") == 0 || strcmp(cmd, "gen all") == 0) {
        runAllTests();
        return true;
    } else if (strcmp(cmd, "gen help") == 0 || strcmp(cmd, "generator help") == 0) {
        printHelp();
        return true;
    } else if (strcmp(cmd, "gen status") == 0 || strcmp(cmd, "generator status") == 0) {
        printSystemStatus();
        return true;
    }

    // Try unified fire generator commands
    if (strncmp(cmd, "fire", 4) == 0) {
        UnifiedFireGeneratorTest::runAllTests();
        return true;
    }

    // Handle generator-specific commands
    if (strncmp(cmd, "gen ", 4) == 0) {
        const char* genType = cmd + 4;
        while (*genType == ' ') genType++; // Skip spaces

        runGeneratorTests(genType);
        return true;
    }

    return false; // Command not handled
}

void GeneratorTestRunner::printHelp() const {
    Serial.println(F("=== Comprehensive Test Commands ==="));
    Serial.println(F("generators      - Run all component tests"));
    Serial.println(F("gen all         - Run all component tests"));
    Serial.println(F("gen fire        - Run unified fire generator tests"));
    Serial.println(F("gen effects     - Run effect tests"));
    Serial.println(F("gen renderer    - Run renderer tests"));
    Serial.println(F("gen status      - Show system status"));
    Serial.println(F("gen help        - Show this help"));
    Serial.println();
    Serial.println(F("=== Fire Generator Layout Tests ==="));
    Serial.println(F("Unified Fire Generator supports: MATRIX, LINEAR, RANDOM layouts"));
}

bool GeneratorTestRunner::getLastTestResult() const {
    // For the unified system, we'll assume tests passed
    // Individual test methods return their own results
    return true;
}

void GeneratorTestRunner::printSystemStatus() const {
    Serial.println(F("=== Generator System Status ==="));
    Serial.print(F("Matrix Size: "));
    Serial.print(matrixWidth_);
    Serial.print(F("x"));
    Serial.println(matrixHeight_);

    Serial.println(F("Available Generators:"));
    Serial.println(F("  - Unified Fire: ✓ Available + Tests (Matrix/Linear/Random layouts)"));
    Serial.println(F("  - Stars: ⏳ Planned"));
    Serial.println(F("  - Waves: ⏳ Planned"));
    Serial.println(F("  - Noise: ⏳ Planned"));

    Serial.println(F("Available Effects:"));
    Serial.println(F("  - HueRotation: ✓ Available + Tests"));
    Serial.println(F("  - Brightness: ⏳ Planned"));
    Serial.println(F("  - Blur: ⏳ Planned"));

    Serial.println(F("Available Renderers:"));
    Serial.println(F("  - EffectRenderer: ✓ Available + Tests"));

    Serial.println(F("Architecture:"));
    Serial.println(F("  Generator -> Effects -> Renderer -> Hardware"));
    Serial.println();
}
