#include "FireEffectTest.h"
#include <Arduino.h>

FireEffectTest::FireEffectTest(int width, int height) 
    : testWidth_(width), testHeight_(height), testsRun_(0), testsPassed_(0), testsFailed_(0) {
    fireEffect_ = new FireVisualEffect();
    testMatrix_ = new EffectMatrix(width, height);
    fireEffect_->begin(width, height);
}

FireEffectTest::~FireEffectTest() {
    delete fireEffect_;
    delete testMatrix_;
}

void FireEffectTest::runAllTests() {
    Serial.println(F("=== FireEffect Test Suite ==="));
    resetStats();
    
    testColorPalette();
    testHeatToColor();
    testMatrixGeneration();
    testAudioResponsiveness();
    testHeatDiffusion();
    testSparkGeneration();
    testBoundaryConditions();
    
    printResults();
}

bool FireEffectTest::testColorPalette() {
    Serial.println(F("\n--- Testing Color Palette ---"));
    
    // Test key heat values for expected fire colors
    struct TestColor {
        float heat;
        const char* expectedDescription;
        uint8_t minRed;
        uint8_t maxGreen;
    };
    
    TestColor testColors[] = {
        {0.0f, "Black", 0, 50},           // Should be black/very dark
        {0.2f, "Dark Red", 100, 80},      // Should be red-dominant
        {0.5f, "Bright Red", 200, 100},   // Should be bright red
        {0.8f, "Orange/Yellow", 200, 200}, // Should have significant red+green
        {1.0f, "Hot White", 200, 200}     // Should be very bright
    };
    
    bool allPassed = true;
    
    for (int i = 0; i < 5; i++) {
        fireEffect_->clearHeat();
        
        // Set a test heat value and render
        fireEffect_->setHeat(1, 1, testColors[i].heat * 255.0f);
        fireEffect_->render(*testMatrix_);
        
        RGB color = testMatrix_->getPixel(1, testHeight_ - 2); // Account for vertical flip
        
        bool colorValid = (color.r >= testColors[i].minRed) && 
                         (testColors[i].heat > 0.1f ? color.g <= testColors[i].maxGreen : true);
        
        char details[100];
        sprintf(details, "%s: Heat=%.1f -> RGB(%d,%d,%d)", 
                testColors[i].expectedDescription, testColors[i].heat, color.r, color.g, color.b);
        
        logTest("Color Palette", colorValid, details);
        if (!colorValid) allPassed = false;
    }
    
    return allPassed;
}

bool FireEffectTest::testHeatToColor() {
    Serial.println(F("\n--- Testing Heat-to-Color Conversion ---"));
    
    bool passed = true;
    
    // Test that colors progress logically from dark to bright
    RGB black, darkRed, brightRed, orange, white;
    
    fireEffect_->clearHeat();
    fireEffect_->setHeat(0, 0, 0);
    fireEffect_->render(*testMatrix_);
    black = testMatrix_->getPixel(0, testHeight_ - 1);
    
    fireEffect_->setHeat(0, 0, 51);  // 20% heat
    fireEffect_->render(*testMatrix_);
    darkRed = testMatrix_->getPixel(0, testHeight_ - 1);
    
    fireEffect_->setHeat(0, 0, 128); // 50% heat
    fireEffect_->render(*testMatrix_);
    brightRed = testMatrix_->getPixel(0, testHeight_ - 1);
    
    fireEffect_->setHeat(0, 0, 204); // 80% heat
    fireEffect_->render(*testMatrix_);
    orange = testMatrix_->getPixel(0, testHeight_ - 1);
    
    fireEffect_->setHeat(0, 0, 255); // 100% heat
    fireEffect_->render(*testMatrix_);
    white = testMatrix_->getPixel(0, testHeight_ - 1);
    
    // Test progression: each level should be brighter than the last
    bool progressionValid = (black.r < darkRed.r) && 
                           (darkRed.r < brightRed.r) &&
                           (brightRed.r <= orange.r) &&
                           (orange.r <= white.r);
    
    logTest("Heat Progression", progressionValid, 
            progressionValid ? "Colors progress correctly" : "Color progression failed");
    
    // Test that fire colors are red-dominant (except for hottest)
    bool redDominant = (darkRed.r > darkRed.g + 50) && 
                      (brightRed.r > brightRed.g + 50);
    
    logTest("Red Dominance", redDominant, 
            redDominant ? "Fire shows red-dominant colors" : "Fire lacks red dominance");
    
    return progressionValid && redDominant;
}

bool FireEffectTest::testMatrixGeneration() {
    Serial.println(F("\n--- Testing Matrix Generation ---"));
    
    fireEffect_->clearHeat();
    
    // Set some heat at the bottom
    for (int x = 0; x < testWidth_; x++) {
        fireEffect_->setHeat(x, 0, 200);  // Hot bottom row
    }
    
    // Render and check that bottom row (top of display) has color
    fireEffect_->render(*testMatrix_);
    
    bool bottomHasColor = false;
    bool topIsCooler = true;
    
    // Check bottom row of display (top of heat matrix due to flip)
    for (int x = 0; x < testWidth_; x++) {
        RGB bottomColor = testMatrix_->getPixel(x, testHeight_ - 1);
        if (bottomColor.r > 50 || bottomColor.g > 50 || bottomColor.b > 50) {
            bottomHasColor = true;
        }
        
        // Compare with top row
        RGB topColor = testMatrix_->getPixel(x, 0);
        if (topColor.r > bottomColor.r) {
            topIsCooler = false;
        }
    }
    
    logTest("Matrix Bottom Heat", bottomHasColor, 
            bottomHasColor ? "Bottom row shows heat" : "Bottom row lacks heat");
    
    logTest("Matrix Heat Gradient", topIsCooler, 
            topIsCooler ? "Top cooler than bottom" : "Heat gradient incorrect");
    
    return bottomHasColor && topIsCooler;
}

bool FireEffectTest::testAudioResponsiveness() {
    Serial.println(F("\n--- Testing Audio Responsiveness ---"));
    
    fireEffect_->clearHeat();
    fireEffect_->restoreDefaults();
    
    // Test with low energy
    fireEffect_->update(0.1f, 0.0f);
    fireEffect_->render(*testMatrix_);
    
    int lowEnergyPixels = 0;
    for (int y = 0; y < testHeight_; y++) {
        for (int x = 0; x < testWidth_; x++) {
            RGB color = testMatrix_->getPixel(x, y);
            if (color.r > 10 || color.g > 10 || color.b > 10) {
                lowEnergyPixels++;
            }
        }
    }
    
    // Test with high energy
    fireEffect_->clearHeat();
    fireEffect_->update(0.8f, 0.5f);
    fireEffect_->render(*testMatrix_);
    
    int highEnergyPixels = 0;
    for (int y = 0; y < testHeight_; y++) {
        for (int x = 0; x < testWidth_; x++) {
            RGB color = testMatrix_->getPixel(x, y);
            if (color.r > 10 || color.g > 10 || color.b > 10) {
                highEnergyPixels++;
            }
        }
    }
    
    bool responsive = highEnergyPixels > lowEnergyPixels;
    
    char details[100];
    sprintf(details, "Low energy: %d pixels, High energy: %d pixels", 
            lowEnergyPixels, highEnergyPixels);
    
    logTest("Audio Responsiveness", responsive, details);
    
    return responsive;
}

bool FireEffectTest::testHeatDiffusion() {
    Serial.println(F("\n--- Testing Heat Diffusion ---"));
    
    fireEffect_->clearHeat();
    
    // Set heat at bottom center
    int centerX = testWidth_ / 2;
    fireEffect_->setHeat(centerX, 0, 255);
    
    // Run several updates to allow diffusion
    for (int i = 0; i < 10; i++) {
        fireEffect_->update(0.0f, 0.0f);  // No new sparks, just diffusion
    }
    
    // Check if heat has moved upward
    float bottomHeat = fireEffect_->getHeat(centerX, 0);
    float topHeat = fireEffect_->getHeat(centerX, testHeight_ - 1);
    
    // Bottom should still have some heat, top should have gained some
    bool diffusionWorking = (bottomHeat > 50) && (topHeat > 5);
    
    char details[100];
    sprintf(details, "Bottom heat: %.1f, Top heat: %.1f", bottomHeat, topHeat);
    
    logTest("Heat Diffusion", diffusionWorking, details);
    
    return diffusionWorking;
}

bool FireEffectTest::testSparkGeneration() {
    Serial.println(F("\n--- Testing Spark Generation ---"));
    
    fireEffect_->clearHeat();
    fireEffect_->restoreDefaults();
    
    // Force spark generation with high energy
    for (int i = 0; i < 20; i++) {
        fireEffect_->update(1.0f, 1.0f);  // Maximum energy and hit
    }
    
    // Check if any sparks were generated in bottom rows
    bool sparksGenerated = false;
    for (int y = 0; y < fireEffect_->params.bottomRowsForSparks; y++) {
        for (int x = 0; x < testWidth_; x++) {
            if (fireEffect_->getHeat(x, y) > 10) {
                sparksGenerated = true;
                break;
            }
        }
        if (sparksGenerated) break;
    }
    
    logTest("Spark Generation", sparksGenerated, 
            sparksGenerated ? "Sparks generated in bottom rows" : "No sparks generated");
    
    return sparksGenerated;
}

bool FireEffectTest::testBoundaryConditions() {
    Serial.println(F("\n--- Testing Boundary Conditions ---"));
    
    bool passed = true;
    
    // Test setting heat outside bounds (should not crash)
    fireEffect_->setHeat(-1, -1, 255);
    fireEffect_->setHeat(testWidth_, testHeight_, 255);
    
    // Test getting heat outside bounds (should return 0)
    float outOfBounds1 = fireEffect_->getHeat(-1, -1);
    float outOfBounds2 = fireEffect_->getHeat(testWidth_, testHeight_);
    
    bool boundsHandled = (outOfBounds1 == 0.0f) && (outOfBounds2 == 0.0f);
    
    logTest("Boundary Handling", boundsHandled, 
            boundsHandled ? "Out-of-bounds access handled safely" : "Boundary issues detected");
    
    // Test matrix access with valid coordinates
    fireEffect_->clearHeat();
    fireEffect_->setHeat(0, 0, 100);
    fireEffect_->render(*testMatrix_);
    
    RGB corner = testMatrix_->getPixel(0, testHeight_ - 1);
    bool validAccess = (corner.r > 0 || corner.g > 0 || corner.b > 0);
    
    logTest("Valid Access", validAccess, 
            validAccess ? "Matrix access works correctly" : "Matrix access failed");
    
    return boundsHandled && validAccess;
}

void FireEffectTest::logTest(const char* testName, bool passed, const char* details) {
    testsRun_++;
    if (passed) {
        testsPassed_++;
        Serial.print(F("✓ PASS: "));
    } else {
        testsFailed_++;
        Serial.print(F("✗ FAIL: "));
    }
    
    Serial.print(testName);
    if (details) {
        Serial.print(F(" - "));
        Serial.print(details);
    }
    Serial.println();
}

void FireEffectTest::printResults() {
    Serial.println(F("\n=== Test Results ==="));
    Serial.print(F("Tests Run: ")); Serial.println(testsRun_);
    Serial.print(F("Passed: ")); Serial.println(testsPassed_);
    Serial.print(F("Failed: ")); Serial.println(testsFailed_);
    Serial.print(F("Success Rate: ")); 
    Serial.print((testsPassed_ * 100) / testsRun_); 
    Serial.println(F("%"));
    
    if (testsFailed_ == 0) {
        Serial.println(F("🔥 ALL TESTS PASSED! Fire effect is working correctly."));
    } else {
        Serial.println(F("⚠️  Some tests failed. Check implementation."));
    }
}

void FireEffectTest::resetStats() {
    testsRun_ = 0;
    testsPassed_ = 0;
    testsFailed_ = 0;
}

void FireEffectTest::printMatrixColors(const EffectMatrix& matrix) {
    Serial.println(F("Matrix Color Dump:"));
    for (int y = 0; y < matrix.getHeight(); y++) {
        Serial.print(F("Row ")); Serial.print(y); Serial.print(F(": "));
        for (int x = 0; x < matrix.getWidth(); x++) {
            RGB color = matrix.getPixel(x, y);
            Serial.print(F("(")); 
            Serial.print(color.r); Serial.print(F(","));
            Serial.print(color.g); Serial.print(F(","));
            Serial.print(color.b); Serial.print(F(") "));
        }
        Serial.println();
    }
}