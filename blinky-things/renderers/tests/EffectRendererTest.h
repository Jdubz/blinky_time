#ifndef EFFECT_RENDERER_TEST_H
#define EFFECT_RENDERER_TEST_H

#include "../EffectRenderer.h"
#include "../../../core/EffectMatrix.h"

/**
 * Test suite for EffectRenderer
 * Tests hardware mapping and LED output functionality
 */
class EffectRendererTest {
public:
    EffectRendererTest();

    // Main test runner
    bool runAllTests();

private:
    // Individual test methods
    bool testInitialization();
    bool testMatrixRendering();
    bool testColorOutput();
    bool testBrightnessControl();
    bool testDifferentSizes();
    bool testEdgeCases();

    // Helper methods
    bool compareColors(uint32_t color1, uint32_t color2, uint8_t tolerance = 5);
    uint32_t createColor(uint8_t r, uint8_t g, uint8_t b);
    void extractRGB(uint32_t color, uint8_t& r, uint8_t& g, uint8_t& b);
    void logTestInfo(const char* info);
    void printResults();
    void printTestResult(const char* testName, bool passed);

    // Test tracking
    int testsRun;
    int testsPassed;
};

#endif // EFFECT_RENDERER_TEST_H
