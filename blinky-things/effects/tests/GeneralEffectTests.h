#pragma once
#include "../core/Effect.h"
#include "../core/EffectMatrix.h"

/**
 * GeneralEffectTests - Universal test suite for all effects
 *
 * Tests basic contract and behavior that all effects should satisfy:
 * - Basic interface compliance
 * - Matrix safety (no crashes, valid data)
 * - State management (reset functionality)
 * - Performance constraints
 */
class GeneralEffectTests {
private:
    Effect* testEffect_;
    EffectMatrix* testMatrix_;
    int testWidth_;
    int testHeight_;
    int testsRun_;
    int testsPassed_;
    int testsFailed_;

    void logTest(const char* testName, bool passed, const char* details = nullptr);
    bool isValidPixelData(uint8_t r, uint8_t g, uint8_t b) const;
    bool matrixHasValidData(EffectMatrix* matrix) const;

public:
    GeneralEffectTests(Effect* effect, int width = 4, int height = 4);
    ~GeneralEffectTests();

    // Test execution
    void runAllTests();
    void runBasicTests();
    void runSafetyTests();
    void runPerformanceTests();

    // Individual test methods
    bool testBasicInterface();
    bool testMatrixSafety();
    bool testNullMatrixHandling();
    bool testResetFunctionality();
    bool testMultipleApplications();
    bool testPerformanceConstraints();
    bool testDataIntegrity();

    // Results
    void printResults() const;
    bool allTestsPassed() const;
    int getTestsRun() const { return testsRun_; }
    int getTestsPassed() const { return testsPassed_; }
    int getTestsFailed() const { return testsFailed_; }
};