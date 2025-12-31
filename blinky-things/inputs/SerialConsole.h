#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../config/SettingsRegistry.h"

// Forward declarations
class ConfigStorage;
class Fire;
class AdaptiveMic;
class BatteryMonitor;
class AudioController;

/**
 * SerialConsole - JSON API for web app communication
 *
 * Supported Commands:
 *   JSON API (for web app):
 *     json info           - Device info as JSON
 *     json settings       - All settings as JSON with metadata
 *     stream on/off       - Audio data streaming (~20Hz)
 *
 *   Settings (via SettingsRegistry):
 *     set <name> <value>  - Set a parameter value
 *     get <name>          - Get a parameter value
 *     show [category]     - Show all settings or by category
 *     list                - List all settings (alias for show)
 *     categories          - List all setting categories
 *     settings            - Show settings with help text
 *
 *   Configuration:
 *     save                - Save settings to flash
 *     load                - Load settings from flash
 *     defaults            - Restore default values
 *     reset / factory     - Factory reset (clear saved settings)
 */
class SerialConsole {
public:
    SerialConsole(Fire* fireGen, AdaptiveMic* mic);

    void begin();
    void update();

    // External access
    void setConfigStorage(ConfigStorage* storage) { configStorage_ = storage; }
    void setFireGenerator(Fire* fireGen) { fireGenerator_ = fireGen; }
    void setBatteryMonitor(BatteryMonitor* battery) { battery_ = battery; }
    void setAudioController(AudioController* audioCtrl) { audioCtrl_ = audioCtrl; }
    SettingsRegistry& getSettings() { return settings_; }

private:
    void registerSettings();
    void handleCommand(const char* cmd);
    bool handleSpecialCommand(const char* cmd);
    void restoreDefaults();
    void streamTick();

    // Settings registration helpers (extracted from registerSettings for clarity)
    void registerFireSettings(FireParams* fp);
    void registerFireMusicSettings(FireParams* fp);
    void registerFireOrganicSettings(FireParams* fp);
    void registerAudioSettings();
    void registerAgcSettings();
    void registerTransientSettings();
    void registerDetectionSettings();
    void registerRhythmSettings();

    // Command handlers (extracted from handleSpecialCommand for clarity)
    bool handleJsonCommand(const char* cmd);
    bool handleBatteryCommand(const char* cmd);
    bool handleStreamCommand(const char* cmd);
    bool handleTestCommand(const char* cmd);
    bool handleAudioStatusCommand(const char* cmd);
    bool handlePresetCommand(const char* cmd);
    bool handleModeCommand(const char* cmd);
    bool handleConfigCommand(const char* cmd);

    // Members
    Fire* fireGenerator_;
    AdaptiveMic* mic_;
    BatteryMonitor* battery_;
    AudioController* audioCtrl_;
    ConfigStorage* configStorage_;
    SettingsRegistry settings_;

    // JSON streaming state (for web app)
    bool streamEnabled_ = false;
    bool streamDebug_ = false;     // Include debug fields (baselines, raw energy)
    bool streamFast_ = false;      // 100Hz mode for testing
    uint32_t streamLastMs_ = 0;
    uint32_t batteryLastMs_ = 0;
    static const uint16_t STREAM_PERIOD_MS = 50;        // Normal: ~20Hz for audio
    static const uint16_t STREAM_FAST_PERIOD_MS = 10;   // Fast: ~100Hz for testing
    static const uint16_t BATTERY_PERIOD_MS = 1000;     // 1Hz for battery

    // Static instance pointer for callbacks
    static SerialConsole* instance_;
};
