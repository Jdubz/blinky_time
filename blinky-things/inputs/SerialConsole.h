#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "../generators/Fire.h"
#include "../config/SettingsRegistry.h"
#include "../config/Globals.h"

// Forward declarations
class ConfigStorage;
class Fire;
class GeneratorTestRunner;
class AdaptiveMic;

/**
 * SerialConsole - Interactive serial command interface
 *
 * Features:
 * - Settings-based parameter tuning via SettingsRegistry
 * - Debug output modes (mic, fire, IMU)
 * - Visualization modes (IMU, battery, test pattern)
 * - Configuration persistence
 *
 * Setting Commands (handled by SettingsRegistry):
 *   set <name> <value>      - Set a parameter
 *   get <name>              - Get current value
 *   show                    - Show all settings
 *   show <category>         - Show category (fire, audio, agc, debug)
 *   settings                - Show all settings with descriptions
 *
 * Special Commands:
 *   help                    - Show all available commands
 *   version                 - Show version info
 *   defaults                - Restore default values
 *   save                    - Save settings to flash
 *   load                    - Load settings from flash
 *   reset                   - Factory reset
 *
 * Debug Commands:
 *   mic stats               - Show mic statistics
 *   fire stats              - Show fire statistics
 *   battery stats           - Show battery statistics
 *
 * Visualization Commands:
 *   imu viz on/off          - IMU orientation display
 *   battery viz on/off      - Battery level display
 *   test pattern on/off     - Test pattern display
 *   fire disable/enable     - Disable fire for visualization
 */
class SerialConsole {
public:
    SerialConsole(Fire* fireGen, AdaptiveMic* mic, Adafruit_NeoPixel& leds);

    void begin();
    void update();

    // External access
    void setConfigStorage(ConfigStorage* storage) { configStorage_ = storage; }
    void setFireGenerator(Fire* fireGen) { fireGenerator_ = fireGen; }
    SettingsRegistry& getSettings() { return settings_; }

    // Visualization rendering (called from main loop when enabled)
    void renderIMUVisualization();
    void renderTopVisualization();
    void renderBatteryVisualization();
    void renderTestPattern();

    // Mode flags - public for main loop access
    bool imuVizEnabled = false;
    bool fireDisabled = false;
    bool heatVizEnabled = false;
    bool batteryVizEnabled = false;
    bool testPatternEnabled = false;

private:
    void registerSettings();
    void handleCommand(const char* cmd);
    bool handleSpecialCommand(const char* cmd);
    void printHelp();

    // Callbacks for settings changes
    static void onFireParamChanged();

    // Debug/stats output
    void micDebugTick();
    void micDebugPrintLine();
    void debugTick();
    void imuDebugTick();
    void printRawIMUData();
    void printFireStats();
    void printMicStats();
    void printBatteryStats();
    void printVersionInfo();
    void restoreDefaults();

    // Helpers
    int xyToPixelIndex(int x, int y);

    // Members
    Fire* fireGenerator_;
    AdaptiveMic* mic_;
    Adafruit_NeoPixel& leds_;
    ConfigStorage* configStorage_;
    GeneratorTestRunner* testRunner_;
    SettingsRegistry settings_;

    // Debug state
    bool micDebugEnabled_ = false;
    bool debugEnabled_ = false;
    bool imuDebugEnabled_ = false;

    uint32_t micDebugLastMs_ = 0;
    uint32_t debugLastMs_ = 0;
    uint32_t imuDebugLastMs_ = 0;

    uint16_t micDebugPeriodMs_ = 200;
    uint16_t debugPeriodMs_ = 500;
    uint16_t imuDebugPeriodMs_ = 200;

    // Static instance pointer for callbacks
    static SerialConsole* instance_;
};
