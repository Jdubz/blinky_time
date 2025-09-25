#include "FireTestRunner.h"
#include <Arduino.h>
#include <string.h>

FireTestRunner::FireTestRunner(int width, int height) 
    : testWidth_(width), testHeight_(height) {
    fireTest_ = new FireGeneratorTest(width, height);
}

FireTestRunner::~FireTestRunner() {
    delete fireTest_;
}

void FireTestRunner::runAllTests() {
    Serial.println(F("Starting Fire Generator Test Suite..."));
    fireTest_->runAllTests();
}

void FireTestRunner::runSpecificTest(const char* testName) {
    Serial.print(F("Running specific test: "));
    Serial.println(testName);
    
    if (strcmp(testName, "init") == 0 || strcmp(testName, "initialization") == 0) {
        fireTest_->testInitialization();
    } else if (strcmp(testName, "heat") == 0 || strcmp(testName, "heatmanagement") == 0) {
        fireTest_->testHeatManagement();
    } else if (strcmp(testName, "color") == 0 || strcmp(testName, "colorgeneration") == 0) {
        fireTest_->testColorGeneration();
    } else if (strcmp(testName, "matrix") == 0 || strcmp(testName, "matrixoutput") == 0) {
        fireTest_->testMatrixOutput();
    } else if (strcmp(testName, "progression") == 0 || strcmp(testName, "fireprogression") == 0) {
        fireTest_->testFireProgression();
    } else if (strcmp(testName, "audio") == 0 || strcmp(testName, "audioresponse") == 0) {
        fireTest_->testAudioResponse();
    } else if (strcmp(testName, "params") == 0 || strcmp(testName, "parameters") == 0) {
        fireTest_->testParameterEffects();
    } else if (strcmp(testName, "boundary") == 0 || strcmp(testName, "boundaries") == 0) {
        fireTest_->testBoundaryConditions();
    } else if (strcmp(testName, "perf") == 0 || strcmp(testName, "performance") == 0) {
        fireTest_->testPerformance();
    } else {
        Serial.print(F("Unknown test: "));
        Serial.println(testName);
        printHelp();
    }
}

bool FireTestRunner::handleCommand(const char* command) {
    if (!command) return false;
    
    // Convert to lowercase for easier matching
    char cmd[64];
    strncpy(cmd, command, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    
    for (int i = 0; cmd[i]; i++) {
        cmd[i] = tolower(cmd[i]);
    }
    
    if (strncmp(cmd, "fire", 4) == 0) {
        char* subCmd = cmd + 4;
        while (*subCmd == ' ') subCmd++; // Skip spaces
        
        if (strlen(subCmd) == 0 || strcmp(subCmd, "all") == 0) {
            runAllTests();
            return true;
        } else if (strcmp(subCmd, "help") == 0) {
            printHelp();
            return true;
        } else {
            runSpecificTest(subCmd);
            return true;
        }
    }
    
    return false; // Command not handled
}

void FireTestRunner::printHelp() const {
    Serial.println(F("=== Fire Generator Test Commands ==="));
    Serial.println(F("fire all        - Run all fire generator tests"));
    Serial.println(F("fire init       - Test initialization"));
    Serial.println(F("fire heat       - Test heat management"));
    Serial.println(F("fire color      - Test color generation"));
    Serial.println(F("fire matrix     - Test matrix output"));
    Serial.println(F("fire progression- Test fire progression"));
    Serial.println(F("fire audio      - Test audio response"));
    Serial.println(F("fire params     - Test parameter effects"));
    Serial.println(F("fire boundary   - Test boundary conditions"));
    Serial.println(F("fire perf       - Test performance"));
    Serial.println(F("fire help       - Show this help"));
    Serial.println();
}

bool FireTestRunner::getLastTestResult() const {
    return fireTest_->allTestsPassed();
}

void FireTestRunner::printLastResults() const {
    fireTest_->printResults();
}