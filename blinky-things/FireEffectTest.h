#pragma once
#include "FireVisualEffect.h"
#include "EffectMatrix.h"

/**
 * FireEffectTest - Comprehensive test suite for FireVisualEffect
 * 
 * Tests color generation, heat simulation, and matrix output to ensure
 * the fire effect produces the expected red/orange/yellow fire colors.
 */
class FireEffectTest {
private:
    FireVisualEffect* fireEffect_;
    EffectMatrix* testMatrix_;
    int testWidth_;
    int testHeight_;
    
    // Test result tracking
    int testsRun_;
    int testsPassed_;
    int testsFailed_;
    
    // Helper functions
    void logTest(const char* testName, bool passed, const char* details = nullptr);
    bool isFireColor(const RGB& color);
    bool isValidColorProgression(const RGB& cold, const RGB& hot);
    void printColorDetails(const RGB& color);
    
public:
    FireEffectTest(int width = 4, int height = 15);
    ~FireEffectTest();
    
    // Test suite
    void runAllTests();
    
    // Individual tests
    bool testColorPalette();
    bool testHeatToColor();
    bool testMatrixGeneration();
    bool testAudioResponsiveness();
    bool testHeatDiffusion();
    bool testSparkGeneration();
    bool testBoundaryConditions();
    
    // Utility functions
    void printResults();
    void resetStats();
    
    // Manual testing helpers
    void generateTestPattern(EffectMatrix& matrix, int pattern);
    void printMatrixColors(const EffectMatrix& matrix);
};