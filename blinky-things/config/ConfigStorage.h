#pragma once

#include <Arduino.h>
#include "../generators/legacy-fire/FireGenerator.h"
#include "../generators/matrix-fire/MatrixFireGenerator.h"
#include "../generators/string-fire/StringFireGenerator.h"
#include "../hardware/AdaptiveMic.h"

// Include EEPROM library for platforms that support it (not needed for nRF52 variants)
#if !defined(ARDUINO_ARCH_NRF52) && !defined(NRF52) && !defined(TARGET_NAME) && !defined(MBED_CONF_TARGET_NAME)
  #if defined(ESP32) || defined(ARDUINO_ARCH_AVR)
    #include <EEPROM.h>
  #endif
#endif

// Simple configuration storage that works across platforms
// On nRF52: Parameters reset to defaults on each boot (no persistence yet)
// On ESP32/AVR: Uses EEPROM for persistence

// EEPROM Configuration Storage for Blinky Things
// Handles persistent storage of all runtime-configurable parameters

class ConfigStorage {
public:
    // Configuration constants
    static const uint16_t MAGIC_NUMBER = 0x8F1E; // "FIRE" in hex-like
    static const uint8_t CONFIG_VERSION = 1;
    
    // File name for nRF52 internal filesystem
    static constexpr const char* CONFIG_FILE = "/blinky_config.bin";
    
    struct ConfigHeader {
        uint16_t magic;
        uint8_t version;
        uint8_t deviceType;  // Current device type (1=Hat, 2=Tube, 3=Bucket)
        uint32_t checksum;
        uint8_t reserved[8];
    };
    
    // Forward declare storage structures before use
    struct StoredFireParams {
        // Core fire effect parameters
        uint8_t baseCooling;
        uint8_t sparkHeatMin;
        uint8_t sparkHeatMax;
        float sparkChance;
        float audioSparkBoost;
        uint8_t audioHeatBoostMax;
        int8_t coolingAudioBias;
        uint8_t bottomRowsForSparks;
        uint8_t transientHeatMax;
        uint8_t reserved[7]; // Padding for future expansion
    };
    
    struct StoredMicParams {
        // Audio processing parameters
        float attackSeconds;
        float releaseSeconds;
        float noiseGate;
        float globalGain;
        uint32_t transientCooldownMs;
        bool agEnabled;
        float agTarget;
        float agStrength;
        float transientFactor;
        float loudFloor;
        float transientDecay;
        float compRatio;
        float compThresh;
        uint8_t reserved[15]; // Padding for future expansion
    };
    
    struct ConfigData {
        ConfigHeader header;
        StoredFireParams fireParams;
        StoredMicParams micParams;
    };
    
    ConfigStorage();
    
    // Main interface
    void begin();
    bool isValid() const { return valid_; }
    
    // Load/Save all parameters
    void loadConfiguration(FireParams& fireParams, AdaptiveMic& mic);
    void saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic);
    
    // Overloads for MatrixFireGenerator
    void loadConfiguration(MatrixFireParams& matrixFireParams, AdaptiveMic& mic);
    void saveConfiguration(const MatrixFireParams& matrixFireParams, const AdaptiveMic& mic);
    
    // Overloads for StringFireGenerator
    void loadConfiguration(StringFireParams& stringFireParams, AdaptiveMic& mic);
    void saveConfiguration(const StringFireParams& stringFireParams, const AdaptiveMic& mic);
    
    // Device type management
    void setDeviceType(uint8_t deviceType);
    uint8_t getDeviceType() const { return configData_.header.deviceType; }
    
    // Factory reset
    void factoryReset();
    
    // Individual parameter save (for immediate persistence)
    void saveFireParam(const char* paramName, const FireParams& params);
    void saveMicParam(const char* paramName, const AdaptiveMic& mic);
    
    // Overloads for MatrixFireGenerator
    void saveMatrixFireParam(const char* paramName, const MatrixFireParams& params);
    
    // Overloads for StringFireGenerator
    void saveStringFireParam(const char* paramName, const StringFireParams& params);
    
    // Status and diagnostics
    void printStatus() const;
    uint32_t calculateChecksum(const void* data, size_t size) const;
    
private:
    ConfigData configData_;
    bool valid_;
    bool needsSave_;
    
    // Internal helpers
    bool loadFromStorage();
    void saveToStorage();
    void loadDefaults();
    void copyFireParamsTo(FireParams& params) const;
    void copyFireParamsFrom(const FireParams& params);
    void copyMicParamsTo(AdaptiveMic& mic) const;
    void copyMicParamsFrom(const AdaptiveMic& mic);
    
    // MatrixFire parameter copying
    void copyMatrixFireParamsTo(MatrixFireParams& params) const;
    void copyMatrixFireParamsFrom(const MatrixFireParams& params);
    
    // StringFire parameter copying
    void copyStringFireParamsTo(StringFireParams& params) const;
    void copyStringFireParamsFrom(const StringFireParams& params);
    
    // Note: EEPROM functionality for ESP32/AVR platforms will be implemented when needed
    // Currently all platforms use defaults-only mode
};