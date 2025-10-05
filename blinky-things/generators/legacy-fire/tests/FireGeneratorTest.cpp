#include "FireGeneratorTest.h"
#include <Arduino.h>

FireGeneratorTest::FireGeneratorTest(int width, int height)
    : testWidth_(width), testHeight_(height), testsRun_(0), testsPassed_(0), testsFailed_(0) {
    fireGenerator_ = new FireGenerator();
    testMatrix_ = new EffectMatrix(width, height);
    fireGenerator_->begin(width, height);
}

FireGeneratorTest::~FireGeneratorTest() {
    delete fireGenerator_;
    delete testMatrix_;
}

void FireGeneratorTest::logTest(const char* testName, bool passed, const char* details) {
    testsRun_++;
    if (passed) {
        testsPassed_++;
        Serial.print(F("✓ "));
    } else {
        testsFailed_++;
        Serial.print(F("✗ "));
    }

    Serial.print(F("FireGeneratorTest::"));
    Serial.print(testName);

    if (details && strlen(details) > 0) {
        Serial.print(F(" - "));
        Serial.print(details);
    }

    Serial.println();
}

bool FireGeneratorTest::isFireColor(const RGB& color) const {
    // Fire colors should be primarily red/orange/yellow (high red, some green, low blue)
    return (color.r >= 128 && color.b <= color.r && color.g <= color.r);
}

bool FireGeneratorTest::isValidFireProgression(const RGB& bottom, const RGB& top) const {
    // Bottom should be hotter (more intense) than top in most cases
    uint32_t bottomIntensity = (uint32_t)bottom.r + bottom.g + bottom.b;
    uint32_t topIntensity = (uint32_t)top.r + top.g + top.b;

    // Allow some variance, but generally bottom should be >= top
    return bottomIntensity >= (topIntensity * 0.8f);
}

void FireGeneratorTest::runAllTests() {
    Serial.println(F("=== FireGenerator Test Suite ==="));
    Serial.print(F("Testing fire generator with "));
    Serial.print(testWidth_);
    Serial.print(F("x"));
    Serial.print(testHeight_);
    Serial.println(F(" matrix"));
    Serial.println();

    testInitialization();
    testHeatManagement();
    testColorGeneration();
    testMatrixOutput();
    testFireProgression();
    testAudioResponse();
    testParameterEffects();
    testBoundaryConditions();
    testPerformance();

    Serial.println();
    printResults();
}

bool FireGeneratorTest::testInitialization() {
    fireGenerator_->clearHeat();

    // Test that generator initializes with zero heat
    bool allZero = true;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            if (fireGenerator_->getHeat(x, y) != 0.0f) {
                allZero = false;
                break;
            }
        }
        if (!allZero) break;
    }

    logTest("testInitialization", allZero, "Heat buffer should initialize to zero");
    return allZero;
}

bool FireGeneratorTest::testHeatManagement() {
    fireGenerator_->clearHeat();

    // Set some heat values
    fireGenerator_->setHeat(1, testHeight_ - 1, 0.8f);
    fireGenerator_->setHeat(2, testHeight_ - 2, 0.6f);

    // Verify heat values
    bool heatSet = (abs(fireGenerator_->getHeat(1, testHeight_ - 1) - 0.8f) < 0.01f) &&
                   (abs(fireGenerator_->getHeat(2, testHeight_ - 2) - 0.6f) < 0.01f);

    // Clear and verify
    fireGenerator_->clearHeat();
    bool heatCleared = (fireGenerator_->getHeat(1, testHeight_ - 1) == 0.0f) &&
                       (fireGenerator_->getHeat(2, testHeight_ - 2) == 0.0f);

    bool passed = heatSet && heatCleared;
    logTest("testHeatManagement", passed, "Heat set/get/clear operations");
    return passed;
}

bool FireGeneratorTest::testColorGeneration() {
    fireGenerator_->clearHeat();

    // Set different heat levels and check color generation
    fireGenerator_->setHeat(0, testHeight_ - 1, 0.9f);  // High heat - should be bright fire color
    fireGenerator_->setHeat(1, testHeight_ - 1, 0.5f);  // Medium heat - should be red/orange
    fireGenerator_->setHeat(2, testHeight_ - 1, 0.1f);  // Low heat - should be dark red
    fireGenerator_->setHeat(3, testHeight_ - 1, 0.0f);  // No heat - should be black

    fireGenerator_->generate(testMatrix_);

    RGB highHeatColor = testMatrix_->getPixel(0, testHeight_ - 1);
    RGB mediumHeatColor = testMatrix_->getPixel(1, testHeight_ - 1);
    RGB lowHeatColor = testMatrix_->getPixel(2, testHeight_ - 1);
    RGB noHeatColor = testMatrix_->getPixel(3, testHeight_ - 1);

    bool highIsFireColor = isFireColor(highHeatColor) && (highHeatColor.r > 200);
    bool mediumIsFireColor = isFireColor(mediumHeatColor) && (mediumHeatColor.r > 100);
    bool lowIsRed = (lowHeatColor.r > 0) && (lowHeatColor.g == 0) && (lowHeatColor.b == 0);
    bool noHeatIsBlack = (noHeatColor.r == 0) && (noHeatColor.g == 0) && (noHeatColor.b == 0);

    bool passed = highIsFireColor && mediumIsFireColor && lowIsRed && noHeatIsBlack;
    logTest("testColorGeneration", passed, "Heat to color conversion accuracy");
    return passed;
}

bool FireGeneratorTest::testMatrixOutput() {
    fireGenerator_->clearHeat();
    testMatrix_->clear();

    // Add some heat pattern
    for (int x = 0; x < testWidth_; x++) {
        fireGenerator_->setHeat(x, testHeight_ - 1, 0.7f);
        fireGenerator_->setHeat(x, testHeight_ - 2, 0.4f);
    }

    fireGenerator_->generate(testMatrix_);

    // Verify matrix was filled with fire colors
    bool allPixelsValid = true;
    int firePixels = 0;

    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            RGB pixel = testMatrix_->getPixel(x, y);

            // Check if pixel is valid (either black or fire color)
            bool isBlack = (pixel.r == 0 && pixel.g == 0 && pixel.b == 0);
            bool isFire = isFireColor(pixel);

            if (!isBlack && !isFire) {
                allPixelsValid = false;
                break;
            }

            if (isFire) firePixels++;
        }
        if (!allPixelsValid) break;
    }

    bool hasFirePixels = (firePixels >= testWidth_ * 2); // Should have fire in bottom 2 rows
    bool passed = allPixelsValid && hasFirePixels;

    logTest("testMatrixOutput", passed, "Matrix output contains valid fire colors");
    return passed;
}

bool FireGeneratorTest::testFireProgression() {
    fireGenerator_->clearHeat();

    // Create initial heat at bottom
    for (int x = 0; x < testWidth_; x++) {
        fireGenerator_->setHeat(x, testHeight_ - 1, 0.8f);
    }

    // Run several update cycles to let heat propagate
    for (int i = 0; i < 10; i++) {
        fireGenerator_->update();
        delay(20); // Small delay to simulate time passing
    }

    fireGenerator_->generate(testMatrix_);

    // Check that heat has moved upward
    bool heatPropagated = false;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_ - 3; y++) {
            RGB pixel = testMatrix_->getPixel(x, y);
            if (isFireColor(pixel)) {
                heatPropagated = true;
                break;
            }
        }
        if (heatPropagated) break;
    }

    logTest("testFireProgression", heatPropagated, "Heat propagates upward over time");
    return heatPropagated;
}

bool FireGeneratorTest::testAudioResponse() {
    fireGenerator_->clearHeat();
    fireGenerator_->restoreDefaults();

    // Test with no audio
    fireGenerator_->setAudioInput(0.0f, 0.0f);
    for (int i = 0; i < 5; i++) {
        fireGenerator_->update();
    }
    fireGenerator_->generate(testMatrix_);

    int lowEnergyFirePixels = 0;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            if (isFireColor(testMatrix_->getPixel(x, y))) {
                lowEnergyFirePixels++;
            }
        }
    }

    // Clear and test with high audio
    fireGenerator_->clearHeat();
    fireGenerator_->setAudioInput(1.0f, 1.0f); // Max energy and hit
    for (int i = 0; i < 5; i++) {
        fireGenerator_->update();
    }
    fireGenerator_->generate(testMatrix_);

    int highEnergyFirePixels = 0;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            if (isFireColor(testMatrix_->getPixel(x, y))) {
                highEnergyFirePixels++;
            }
        }
    }

    bool audioIncreasesFire = highEnergyFirePixels > lowEnergyFirePixels;
    logTest("testAudioResponse", audioIncreasesFire, "Audio input increases fire intensity");
    return audioIncreasesFire;
}

bool FireGeneratorTest::testParameterEffects() {
    fireGenerator_->clearHeat();

    // Test spark chance parameter
    float originalSparkChance = fireGenerator_->params.sparkChance;
    fireGenerator_->params.sparkChance = 1.0f; // 100% chance

    fireGenerator_->update();
    fireGenerator_->generate(testMatrix_);

    // Count fire pixels in bottom rows
    int sparkPixels = 0;
    int bottomRows = min(fireGenerator_->params.bottomRowsForSparks, testHeight_);
    for (int x = 0; x < testWidth_; x++) {
        for (int y = testHeight_ - bottomRows; y < testHeight_; y++) {
            if (isFireColor(testMatrix_->getPixel(x, y))) {
                sparkPixels++;
            }
        }
    }

    // Restore original parameter
    fireGenerator_->params.sparkChance = originalSparkChance;

    bool sparksGenerated = sparkPixels > 0;
    logTest("testParameterEffects", sparksGenerated, "Parameter changes affect generation");
    return sparksGenerated;
}

bool FireGeneratorTest::testBoundaryConditions() {
    fireGenerator_->clearHeat();

    // Test setting heat at boundaries
    fireGenerator_->setHeat(-1, 0, 0.5f);           // Negative X
    fireGenerator_->setHeat(testWidth_, 0, 0.5f);   // X beyond width
    fireGenerator_->setHeat(0, -1, 0.5f);           // Negative Y
    fireGenerator_->setHeat(0, testHeight_, 0.5f);  // Y beyond height

    // Test getting heat at boundaries
    float heat1 = fireGenerator_->getHeat(-1, 0);
    float heat2 = fireGenerator_->getHeat(testWidth_, 0);
    float heat3 = fireGenerator_->getHeat(0, -1);
    float heat4 = fireGenerator_->getHeat(0, testHeight_);

    // All boundary accesses should return 0 or handle gracefully
    bool boundariesHandled = (heat1 == 0.0f) && (heat2 == 0.0f) &&
                            (heat3 == 0.0f) && (heat4 == 0.0f);

    logTest("testBoundaryConditions", boundariesHandled, "Boundary conditions handled safely");
    return boundariesHandled;
}

bool FireGeneratorTest::testPerformance() {
    fireGenerator_->clearHeat();

    // Set up a complex fire pattern
    for (int x = 0; x < testWidth_; x++) {
        for (int y = testHeight_ - 3; y < testHeight_; y++) {
            fireGenerator_->setHeat(x, y, 0.8f);
        }
    }

    // Time multiple update cycles
    unsigned long startTime = millis();
    for (int i = 0; i < 50; i++) {
        fireGenerator_->update();
        fireGenerator_->generate(testMatrix_);
    }
    unsigned long elapsed = millis() - startTime;

    // Should complete 50 cycles in reasonable time (< 1 second for small matrix)
    bool performanceOk = elapsed < 1000;

    char perfDetails[64];
    snprintf(perfDetails, sizeof(perfDetails), "50 cycles took %lu ms", elapsed);
    logTest("testPerformance", performanceOk, perfDetails);
    return performanceOk;
}

void FireGeneratorTest::printResults() const {
    Serial.println(F("=== FireGenerator Test Results ==="));
    Serial.print(F("Tests Run: "));
    Serial.println(testsRun_);
    Serial.print(F("Tests Passed: "));
    Serial.println(testsPassed_);
    Serial.print(F("Tests Failed: "));
    Serial.println(testsFailed_);

    if (testsFailed_ == 0) {
        Serial.println(F("🎉 All tests PASSED! Fire generator is working correctly."));
    } else {
        Serial.print(F("⚠️  "));
        Serial.print(testsFailed_);
        Serial.println(F(" tests FAILED. Check implementation."));
    }
    Serial.println();
}
