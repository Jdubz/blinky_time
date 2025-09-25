#pragma once
#include "../FireGenerator.h"
#include "../../../EffectMatrix.h"

/**
 * FireGeneratorTest - Comprehensive test suite for FireGenerator
 * 
 * Tests fire pattern generation, heat simulation, and matrix output to ensure
 * the fire generator produces the expected red/orange/yellow fire colors.
 * 
 * This replaces the old FireEffectTest with the new Generator architecture.
 */
class FireGeneratorTest {
private:
    FireGenerator* fireGenerator_;
    EffectMatrix* testMatrix_;
    int testWidth_;
    int testHeight_;
    int testsRun_;
    int testsPassed_;
    int testsFailed_;
    
    void logTest(const char* testName, bool passed, const char* details = nullptr);
    bool isFireColor(const RGB& color) const;
    bool isValidFireProgression(const RGB& bottom, const RGB& top) const;
    
public:
    FireGeneratorTest(int width = 4, int height = 15);
    ~FireGeneratorTest();
    
    // Test execution
    void runAllTests();
    
    // Individual test methods
    bool testInitialization();
    bool testHeatManagement();
    bool testColorGeneration();
    bool testMatrixOutput();
    bool testFireProgression();
    bool testAudioResponse();
    bool testParameterEffects();
    bool testBoundaryConditions();
    bool testPerformance();
    
    // Results
    void printResults() const;
    bool allTestsPassed() const { return testsFailed_ == 0; }
    int getTestsRun() const { return testsRun_; }
    int getTestsPassed() const { return testsPassed_; }
    int getTestsFailed() const { return testsFailed_; }
};