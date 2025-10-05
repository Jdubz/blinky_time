#pragma once
#include "../generators/legacy-fire/tests/FireTestRunner.h"
#include "../generators/matrix-fire/tests/MatrixFireGeneratorTest.h"
#include "../generators/string-fire/tests/StringFireGeneratorTest.h"
#include "../effects/hue-rotation/tests/HueRotationEffectTest.h"
#include "../renderers/tests/EffectRendererTest.h"

/**
 * GeneratorTestRunner - Main test coordinator for all generator types
 *
 * This replaces the old EffectTestRunner and coordinates testing of all
 * generator types (Fire, Stars, Waves, etc.) in the new architecture.
 *
 * Each generator type has its own specialized test runner and test suite.
 */
class GeneratorTestRunner {
private:
    FireTestRunner* fireTestRunner_;
    MatrixFireGeneratorTest* matrixFireTest_;
    StringFireGeneratorTest* stringFireTest_;
    HueRotationEffectTest* hueRotationTest_;
    EffectRendererTest* rendererTest_;
    int matrixWidth_;
    int matrixHeight_;

public:
    GeneratorTestRunner(int width = 4, int height = 15);
    ~GeneratorTestRunner();

    // Test execution
    void runAllTests();
    void runGeneratorTests(const char* generatorType);

    // Command interface for serial integration
    bool handleCommand(const char* command);
    void printHelp() const;

    // Results access
    bool getLastTestResult() const;
    void printSystemStatus() const;
};
