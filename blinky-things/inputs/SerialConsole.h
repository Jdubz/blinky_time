#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../generators/Water.h"
#include "../generators/Lightning.h"
#include "../config/SettingsRegistry.h"

// Forward declarations
class ConfigStorage;
class Fire;
class Water;
class Lightning;
class AdaptiveMic;
class BatteryMonitor;
class AudioController;
class RenderPipeline;
class HueRotationEffect;

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
 *
 *   Generator/Effect Control:
 *     gen list            - List available generators
 *     gen <name>          - Switch to generator (fire, water, lightning)
 *     effect list         - List available effects
 *     effect <name>       - Switch to effect (none, hue)
 *
 *   Logging:
 *     log                 - Show current log level
 *     log off             - Disable all logging
 *     log error           - Show errors only
 *     log warn            - Show warnings and errors
 *     log info            - Show info, warnings, and errors (default)
 *     log debug           - Show all messages including debug
 *
 *   Debug Channels (independent per-subsystem control):
 *     debug               - Show enabled debug channels
 *     debug <ch> on/off   - Enable/disable channel (transient, rhythm, hypothesis, audio, generator, ensemble)
 *     debug all on/off    - Enable/disable all channels
 *
 *   Note: Debug channels control JSON debug output independently from log levels.
 *   Use for testing specific subsystems without flooding serial with unrelated messages.
 */

// Log levels (higher = more verbose)
enum class LogLevel : uint8_t {
    OFF = 0,
    ERROR = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4
};

// Debug channels - bit flags for independent subsystem debug control
// Each channel can be enabled/disabled separately to avoid serial flooding
// Usage: debug transient on, debug rhythm off, debug all off
enum class DebugChannel : uint16_t {
    NONE       = 0x0000,
    TRANSIENT  = 0x0001,  // Transient detection events (pulse > 0)
    RHYTHM     = 0x0002,  // Rhythm/BPM tracking (RHYTHM_DEBUG, RHYTHM_DEBUG2)
    HYPOTHESIS = 0x0004,  // Multi-hypothesis tracker (HYPO_* events)
    AUDIO      = 0x0008,  // Audio level, AGC, mic debug
    GENERATOR  = 0x0010,  // Generator-specific debug (fire, water, lightning)
    ENSEMBLE   = 0x0020,  // Ensemble detector internals
    ALL        = 0xFFFF
};

// Bitwise operators for DebugChannel
inline DebugChannel operator|(DebugChannel a, DebugChannel b) {
    return static_cast<DebugChannel>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline DebugChannel operator&(DebugChannel a, DebugChannel b) {
    return static_cast<DebugChannel>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
inline DebugChannel operator~(DebugChannel a) {
    return static_cast<DebugChannel>(~static_cast<uint16_t>(a));
}
inline DebugChannel& operator|=(DebugChannel& a, DebugChannel b) {
    a = a | b;
    return a;
}
inline DebugChannel& operator&=(DebugChannel& a, DebugChannel b) {
    a = a & b;
    return a;
}
class SerialConsole {
public:
    SerialConsole(RenderPipeline* pipeline, AdaptiveMic* mic);

    void begin();
    void update();

    // External access
    void setConfigStorage(ConfigStorage* storage) { configStorage_ = storage; }
    void setRenderPipeline(RenderPipeline* pipeline) { pipeline_ = pipeline; }
    void setFireGenerator(Fire* fireGen) { fireGenerator_ = fireGen; }
    void setBatteryMonitor(BatteryMonitor* battery) { battery_ = battery; }
    void setAudioController(AudioController* audioCtrl) { audioCtrl_ = audioCtrl; }
    SettingsRegistry& getSettings() { return settings_; }

    // Logging control
    void setLogLevel(LogLevel level) { logLevel_ = level; }
    LogLevel getLogLevel() const { return logLevel_; }
    static LogLevel getGlobalLogLevel() { return instance_ ? instance_->logLevel_ : LogLevel::INFO; }

    // Debug channel control - allows independent enable/disable of subsystem debug output
    // Usage: debug transient on, debug rhythm off, debug hypothesis on, debug all off
    static void enableDebugChannel(DebugChannel channel) {
        debugChannels_ = debugChannels_ | channel;
    }
    static void disableDebugChannel(DebugChannel channel) {
        debugChannels_ = debugChannels_ & ~channel;
    }
    static bool isDebugChannelEnabled(DebugChannel channel) {
        return (debugChannels_ & channel) == channel;
    }
    static DebugChannel getDebugChannels() { return debugChannels_; }
    static void setDebugChannels(DebugChannel channels) { debugChannels_ = channels; }

    // Logging helpers - use these instead of Serial.print for debug output
    static void logDebug(const __FlashStringHelper* msg);
    static void logInfo(const __FlashStringHelper* msg);
    static void logWarn(const __FlashStringHelper* msg);
    static void logError(const __FlashStringHelper* msg);

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
    void registerEnsembleSettings();  // Ensemble detector configuration
    void registerRhythmSettings();

    // Command handlers (extracted from handleSpecialCommand for clarity)
    bool handleJsonCommand(const char* cmd);
    bool handleBatteryCommand(const char* cmd);
    bool handleStreamCommand(const char* cmd);
    bool handleTestCommand(const char* cmd);
    bool handleAudioStatusCommand(const char* cmd);
    bool handleModeCommand(const char* cmd);
    bool handleConfigCommand(const char* cmd);
    bool handleGeneratorCommand(const char* cmd);
    bool handleEffectCommand(const char* cmd);
    bool handleLogCommand(const char* cmd);
    bool handleDebugCommand(const char* cmd);     // Debug channel control
    bool handleEnsembleCommand(const char* cmd);  // Ensemble detector configuration
    bool handleHypothesisCommand(const char* cmd);  // Multi-hypothesis tracking commands

    // Settings registration for other generators
    void registerWaterSettings(WaterParams* wp);
    void registerLightningSettings(LightningParams* lp);
    void registerEffectSettings();
    void syncEffectSettings();  // Apply effect settings to actual effect

    // Members
    RenderPipeline* pipeline_;
    Fire* fireGenerator_;
    Water* waterGenerator_;
    Lightning* lightningGenerator_;
    HueRotationEffect* hueEffect_;
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

    // Logging level (default: INFO)
    LogLevel logLevel_ = LogLevel::INFO;

    // Static instance pointer for callbacks
    static SerialConsole* instance_;

    // Debug channels (default: NONE - all debug output disabled)
    // Each channel can be enabled independently for targeted debugging
    static DebugChannel debugChannels_;
};
