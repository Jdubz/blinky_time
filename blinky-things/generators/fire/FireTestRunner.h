#pragma once
#include "FireGeneratorTest.h"

/**
 * FireTestRunner - Test runner specifically for Fire generator
 * 
 * Provides a simple interface to run fire generator tests via serial commands
 * or during development to verify fire generation behavior.
 * 
 * Part of the new architecture where each generator type has its own test suite.
 */
class FireTestRunner {
private:
    FireGeneratorTest* fireTest_;
    int testWidth_;
    int testHeight_;
    
public:
    FireTestRunner(int width = 4, int height = 15);
    ~FireTestRunner();
    
    // Test execution
    void runAllTests();
    void runSpecificTest(const char* testName);
    
    // Command interface for serial integration
    bool handleCommand(const char* command);
    void printHelp() const;
    
    // Results access
    bool getLastTestResult() const;
    void printLastResults() const;
};