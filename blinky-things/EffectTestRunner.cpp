#include "EffectTestRunner.h"
#include <Arduino.h>

EffectTestRunner::EffectTestRunner() {
    fireTest_ = nullptr;
}

EffectTestRunner::~EffectTestRunner() {
    delete fireTest_;
}

void EffectTestRunner::runAllTests() {
    Serial.println(F("=== Running All Effect Tests ==="));
    runFireTests();
}

void EffectTestRunner::runFireTests() {
    Serial.println(F("\n=== Fire Effect Tests ==="));
    
    // Create test instance with tubelight dimensions
    if (!fireTest_) {
        fireTest_ = new FireEffectTest(4, 15);
    }
    
    fireTest_->runAllTests();
}

bool EffectTestRunner::quickValidation() {
    Serial.println(F("=== Quick Validation Test ==="));
    
    if (!fireTest_) {
        fireTest_ = new FireEffectTest(4, 15);
    }
    
    // Run essential tests only
    bool colorTest = fireTest_->testColorPalette();
    bool matrixTest = fireTest_->testMatrixGeneration();
    
    bool allPassed = colorTest && matrixTest;
    
    if (allPassed) {
        Serial.println(F("✓ Quick validation PASSED"));
    } else {
        Serial.println(F("✗ Quick validation FAILED"));
    }
    
    return allPassed;
}

void EffectTestRunner::handleTestCommand(const char* command) {
    if (strcmp(command, "test all") == 0) {
        runAllTests();
    } else if (strcmp(command, "test fire") == 0) {
        runFireTests();
    } else if (strcmp(command, "test quick") == 0) {
        quickValidation();
    } else if (strcmp(command, "test colors") == 0) {
        if (!fireTest_) {
            fireTest_ = new FireEffectTest(4, 15);
        }
        fireTest_->testColorPalette();
        fireTest_->testHeatToColor();
    } else {
        Serial.println(F("Available test commands:"));
        Serial.println(F("  test all    - Run all tests"));
        Serial.println(F("  test fire   - Run fire effect tests"));
        Serial.println(F("  test quick  - Quick validation"));
        Serial.println(F("  test colors - Test color generation"));
    }
}