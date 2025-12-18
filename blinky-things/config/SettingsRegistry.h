#pragma once

#include <Arduino.h>

/**
 * SettingsRegistry - Unified settings management for serial console
 *
 * Provides a registration-based system for exposing tunable parameters.
 * Register a setting once, and it automatically gets:
 * - Serial command handling (set <name> <value>)
 * - Value display (get <name>, show, show <category>)
 * - Help text generation
 * - Optional bounds checking
 * - Optional change callbacks
 *
 * Usage:
 *   SettingsRegistry settings;
 *   settings.begin();
 *   settings.registerFloat("cooling", &fireParams.baseCooling, "fire", "Base cooling rate", 0, 255);
 *   settings.registerFloat("sparkchance", &fireParams.sparkChance, "fire", "Spark probability", 0, 1);
 *
 *   // In loop:
 *   if (settings.handleCommand(cmd)) { ... }
 */

// Maximum number of settings (adjust based on needs)
#define MAX_SETTINGS 48

// Setting value types
enum class SettingType : uint8_t {
    UINT8,
    INT8,
    UINT16,
    UINT32,
    FLOAT,
    BOOL
};

// Callback type for when a setting changes
typedef void (*SettingCallback)();

// Setting definition
struct Setting {
    const char* name;           // Command name (e.g., "cooling")
    const char* category;       // Category for grouping (e.g., "fire", "audio")
    const char* description;    // Help text
    SettingType type;
    void* valuePtr;             // Pointer to the actual value
    float minVal;               // Minimum allowed value
    float maxVal;               // Maximum allowed value
    SettingCallback onChange;   // Optional callback when value changes
    bool persistent;            // Whether to include in save/load
};

class SettingsRegistry {
public:
    SettingsRegistry();

    void begin();

    // Registration methods - return true if successful
    bool registerUint8(const char* name, uint8_t* value, const char* category,
                       const char* desc, uint8_t minVal = 0, uint8_t maxVal = 255,
                       SettingCallback onChange = nullptr, bool persistent = true);

    bool registerInt8(const char* name, int8_t* value, const char* category,
                      const char* desc, int8_t minVal = -128, int8_t maxVal = 127,
                      SettingCallback onChange = nullptr, bool persistent = true);

    bool registerUint16(const char* name, uint16_t* value, const char* category,
                        const char* desc, uint16_t minVal = 0, uint16_t maxVal = 65535,
                        SettingCallback onChange = nullptr, bool persistent = true);

    bool registerUint32(const char* name, uint32_t* value, const char* category,
                        const char* desc, uint32_t minVal = 0, uint32_t maxVal = 0xFFFFFFFF,
                        SettingCallback onChange = nullptr, bool persistent = true);

    bool registerFloat(const char* name, float* value, const char* category,
                       const char* desc, float minVal = 0.0f, float maxVal = 1.0f,
                       SettingCallback onChange = nullptr, bool persistent = true);

    bool registerBool(const char* name, bool* value, const char* category,
                      const char* desc, SettingCallback onChange = nullptr,
                      bool persistent = true);

    // Command handling - returns true if command was handled
    bool handleCommand(const char* cmd);

    // Display methods
    void printAll();                           // Print all settings
    void printCategory(const char* category);  // Print settings in category
    void printHelp();                          // Print help with all commands
    void printValue(const char* name);         // Print single value

    // Utility
    Setting* findSetting(const char* name);
    uint8_t getSettingCount() const { return numSettings_; }
    const Setting* getSetting(uint8_t index) const;

    // Category enumeration
    void printCategories();

private:
    Setting settings_[MAX_SETTINGS];
    uint8_t numSettings_;

    bool registerSetting(const Setting& setting);
    bool setValue(Setting* s, const char* valueStr);
    void printSettingValue(const Setting& s);
    void printSettingHelp(const Setting& s);

    // Parse helpers
    static bool parseFloat(const char* str, float& out);
    static bool parseInt(const char* str, int32_t& out);
    static bool parseBool(const char* str, bool& out);
};
