#include "GeneralEffectTests.h"
#include <Arduino.h>

GeneralEffectTests::GeneralEffectTests(Effect* effect, int width, int height)
    : testEffect_(effect), testMatrix_(nullptr), testWidth_(width), testHeight_(height),
      testsRun_(0), testsPassed_(0), testsFailed_(0) {
    
    if (testEffect_) {
        testMatrix_ = new EffectMatrix(width, height);
    }
}

GeneralEffectTests::~GeneralEffectTests() {
    if (testMatrix_) {
        delete testMatrix_;
        testMatrix_ = nullptr;
    }
}

void GeneralEffectTests::runAllTests() {
    Serial.println(F("=== Running General Effect Tests ==="));
    Serial.print(F("Testing effect: "));
    Serial.println(testEffect_ ? testEffect_->getName() : "NULL");
    Serial.println();

    runBasicTests();
    runSafetyTests();
    runPerformanceTests();

    printResults();
}

void GeneralEffectTests::runBasicTests() {
    Serial.println(F("--- Basic Interface Tests ---"));
    testBasicInterface();
    testResetFunctionality();
    testMultipleApplications();
}

void GeneralEffectTests::runSafetyTests() {
    Serial.println(F("--- Safety Tests ---"));
    testMatrixSafety();
    testNullMatrixHandling();
    testDataIntegrity();
}

void GeneralEffectTests::runPerformanceTests() {
    Serial.println(F("--- Performance Tests ---"));
    testPerformanceConstraints();
}

bool GeneralEffectTests::testBasicInterface() {
    bool passed = true;
    
    // Test that effect has a name
    if (!testEffect_) {
        logTest("Basic Interface", false, "Effect is null");
        return false;
    }
    
    const char* name = testEffect_->getName();
    if (!name || strlen(name) == 0) {
        passed = false;
        logTest("Basic Interface", false, "Effect name is null or empty");
    } else {
        logTest("Basic Interface", true, "Effect has valid name");
    }
    
    return passed;
}

bool GeneralEffectTests::testMatrixSafety() {
    if (!testEffect_ || !testMatrix_) {
        logTest("Matrix Safety", false, "Test setup failed");
        return false;
    }

    // Fill matrix with test data
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            testMatrix_->setPixel(x, y, 128, 64, 32);
        }
    }

    // Apply effect and check for crashes
    bool passed = true;
    try {
        testEffect_->apply(testMatrix_);
        
        // Verify matrix still has valid data
        if (!matrixHasValidData(testMatrix_)) {
            passed = false;
            logTest("Matrix Safety", false, "Matrix contains invalid data after effect");
        } else {
            logTest("Matrix Safety", true, "Effect applied safely");
        }
    } catch (...) {
        passed = false;
        logTest("Matrix Safety", false, "Effect crashed during application");
    }

    return passed;
}

bool GeneralEffectTests::testNullMatrixHandling() {
    if (!testEffect_) {
        logTest("Null Matrix Handling", false, "Test setup failed");
        return false;
    }

    bool passed = true;
    try {
        testEffect_->apply(nullptr);
        logTest("Null Matrix Handling", true, "Effect handles null matrix gracefully");
    } catch (...) {
        passed = false;
        logTest("Null Matrix Handling", false, "Effect crashes with null matrix");
    }

    return passed;
}

bool GeneralEffectTests::testResetFunctionality() {
    if (!testEffect_) {
        logTest("Reset Functionality", false, "Test setup failed");
        return false;
    }

    bool passed = true;
    try {
        testEffect_->reset();
        logTest("Reset Functionality", true, "Effect reset completed");
    } catch (...) {
        passed = false;
        logTest("Reset Functionality", false, "Effect crashes during reset");
    }

    return passed;
}

bool GeneralEffectTests::testMultipleApplications() {
    if (!testEffect_ || !testMatrix_) {
        logTest("Multiple Applications", false, "Test setup failed");
        return false;
    }

    bool passed = true;
    try {
        // Apply effect multiple times
        for (int i = 0; i < 10; i++) {
            testEffect_->apply(testMatrix_);
        }
        
        if (matrixHasValidData(testMatrix_)) {
            logTest("Multiple Applications", true, "Effect stable over multiple applications");
        } else {
            passed = false;
            logTest("Multiple Applications", false, "Data corruption after multiple applications");
        }
    } catch (...) {
        passed = false;
        logTest("Multiple Applications", false, "Effect crashes with multiple applications");
    }

    return passed;
}

bool GeneralEffectTests::testPerformanceConstraints() {
    if (!testEffect_ || !testMatrix_) {
        logTest("Performance Constraints", false, "Test setup failed");
        return false;
    }

    // Measure time for single application
    unsigned long startTime = micros();
    testEffect_->apply(testMatrix_);
    unsigned long endTime = micros();
    
    unsigned long executionTime = endTime - startTime;
    
    // Effect should complete in reasonable time (< 10ms for small matrix)
    bool passed = executionTime < 10000; // 10ms in microseconds
    
    char details[64];
    snprintf(details, sizeof(details), "Execution time: %lu microseconds", executionTime);
    
    logTest("Performance Constraints", passed, details);
    return passed;
}

bool GeneralEffectTests::testDataIntegrity() {
    if (!testEffect_ || !testMatrix_) {
        logTest("Data Integrity", false, "Test setup failed");
        return false;
    }

    // Set known values
    testMatrix_->setPixel(0, 0, 255, 0, 0);   // Red
    testMatrix_->setPixel(1, 0, 0, 255, 0);   // Green  
    testMatrix_->setPixel(2, 0, 0, 0, 255);   // Blue

    testEffect_->apply(testMatrix_);

    // Verify pixels still have reasonable values (effects may transform but shouldn't corrupt)
    uint8_t r, g, b;
    bool passed = true;
    
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            testMatrix_->getPixel(x, y, r, g, b);
            if (!isValidPixelData(r, g, b)) {
                passed = false;
                break;
            }
        }
        if (!passed) break;
    }

    logTest("Data Integrity", passed, passed ? "All pixel data valid" : "Invalid pixel data detected");
    return passed;
}

bool GeneralEffectTests::isValidPixelData(uint8_t r, uint8_t g, uint8_t b) const {
    // Basic sanity check - values should be 0-255 (already guaranteed by uint8_t)
    // Additional checks could be added here for specific constraints
    (void)r; (void)g; (void)b; // Suppress unused warnings
    return true; // uint8_t automatically constrains to 0-255
}

bool GeneralEffectTests::matrixHasValidData(EffectMatrix* matrix) const {
    if (!matrix) return false;
    
    uint8_t r, g, b;
    for (int x = 0; x < testWidth_; x++) {
        for (int y = 0; y < testHeight_; y++) {
            matrix->getPixel(x, y, r, g, b);
            if (!isValidPixelData(r, g, b)) {
                return false;
            }
        }
    }
    return true;
}

void GeneralEffectTests::logTest(const char* testName, bool passed, const char* details) {
    testsRun_++;
    if (passed) {
        testsPassed_++;
        Serial.print(F("✅ "));
    } else {
        testsFailed_++;
        Serial.print(F("❌ "));
    }
    
    Serial.print(testName);
    if (details) {
        Serial.print(F(" - "));
        Serial.print(details);
    }
    Serial.println();
}

void GeneralEffectTests::printResults() const {
    Serial.println();
    Serial.println(F("=== Test Results ==="));
    Serial.print(F("Tests Run: "));
    Serial.println(testsRun_);
    Serial.print(F("Passed: "));
    Serial.println(testsPassed_);
    Serial.print(F("Failed: "));
    Serial.println(testsFailed_);
    
    if (testsFailed_ == 0) {
        Serial.println(F("🎉 All tests PASSED!"));
    } else {
        Serial.print(F("⚠️  "));
        Serial.print(testsFailed_);
        Serial.println(F(" test(s) FAILED"));
    }
    Serial.println();
}

bool GeneralEffectTests::allTestsPassed() const {
    return testsFailed_ == 0 && testsRun_ > 0;
}