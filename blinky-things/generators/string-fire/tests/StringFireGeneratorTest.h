#ifndef STRING_FIRE_GENERATOR_TEST_H
#define STRING_FIRE_GENERATOR_TEST_H

#include "../StringFireGenerator.h"
#include "../../../core/EffectMatrix.h"

/**
 * Test suite for StringFireGenerator
 * Tests lateral heat propagation and string-based fire effects
 */
class StringFireGeneratorTest {
public:
    StringFireGeneratorTest();
    
    // Main test runner
    bool runAllTests();
    
private:
    // Individual test methods
    bool testInitialization();
    bool testLateralHeatPropagation();
    bool testSparkGeneration();
    bool testColorMapping();
    bool testEnergyResponse();
    bool testMatrixOutput();
    bool testStringBehavior();
    
    // Helper methods
    bool compareFloats(float a, float b, float tolerance = 0.01f);
    bool verifyColorRange(uint32_t color);
    void logTestInfo(const char* info);
    void printResults();
    void printTestResult(const char* testName, bool passed);
    
    // Test tracking
    int testsRun;
    int testsPassed;
};

#endif // STRING_FIRE_GENERATOR_TEST_H