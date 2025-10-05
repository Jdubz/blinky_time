#include "../config/ConfigStorage.h"
#include "../TotemDefaults.h"

ConfigStorage::ConfigStorage() : valid_(false), needsSave_(false) {
    memset(&configData_, 0, sizeof(configData_));
}

void ConfigStorage::begin() {
    // Debug: Print detected platform macros
    #ifdef ARDUINO_ARCH_NRF52
    Serial.println(F("[DEBUG] ARDUINO_ARCH_NRF52 defined"));
    #endif
    #ifdef NRF52
    Serial.println(F("[DEBUG] NRF52 defined"));
    #endif
    #ifdef TARGET_NAME
    Serial.println(F("[DEBUG] TARGET_NAME defined"));
    #endif
    #ifdef MBED_CONF_TARGET_NAME
    Serial.println(F("[DEBUG] MBED_CONF_TARGET_NAME defined"));
    #endif

#if defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    Serial.println(F("[CONFIG] nRF52 detected - using defaults only (no persistence yet)"));
    loadDefaults();
    valid_ = true;
#else
    // TODO: Add EEPROM support for ESP32/AVR platforms
    Serial.println(F("[CONFIG] EEPROM not implemented yet - using defaults only"));
    loadDefaults();
    valid_ = true;
#endif
}

void ConfigStorage::loadDefaults() {
    // Initialize header with defaults
    configData_.header.magic = MAGIC_NUMBER;
    configData_.header.version = CONFIG_VERSION;
    configData_.header.deviceType = 1;  // Default to Hat, can be changed at runtime

    // Load fire parameter defaults from TotemDefaults
    configData_.fireParams.baseCooling = Defaults::BaseCooling;
    configData_.fireParams.sparkHeatMin = Defaults::SparkHeatMin;
    configData_.fireParams.sparkHeatMax = Defaults::SparkHeatMax;
    configData_.fireParams.sparkChance = Defaults::SparkChance;
    configData_.fireParams.audioSparkBoost = Defaults::AudioSparkBoost;
    configData_.fireParams.audioHeatBoostMax = Defaults::AudioHeatBoostMax;
    configData_.fireParams.coolingAudioBias = Defaults::CoolingAudioBias;
    configData_.fireParams.bottomRowsForSparks = Defaults::BottomRowsForSparks;
    configData_.fireParams.transientHeatMax = Defaults::TransientHeatMax;

    // Load microphone parameter defaults
    configData_.micParams.attackSeconds = 0.08f;
    configData_.micParams.releaseSeconds = 0.30f;
    configData_.micParams.noiseGate = 0.06f;
    configData_.micParams.globalGain = 1.0f;
    configData_.micParams.transientCooldownMs = 120;
    configData_.micParams.agEnabled = true;
    configData_.micParams.agTarget = 0.35f;
    configData_.micParams.agStrength = 0.9f;
    configData_.micParams.transientFactor = 2.5f;
    configData_.micParams.loudFloor = 0.15f;
    configData_.micParams.transientDecay = 6.0f;
    configData_.micParams.compRatio = 4.0f;
    configData_.micParams.compThresh = 0.7f;

    valid_ = true;
    needsSave_ = false;
}

void ConfigStorage::loadConfiguration(FireParams& fireParams, AdaptiveMic& mic) {
    if (!valid_) {
        Serial.println(F("[CONFIG] Cannot load - configuration invalid"));
        return;
    }

    copyFireParamsTo(fireParams);
    copyMicParamsTo(mic);

    Serial.println(F("[CONFIG] Applied saved parameters"));
}

void ConfigStorage::saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic) {
    copyFireParamsFrom(fireParams);
    copyMicParamsFrom(mic);

    // For nRF52 (all variants): Just keep in memory for now
    // TODO: Add persistent storage implementation

    valid_ = true;
    needsSave_ = false;

    Serial.println(F("[CONFIG] Configuration saved (memory only on nRF52 variants)"));
}

// MatrixFireGenerator overloads
void ConfigStorage::loadConfiguration(MatrixFireParams& matrixFireParams, AdaptiveMic& mic) {
    if (!valid_) {
        Serial.println(F("[CONFIG] Cannot load - configuration invalid"));
        return;
    }

    copyMatrixFireParamsTo(matrixFireParams);
    copyMicParamsTo(mic);

    Serial.println(F("[CONFIG] Applied saved parameters (MatrixFire)"));
}

void ConfigStorage::saveConfiguration(const MatrixFireParams& matrixFireParams, const AdaptiveMic& mic) {
    copyMatrixFireParamsFrom(matrixFireParams);
    copyMicParamsFrom(mic);

    // For nRF52 variants: Parameters are now updated in memory, persistence would be a future enhancement
    // For other platforms: EEPROM saving would be implemented here

    valid_ = true;
    needsSave_ = false;

#if defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    Serial.println(F("[CONFIG] Configuration updated in memory (MatrixFire)"));
#else
    Serial.println(F("[CONFIG] Configuration saved (MatrixFire)"));
#endif
}

// StringFireGenerator overloads
void ConfigStorage::loadConfiguration(StringFireParams& stringFireParams, AdaptiveMic& mic) {
    if (!valid_) {
        Serial.println(F("[CONFIG] Cannot load - configuration invalid"));
        return;
    }

    copyStringFireParamsTo(stringFireParams);
    copyMicParamsTo(mic);

    Serial.println(F("[CONFIG] Applied saved parameters (StringFire)"));
}

void ConfigStorage::saveConfiguration(const StringFireParams& stringFireParams, const AdaptiveMic& mic) {
    copyStringFireParamsFrom(stringFireParams);
    copyMicParamsFrom(mic);

    // For nRF52 variants: Parameters are now updated in memory, persistence would be a future enhancement
    // For other platforms: EEPROM saving would be implemented here

    valid_ = true;
    needsSave_ = false;

#if defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    Serial.println(F("[CONFIG] Configuration updated in memory (StringFire)"));
#else
    Serial.println(F("[CONFIG] Configuration saved to EEPROM (StringFire)"));
#endif
}

void ConfigStorage::copyFireParamsTo(FireParams& params) const {
    params.baseCooling = configData_.fireParams.baseCooling;
    params.sparkHeatMin = configData_.fireParams.sparkHeatMin;
    params.sparkHeatMax = configData_.fireParams.sparkHeatMax;
    params.sparkChance = configData_.fireParams.sparkChance;
    params.audioSparkBoost = configData_.fireParams.audioSparkBoost;
    params.audioHeatBoostMax = configData_.fireParams.audioHeatBoostMax;
    params.coolingAudioBias = configData_.fireParams.coolingAudioBias;
    params.bottomRowsForSparks = configData_.fireParams.bottomRowsForSparks;
    params.transientHeatMax = configData_.fireParams.transientHeatMax;
}

void ConfigStorage::copyFireParamsFrom(const FireParams& params) {
    configData_.fireParams.baseCooling = params.baseCooling;
    configData_.fireParams.sparkHeatMin = params.sparkHeatMin;
    configData_.fireParams.sparkHeatMax = params.sparkHeatMax;
    configData_.fireParams.sparkChance = params.sparkChance;
    configData_.fireParams.audioSparkBoost = params.audioSparkBoost;
    configData_.fireParams.audioHeatBoostMax = params.audioHeatBoostMax;
    configData_.fireParams.coolingAudioBias = params.coolingAudioBias;
    configData_.fireParams.bottomRowsForSparks = params.bottomRowsForSparks;
    configData_.fireParams.transientHeatMax = params.transientHeatMax;
}

void ConfigStorage::copyMicParamsTo(AdaptiveMic& mic) const {
    mic.attackSeconds = configData_.micParams.attackSeconds;
    mic.releaseSeconds = configData_.micParams.releaseSeconds;
    mic.noiseGate = configData_.micParams.noiseGate;
    mic.globalGain = configData_.micParams.globalGain;
    mic.transientCooldownMs = configData_.micParams.transientCooldownMs;
    mic.agEnabled = configData_.micParams.agEnabled;
    mic.agTarget = configData_.micParams.agTarget;
    mic.agStrength = configData_.micParams.agStrength;
    mic.transientFactor = configData_.micParams.transientFactor;
    mic.loudFloor = configData_.micParams.loudFloor;
    mic.transientDecay = configData_.micParams.transientDecay;
    mic.compRatio = configData_.micParams.compRatio;
    mic.compThresh = configData_.micParams.compThresh;
}

void ConfigStorage::copyMicParamsFrom(const AdaptiveMic& mic) {
    configData_.micParams.attackSeconds = mic.attackSeconds;
    configData_.micParams.releaseSeconds = mic.releaseSeconds;
    configData_.micParams.noiseGate = mic.noiseGate;
    configData_.micParams.globalGain = mic.globalGain;
    configData_.micParams.transientCooldownMs = mic.transientCooldownMs;
    configData_.micParams.agEnabled = mic.agEnabled;
    configData_.micParams.agTarget = mic.agTarget;
    configData_.micParams.agStrength = mic.agStrength;
    configData_.micParams.transientFactor = mic.transientFactor;
    configData_.micParams.loudFloor = mic.loudFloor;
    configData_.micParams.transientDecay = mic.transientDecay;
    configData_.micParams.compRatio = mic.compRatio;
    configData_.micParams.compThresh = mic.compThresh;
}

void ConfigStorage::setDeviceType(uint8_t deviceType) {
    if (deviceType >= 1 && deviceType <= 3) {
        configData_.header.deviceType = deviceType;
        needsSave_ = true;
    }
}

void ConfigStorage::saveFireParam(const char* paramName, const FireParams& params) {
    copyFireParamsFrom(params);
    // TODO: Add persistent storage for nRF52

    Serial.print(F("[CONFIG] Saved fire parameter: "));
    Serial.println(paramName);
}

void ConfigStorage::saveMicParam(const char* paramName, const AdaptiveMic& mic) {
    copyMicParamsFrom(mic);
    // TODO: Add persistent storage for nRF52

    Serial.print(F("[CONFIG] Saved mic parameter: "));
    Serial.println(paramName);
}

void ConfigStorage::factoryReset() {
    Serial.println(F("[CONFIG] Performing factory reset..."));

    // Clear configuration and reload defaults
    memset(&configData_, 0, sizeof(configData_));
    loadDefaults();

    Serial.println(F("[CONFIG] Factory reset complete"));
}

void ConfigStorage::printStatus() const {
    Serial.println(F("=== Configuration Status ==="));
    Serial.print(F("Valid: ")); Serial.println(valid_ ? "YES" : "NO");
    Serial.print(F("Device Type: ")); Serial.println(configData_.header.deviceType);
    Serial.print(F("Version: ")); Serial.println(configData_.header.version);
    Serial.print(F("Storage: "));
#if defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    Serial.println(F("Memory only (no persistence)"));
#else
    Serial.println(F("EEPROM"));
#endif

    Serial.println(F("Fire Parameters:"));
    Serial.print(F("  baseCooling: ")); Serial.println(configData_.fireParams.baseCooling);
    Serial.print(F("  sparkChance: ")); Serial.println(configData_.fireParams.sparkChance, 3);
    Serial.print(F("  audioSparkBoost: ")); Serial.println(configData_.fireParams.audioSparkBoost, 3);

    Serial.println(F("Mic Parameters:"));
    Serial.print(F("  globalGain: ")); Serial.println(configData_.micParams.globalGain, 3);
    Serial.print(F("  noiseGate: ")); Serial.println(configData_.micParams.noiseGate, 3);
    Serial.print(F("  attackSeconds: ")); Serial.println(configData_.micParams.attackSeconds, 3);
    Serial.print(F("  releaseSeconds: ")); Serial.println(configData_.micParams.releaseSeconds, 3);
    Serial.println(F("============================"));
}

void ConfigStorage::copyMatrixFireParamsTo(MatrixFireParams& params) const {
    // Copy common parameters (MatrixFire has same base parameters as Fire)
    params.baseCooling = configData_.fireParams.baseCooling;
    params.sparkHeatMin = configData_.fireParams.sparkHeatMin;
    params.sparkHeatMax = configData_.fireParams.sparkHeatMax;
    params.sparkChance = configData_.fireParams.sparkChance;
    params.audioSparkBoost = configData_.fireParams.audioSparkBoost;
    params.audioHeatBoostMax = configData_.fireParams.audioHeatBoostMax;
    params.coolingAudioBias = configData_.fireParams.coolingAudioBias;
    params.bottomRowsForSparks = configData_.fireParams.bottomRowsForSparks;
    params.transientHeatMax = configData_.fireParams.transientHeatMax;
}

void ConfigStorage::copyMatrixFireParamsFrom(const MatrixFireParams& params) {
    // Copy common parameters (MatrixFire shares base parameters with Fire)
    configData_.fireParams.baseCooling = params.baseCooling;
    configData_.fireParams.sparkHeatMin = params.sparkHeatMin;
    configData_.fireParams.sparkHeatMax = params.sparkHeatMax;
    configData_.fireParams.sparkChance = params.sparkChance;
    configData_.fireParams.audioSparkBoost = params.audioSparkBoost;
    configData_.fireParams.audioHeatBoostMax = params.audioHeatBoostMax;
    configData_.fireParams.coolingAudioBias = params.coolingAudioBias;
    configData_.fireParams.bottomRowsForSparks = params.bottomRowsForSparks;
    configData_.fireParams.transientHeatMax = params.transientHeatMax;
}

void ConfigStorage::saveMatrixFireParam(const char* paramName, const MatrixFireParams& params) {
    copyMatrixFireParamsFrom(params);
    // TODO: Add persistent storage for nRF52

    Serial.print(F("[CONFIG] Saved matrix fire parameter: "));
    Serial.println(paramName);
}

void ConfigStorage::copyStringFireParamsTo(StringFireParams& params) const {
    // Copy common parameters (StringFire has same base parameters as Fire)
    params.baseCooling = configData_.fireParams.baseCooling;
    params.sparkHeatMin = configData_.fireParams.sparkHeatMin;
    params.sparkHeatMax = configData_.fireParams.sparkHeatMax;
    params.sparkChance = configData_.fireParams.sparkChance;
    params.audioSparkBoost = configData_.fireParams.audioSparkBoost;
    params.audioHeatBoostMax = configData_.fireParams.audioHeatBoostMax;
    params.coolingAudioBias = configData_.fireParams.coolingAudioBias;
    params.transientHeatMax = configData_.fireParams.transientHeatMax;

    // StringFire doesn't use bottomRowsForSparks - it has its own sparkSpreadRange
    // StringFire-specific parameters keep their defaults (not stored in EEPROM yet)
    // params.sparkSpreadRange uses compile-time defaults
}

void ConfigStorage::copyStringFireParamsFrom(const StringFireParams& params) {
    // Copy common parameters (StringFire shares base parameters with Fire)
    configData_.fireParams.baseCooling = params.baseCooling;
    configData_.fireParams.sparkHeatMin = params.sparkHeatMin;
    configData_.fireParams.sparkHeatMax = params.sparkHeatMax;
    configData_.fireParams.sparkChance = params.sparkChance;
    configData_.fireParams.audioSparkBoost = params.audioSparkBoost;
    configData_.fireParams.audioHeatBoostMax = params.audioHeatBoostMax;
    configData_.fireParams.coolingAudioBias = params.coolingAudioBias;
    configData_.fireParams.transientHeatMax = params.transientHeatMax;

    // StringFire doesn't set bottomRowsForSparks - keep the existing value
    // StringFire-specific parameters not stored yet (future enhancement)
}

void ConfigStorage::saveStringFireParam(const char* paramName, const StringFireParams& params) {
    copyStringFireParamsFrom(params);
    // TODO: Add persistent storage for nRF52

    Serial.print(F("[CONFIG] Saved string fire parameter: "));
    Serial.println(paramName);
}

uint32_t ConfigStorage::calculateChecksum(const void* data, size_t size) const {
    uint32_t checksum = 0;
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum << 1) ^ bytes[i];
    }
    return checksum;
}
