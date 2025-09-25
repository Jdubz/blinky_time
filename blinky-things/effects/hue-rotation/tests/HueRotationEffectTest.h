#pragma once
#include "../HueRotationEffect.h"
#include "../../../EffectMatrix.h"

/**
 * HueRotationEffectTest - Test suite for HueRotationEffect
 * 
 * Tests hue rotation color transformations, timing, and matrix operations
 * to ensure the effect produces correct color shifts.
 */
class HueRotationEffectTest {
private:
    HueRotationEffect* hueEffect_;
    EffectMatrix* testMatrix_;
    int testWidth_;
    int testHeight_;
    int testsRun_;
    int testsPassed_;
    int testsFailed_;
    
    void logTest(const char* testName, bool passed, const char* details = nullptr);
    bool colorsApproximatelyEqual(const RGB& a, const RGB& b, uint8_t tolerance = 5) const;
    
public:
    HueRotationEffectTest(int width = 4, int height = 4);
    ~HueRotationEffectTest();
    
    // Test execution
    void runAllTests();
    
    // Individual test methods
    bool testInitialization();
    bool testStaticHueShift();
    bool testAutoRotation();
    bool testColorPreservation();
    bool testBoundaryConditions();
    
    // Results
    void printResults() const;
    bool allTestsPassed() const { return testsFailed_ == 0; }
    int getTestsRun() const { return testsRun_; }
    int getTestsPassed() const { return testsPassed_; }
    int getTestsFailed() const { return testsFailed_; }
};