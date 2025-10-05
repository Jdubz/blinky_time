#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "../generators/UnifiedFireGenerator.h"
#include "../config/Globals.h"

// Forward declarations to avoid circular includes
class ConfigStorage;
class UnifiedFireGenerator;
class GeneratorTestRunner;

class SerialConsole {
public:
    SerialConsole(UnifiedFireGenerator* fireGen, Adafruit_NeoPixel &leds);

    void begin();
    void update();
    void handleCommand(const char *cmd);

    // Set config storage for parameter persistence
    void setConfigStorage(ConfigStorage* storage) { configStorage_ = storage; }

    // Set string fire effect for configuration (when using STRING_FIRE mode)
    void setUnifiedFireGenerator(UnifiedFireGenerator* fireGen) { fireGenerator_ = fireGen; }

    void restoreDefaults();
    void printAll();
    void renderIMUVisualization();        // IMU orientation visualization
    void renderTopVisualization();        // Cylinder top column visualization
    void renderBatteryVisualization();    // Battery charge level visualization
    void renderTestPattern();             // RGB test pattern for layout verification

private:
    // Helper function for consistent matrix mapping
    int xyToPixelIndex(int x, int y);

    // Helper for saving fire parameters to EEPROM
    void saveFireParameterToEEPROM(const char* paramName);

public:
    // IMU visualization mode
    bool imuVizEnabled = false;          // Enable IMU visualization on matrix
    bool fireDisabled = false;           // Disable fire when showing IMU viz
    bool heatVizEnabled = false;         // Show cylinder top column visualization

    // Battery visualization mode
    bool batteryVizEnabled = false;      // Enable battery charge visualization

    // Test pattern mode
    bool testPatternEnabled = false;     // Enable test pattern for layout verification


private:
    UnifiedFireGenerator* fireGenerator_; // Unified fire generator
    Adafruit_NeoPixel &leds;
    ConfigStorage* configStorage_;        // For saving parameters to EEPROM
    GeneratorTestRunner* testRunner_;     // For running generator tests

    // ---- Debug systems ----
    bool micDebugEnabled = false;         // toggled by "mic debug on/off"
    unsigned long micDebugPeriodMs = 200; // "mic debug rate <ms>"
    unsigned long micDebugLastMs   = 0;

    bool debugEnabled = false;            // General debug output
    unsigned long debugPeriodMs = 500;    // General debug rate
    unsigned long debugLastMs = 0;

    bool imuDebugEnabled = false;         // Real-time IMU debug output
    unsigned long imuDebugPeriodMs = 200; // IMU debug rate
    unsigned long imuDebugLastMs = 0;

    void micDebugTick();                  // periodic printer
    void micDebugPrintLine();             // one line snapshot
    void debugTick();                     // general debug output
    void printFireStats();               // fire engine statistics
    void imuDebugTick();                  // periodic IMU debug output
    void printRawIMUData();               // one-time raw IMU snapshot
};

#endif
