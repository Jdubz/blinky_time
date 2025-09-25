#pragma once
#include "tests/FireEffectTest.h"

/**
 * EffectTestRunner - Simple test runner for visual effects
 * 
 * Can be integrated into the main sketch to run tests via serial commands
 * or during development to verify effect behavior.
 */
class EffectTestRunner {
private:
    FireEffectTest* fireTest_;
    
public:
    EffectTestRunner();
    ~EffectTestRunner();
    
    // Run all available tests
    void runAllTests();
    
    // Run specific test categories
    void runFireTests();
    
    // Quick validation test for deployment
    bool quickValidation();
    
    // Serial command interface
    void handleTestCommand(const char* command);
};