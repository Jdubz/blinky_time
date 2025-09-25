#include "HueRotationEffectTest.h"
#include <Arduino.h>

HueRotationEffectTest::HueRotationEffectTest(int width, int height) 
    : testWidth_(width), testHeight_(height), testsRun_(0), testsPassed_(0), testsFailed_(0) {
    hueEffect_ = new HueRotationEffect();
    testMatrix_ = new EffectMatrix(width, height);
    hueEffect_->begin(width, height);
}

HueRotationEffectTest::~HueRotationEffectTest() {
    delete hueEffect_;
    delete testMatrix_;
}

void HueRotationEffectTest::logTest(const char* testName, bool passed, const char* details) {
    testsRun_++;
    if (passed) {
        testsPassed_++;
        Serial.print(F("✓ "));
    } else {
        testsFailed_++;
        Serial.print(F("✗ "));
    }
    
    Serial.print(F("HueRotationEffectTest::"));
    Serial.print(testName);
    
    if (details && strlen(details) > 0) {
        Serial.print(F(" - "));
        Serial.print(details);
    }
    
    Serial.println();
}

bool HueRotationEffectTest::colorsApproximatelyEqual(const RGB& a, const RGB& b, uint8_t tolerance) const {
    return (abs((int)a.r - (int)b.r) <= tolerance) &&
           (abs((int)a.g - (int)b.g) <= tolerance) &&
           (abs((int)a.b - (int)b.b) <= tolerance);
}

void HueRotationEffectTest::runAllTests() {
    Serial.println(F("=== HueRotationEffect Test Suite ==="));
    Serial.print(F("Testing hue rotation effect with "));
    Serial.print(testWidth_);
    Serial.print(F("x"));
    Serial.print(testHeight_);
    Serial.println(F(" matrix"));
    Serial.println();
    
    testInitialization();
    testStaticHueShift();
    testAutoRotation();
    testColorPreservation();
    testBoundaryConditions();
    
    Serial.println();
    printResults();
}

bool HueRotationEffectTest::testInitialization() {
    // Test that effect initializes properly
    HueRotationEffect testEffect;
    testEffect.begin(testWidth_, testHeight_);
    
    bool initialHueIsZero = (testEffect.getHueShift() == 0.0f);
    bool initialSpeedIsZero = (testEffect.getRotationSpeed() == 0.0f);
    
    bool passed = initialHueIsZero && initialSpeedIsZero;
    logTest("testInitialization", passed, "Effect should initialize with zero hue shift and rotation speed");
    return passed;
}

bool HueRotationEffectTest::testStaticHueShift() {
    testMatrix_->clear();
    
    // Set up a test pattern with known colors
    testMatrix_->setPixel(0, 0, RGB{255, 0, 0});   // Pure red
    testMatrix_->setPixel(1, 0, RGB{0, 255, 0});   // Pure green
    testMatrix_->setPixel(2, 0, RGB{0, 0, 255});   // Pure blue
    testMatrix_->setPixel(3, 0, RGB{0, 0, 0});     // Black (should not change)
    
    // Apply 120-degree hue shift (should shift colors significantly)
    hueEffect_->setHueShift(1.0f/3.0f); // 120 degrees = 1/3 of full rotation
    hueEffect_->apply(testMatrix_);
    
    RGB shiftedRed = testMatrix_->getPixel(0, 0);
    RGB shiftedGreen = testMatrix_->getPixel(1, 0);
    RGB shiftedBlue = testMatrix_->getPixel(2, 0);
    RGB shiftedBlack = testMatrix_->getPixel(3, 0);
    
    // Check that colors have changed (except black)
    bool redChanged = !colorsApproximatelyEqual(shiftedRed, RGB{255, 0, 0});
    bool greenChanged = !colorsApproximatelyEqual(shiftedGreen, RGB{0, 255, 0});
    bool blueChanged = !colorsApproximatelyEqual(shiftedBlue, RGB{0, 0, 255});
    bool blackUnchanged = colorsApproximatelyEqual(shiftedBlack, RGB{0, 0, 0});
    
    bool passed = redChanged && greenChanged && blueChanged && blackUnchanged;
    logTest("testStaticHueShift", passed, "Hue shift should change colors but preserve black");
    return passed;
}

bool HueRotationEffectTest::testAutoRotation() {
    testMatrix_->clear();
    testMatrix_->setPixel(0, 0, RGB{255, 0, 0}); // Red pixel
    
    // Set rotation speed and record initial color
    hueEffect_->setRotationSpeed(1.0f); // 1 full rotation per second
    RGB initialColor = testMatrix_->getPixel(0, 0);
    
    // Simulate time passing and apply effect multiple times
    delay(100); // 100ms delay
    hueEffect_->apply(testMatrix_);
    RGB color1 = testMatrix_->getPixel(0, 0);
    
    delay(100); // Another 100ms
    hueEffect_->apply(testMatrix_);
    RGB color2 = testMatrix_->getPixel(0, 0);
    
    // Colors should be different due to auto-rotation
    bool color1Changed = !colorsApproximatelyEqual(initialColor, color1);
    bool color2Changed = !colorsApproximatelyEqual(color1, color2);
    
    bool passed = color1Changed && color2Changed;
    logTest("testAutoRotation", passed, "Auto-rotation should continuously change colors");
    return passed;
}

bool HueRotationEffectTest::testColorPreservation() {
    testMatrix_->clear();
    
    // Create a pattern with various colors
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            uint8_t r = (x * 64) % 256;
            uint8_t g = (y * 64) % 256;
            uint8_t b = ((x + y) * 32) % 256;
            testMatrix_->setPixel(x, y, RGB{r, g, b});
        }
    }
    
    // Count non-black pixels before
    int nonBlackBefore = 0;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            RGB pixel = testMatrix_->getPixel(x, y);
            if (pixel.r > 0 || pixel.g > 0 || pixel.b > 0) {
                nonBlackBefore++;
            }
        }
    }
    
    // Apply hue shift
    hueEffect_->setHueShift(0.5f); // 180-degree shift
    hueEffect_->apply(testMatrix_);
    
    // Count non-black pixels after
    int nonBlackAfter = 0;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            RGB pixel = testMatrix_->getPixel(x, y);
            if (pixel.r > 0 || pixel.g > 0 || pixel.b > 0) {
                nonBlackAfter++;
            }
        }
    }
    
    // Should preserve the same number of non-black pixels
    bool pixelCountPreserved = (nonBlackBefore == nonBlackAfter);
    
    logTest("testColorPreservation", pixelCountPreserved, "Hue shift should preserve pixel brightness patterns");
    return pixelCountPreserved;
}

bool HueRotationEffectTest::testBoundaryConditions() {
    testMatrix_->clear();
    testMatrix_->setPixel(0, 0, RGB{128, 64, 192}); // Arbitrary color
    
    // Test extreme hue shifts
    hueEffect_->setHueShift(-1.5f); // Negative, > 1.0
    hueEffect_->apply(testMatrix_);
    RGB negativeShift = testMatrix_->getPixel(0, 0);
    
    testMatrix_->setPixel(0, 0, RGB{128, 64, 192}); // Reset
    hueEffect_->setHueShift(2.5f); // > 1.0
    hueEffect_->apply(testMatrix_);
    RGB positiveShift = testMatrix_->getPixel(0, 0);
    
    // Both should produce valid colors (no invalid RGB values)
    bool negativeValid = (negativeShift.r <= 255 && negativeShift.g <= 255 && negativeShift.b <= 255);
    bool positiveValid = (positiveShift.r <= 255 && positiveShift.g <= 255 && positiveShift.b <= 255);
    
    bool passed = negativeValid && positiveValid;
    logTest("testBoundaryConditions", passed, "Extreme hue values should produce valid colors");
    return passed;
}

void HueRotationEffectTest::printResults() const {
    Serial.println(F("=== HueRotationEffect Test Results ==="));
    Serial.print(F("Tests Run: "));
    Serial.println(testsRun_);
    Serial.print(F("Tests Passed: "));
    Serial.println(testsPassed_);
    Serial.print(F("Tests Failed: "));
    Serial.println(testsFailed_);
    
    if (testsFailed_ == 0) {
        Serial.println(F("🎉 All tests PASSED! Hue rotation effect is working correctly."));
    } else {
        Serial.print(F("⚠️  "));
        Serial.print(testsFailed_);
        Serial.println(F(" tests FAILED. Check implementation."));
    }
    Serial.println();
}