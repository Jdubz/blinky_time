#pragma once

#include "MatrixFireGenerator.h"
#include "../../../core/EffectMatrix.h"

/**
 * MatrixFireGeneratorTest - Test suite for MatrixFireGenerator
 *
 * Tests fire pattern generation, heat simulation, and color mapping
 * for matrix-style LED arrangements.
 */
class MatrixFireGeneratorTest {
public:
    MatrixFireGeneratorTest();

    // Test runner
    bool runAllTests();

    // Individual test methods
    bool testInitialization();
    bool testHeatSimulation();
    bool testSparkGeneration();
    bool testColorMapping();
    bool testEnergyResponse();
    bool testMatrixOutput();

    // Utility methods
    void printResults();
    void printTestResult(const char* testName, bool passed);

private:
    int testsRun;
    int testsPassed;

    // Test helpers
    bool compareFloats(float a, float b, float tolerance = 0.001f);
    bool verifyColorRange(uint32_t color);
    void logTestInfo(const char* info);
};
