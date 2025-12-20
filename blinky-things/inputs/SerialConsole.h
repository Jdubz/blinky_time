#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../config/SettingsRegistry.h"

// Forward declarations
class ConfigStorage;
class Fire;
class AdaptiveMic;

/**
 * SerialConsole - JSON API for web app communication
 *
 * Supported Commands:
 *   json info           - Device info as JSON
 *   json settings       - All settings as JSON with metadata
 *   set <name> <value>  - Set a parameter value
 *   stream on/off       - Audio data streaming (~20Hz)
 *   save                - Save settings to flash
 *   load                - Load settings from flash
 *   defaults            - Restore default values
 *   reset               - Factory reset
 */
class SerialConsole {
public:
    SerialConsole(Fire* fireGen, AdaptiveMic* mic);

    void begin();
    void update();

    // External access
    void setConfigStorage(ConfigStorage* storage) { configStorage_ = storage; }
    void setFireGenerator(Fire* fireGen) { fireGenerator_ = fireGen; }
    SettingsRegistry& getSettings() { return settings_; }

private:
    void registerSettings();
    void handleCommand(const char* cmd);
    bool handleSpecialCommand(const char* cmd);
    void restoreDefaults();
    void streamTick();

    // Callbacks for settings changes
    static void onFireParamChanged();

    // Members
    Fire* fireGenerator_;
    AdaptiveMic* mic_;
    ConfigStorage* configStorage_;
    SettingsRegistry settings_;

    // JSON streaming state (for web app)
    bool streamEnabled_ = false;
    uint32_t streamLastMs_ = 0;
    static const uint16_t STREAM_PERIOD_MS = 50;  // ~20Hz

    // Static instance pointer for callbacks
    static SerialConsole* instance_;
};
