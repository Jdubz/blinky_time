#pragma once
#include "generators/fire/FireTestRunner.h"

/**
 * GeneratorTestRunner - Main test coordinator for all generator types
 * 
 * This replaces the old EffectTestRunner and coordinates testing of all
 * generator types (Fire, Stars, Waves, etc.) in the new architecture.
 * 
 * Each generator type has its own specialized test runner and test suite.
 */
class GeneratorTestRunner {
private:
    FireTestRunner* fireTestRunner_;
    int matrixWidth_;
    int matrixHeight_;
    
public:
    GeneratorTestRunner(int width = 4, int height = 15);
    ~GeneratorTestRunner();
    
    // Test execution
    void runAllTests();
    void runGeneratorTests(const char* generatorType);
    
    // Command interface for serial integration
    bool handleCommand(const char* command);
    void printHelp() const;
    
    // Results access
    bool getLastTestResult() const;
    void printSystemStatus() const;
};