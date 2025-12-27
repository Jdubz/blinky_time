#include "ConfigStorage.h"
#include "../tests/SafetyTest.h"

// Flash storage for nRF52 mbed core
#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
#include "FlashIAP.h"
static mbed::FlashIAP flash;
static bool flashOk = false;
static uint32_t flashAddr = 0;
// Flash storage for native nRF52 platform (Seeeduino:nrf52)
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
// CRITICAL: Use pointer to avoid Static Initialization Order Fiasco (SIOF)
// Static File object constructor could crash before main() if InternalFS not ready
static File* configFile = nullptr;
static bool flashOk = false;
static const char* CONFIG_FILENAME = "/config.bin";
#endif

ConfigStorage::ConfigStorage() : valid_(false), dirty_(false), lastSaveMs_(0) {
    memset(&data_, 0, sizeof(data_));
}

void ConfigStorage::begin() {
#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    if (flash.init() == 0) {
        flashOk = true;
        // Use last 4KB of flash
        flashAddr = flash.get_flash_start() + flash.get_flash_size() - 4096;
        Serial.print(F("[CONFIG] Flash at 0x")); Serial.println(flashAddr, HEX);

        // Runtime struct size validation (helps catch padding issues)
        Serial.print(F("[CONFIG] ConfigData size: ")); Serial.print(sizeof(ConfigData));
        Serial.print(F(" bytes (StoredMicParams: ")); Serial.print(sizeof(StoredMicParams));
        Serial.println(F(" bytes)"));

        // CRITICAL: Validate flash address before ANY operations
        // This prevents bootloader corruption
        if (!SafetyTest::isFlashAddressSafe(flashAddr, 4096)) {
            Serial.println(F("[CONFIG] !!! UNSAFE FLASH ADDRESS DETECTED !!!"));
            Serial.print(F("[CONFIG] Address 0x")); Serial.print(flashAddr, HEX);
            Serial.println(F(" is in protected region"));
            Serial.println(F("[CONFIG] Flash operations DISABLED for safety"));
            flashOk = false;  // Disable all flash operations
        } else {
            Serial.println(F("[CONFIG] Flash address validated OK"));

            if (loadFromFlash()) {
                Serial.println(F("[CONFIG] Loaded from flash"));
                valid_ = true;
                return;
            }
        }
    }
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
    // Initialize InternalFS (should already be initialized by core)
    InternalFS.begin();

    // CRITICAL: Initialize File pointer AFTER InternalFS is ready (prevents SIOF)
    if (configFile == nullptr) {
        configFile = new File(InternalFS);
    }
    flashOk = true;

    // Runtime struct size validation
    Serial.print(F("[CONFIG] ConfigData size: ")); Serial.print(sizeof(ConfigData));
    Serial.print(F(" bytes (StoredMicParams: ")); Serial.print(sizeof(StoredMicParams));
    Serial.println(F(" bytes)"));

    if (loadFromFlash()) {
        Serial.println(F("[CONFIG] Loaded from flash"));
        valid_ = true;
        return;
    }
#endif
    Serial.println(F("[CONFIG] Using defaults"));
    loadDefaults();
    valid_ = true;
}

void ConfigStorage::loadDefaults() {
    data_.magic = MAGIC_NUMBER;
    data_.version = CONFIG_VERSION;

    // Fire defaults
    data_.fire.baseCooling = 90;
    data_.fire.sparkHeatMin = 200;
    data_.fire.sparkHeatMax = 255;
    data_.fire.sparkChance = 0.08f;
    data_.fire.audioSparkBoost = 0.8f;
    data_.fire.coolingAudioBias = -70;
    data_.fire.spreadDistance = 3;
    data_.fire.heatDecay = 0.60f;
    data_.fire.suppressionMs = 300;
    data_.fire.emberNoiseSpeed = 0.00033f;
    data_.fire.emberHeatMax = 18;
    data_.fire.bottomRowsForSparks = 1;
    data_.fire.burstSparks = 8;

    // Mic defaults (hardware-primary, window/range normalization)
    // Window/Range normalization parameters
    data_.mic.peakTau = 2.0f;        // 2s peak adaptation
    data_.mic.releaseTau = 5.0f;     // 5s peak release
    // Hardware AGC parameters (primary - optimizes raw ADC input)
    data_.mic.hwTarget = 0.35f;      // Target raw input level (±0.01 dead zone)

    // Simplified transient detection defaults (v19+)
    // NOTE: Old onset detection configs (v18 and below) are incompatible with simplified detection.
    // Version check in loadFromFlash() will reject old configs, forcing these defaults on upgrade.
    data_.mic.transientThreshold = 3.0f;  // 3x louder than recent average
    data_.mic.attackMultiplier = 1.3f;    // 30% sudden rise required
    data_.mic.averageTau = 0.8f;          // Recent average tracking time
    data_.mic.cooldownMs = 80;            // 80ms cooldown between hits

    data_.brightness = 100;
}

bool ConfigStorage::loadFromFlash() {
#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    if (!flashOk) return false;

    ConfigData temp;
    if (flash.read(&temp, flashAddr, sizeof(ConfigData)) != 0) return false;
    if (temp.magic != MAGIC_NUMBER) return false;
    if (temp.version != CONFIG_VERSION) return false;

    memcpy(&data_, &temp, sizeof(ConfigData));
    return true;
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
    if (!flashOk || configFile == nullptr) return false;

    // Open config file for reading
    configFile->open(CONFIG_FILENAME, FILE_O_READ);
    if (!(*configFile)) return false;  // Check if file opened successfully

    ConfigData temp;
    uint32_t bytesRead = configFile->read((uint8_t*)&temp, sizeof(ConfigData));
    configFile->close();

    if (bytesRead != sizeof(ConfigData)) return false;
    if (temp.magic != MAGIC_NUMBER) return false;
    if (temp.version != CONFIG_VERSION) return false;

    memcpy(&data_, &temp, sizeof(ConfigData));
    return true;
#else
    return false;
#endif
}

void ConfigStorage::saveToFlash() {
#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    if (!flashOk) {
        Serial.println(F("[CONFIG] Flash not available"));
        return;
    }

    // CRITICAL: Double-check flash address safety before EVERY write
    // This is the last line of defense against bootloader corruption
    uint32_t sectorSize = flash.get_sector_size(flashAddr);
    SafetyTest::assertFlashSafe(flashAddr, sectorSize);

    data_.magic = MAGIC_NUMBER;
    data_.version = CONFIG_VERSION;

    if (flash.erase(flashAddr, sectorSize) != 0) {
        Serial.println(F("[CONFIG] Erase failed"));
        return;
    }

    if (flash.program(&data_, flashAddr, sizeof(ConfigData)) != 0) {
        Serial.println(F("[CONFIG] Write failed"));
        return;
    }

    Serial.println(F("[CONFIG] Saved to flash"));
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
    if (!flashOk || configFile == nullptr) {
        Serial.println(F("[CONFIG] Flash not available"));
        return;
    }

    data_.magic = MAGIC_NUMBER;
    data_.version = CONFIG_VERSION;

    // Delete existing file if present
    if (InternalFS.exists(CONFIG_FILENAME)) {
        InternalFS.remove(CONFIG_FILENAME);
    }

    // Write config to file
    configFile->open(CONFIG_FILENAME, FILE_O_WRITE);
    if (!(*configFile)) {
        Serial.println(F("[CONFIG] Failed to open file for writing"));
        return;
    }

    uint32_t bytesWritten = configFile->write((const uint8_t*)&data_, sizeof(ConfigData));
    configFile->close();

    if (bytesWritten != sizeof(ConfigData)) {
        Serial.println(F("[CONFIG] Write failed"));
        return;
    }

    Serial.println(F("[CONFIG] Saved to flash"));
#else
    Serial.println(F("[CONFIG] No flash on this platform"));
#endif
}

void ConfigStorage::loadConfiguration(FireParams& fireParams, AdaptiveMic& mic) {
    // Validation helpers to reduce code duplication
    bool corrupt = false;

    auto validateFloat = [&](float value, float min, float max, const __FlashStringHelper* name) {
        if (value < min || value > max) {
            Serial.print(F("[CONFIG] BAD "));
            Serial.print(name);
            Serial.print(F(": "));
            Serial.println(value);
            corrupt = true;
        }
    };

    auto validateUint32 = [&](uint32_t value, uint32_t min, uint32_t max, const __FlashStringHelper* name) {
        if (value < min || value > max) {
            Serial.print(F("[CONFIG] BAD "));
            Serial.print(name);
            Serial.print(F(": "));
            Serial.println(value);
            corrupt = true;
        }
    };

    // Validate critical parameters - if out of range, use defaults
    validateFloat(data_.fire.heatDecay, 0.0f, 1.0f, F("heatDecay"));
    validateFloat(data_.fire.sparkChance, 0.0f, 1.0f, F("sparkChance"));

    // Validate window/range normalization parameters
    validateFloat(data_.mic.peakTau, 0.5f, 10.0f, F("peakTau"));
    validateFloat(data_.mic.releaseTau, 1.0f, 30.0f, F("releaseTau"));

    // Validate hardware AGC parameters (expanded - allow full ADC range usage)
    validateFloat(data_.mic.hwTarget, 0.05f, 0.9f, F("hwTarget"));

    // Validate simplified transient detection parameters (v19+)
    validateFloat(data_.mic.transientThreshold, 1.5f, 10.0f, F("transientThreshold"));
    validateFloat(data_.mic.attackMultiplier, 1.1f, 2.0f, F("attackMultiplier"));
    validateFloat(data_.mic.averageTau, 0.1f, 5.0f, F("averageTau"));
    validateUint32(data_.mic.cooldownMs, 20, 500, F("cooldownMs")); // uint16_t safely converts to uint32_t

    if (corrupt) {
        Serial.println(F("[CONFIG] Corrupt data detected, using defaults"));
        loadDefaults();
    }

    // Debug: show loaded values
    Serial.print(F("[CONFIG] heatDecay=")); Serial.print(data_.fire.heatDecay, 2);
    Serial.print(F(" cooling=")); Serial.print(data_.fire.baseCooling);
    Serial.print(F(" spread=")); Serial.println(data_.fire.spreadDistance);

    fireParams.baseCooling = data_.fire.baseCooling;
    fireParams.sparkHeatMin = data_.fire.sparkHeatMin;
    fireParams.sparkHeatMax = data_.fire.sparkHeatMax;
    fireParams.sparkChance = data_.fire.sparkChance;
    fireParams.audioSparkBoost = data_.fire.audioSparkBoost;
    fireParams.coolingAudioBias = data_.fire.coolingAudioBias;
    fireParams.spreadDistance = data_.fire.spreadDistance;
    fireParams.heatDecay = data_.fire.heatDecay;
    fireParams.suppressionMs = data_.fire.suppressionMs;
    fireParams.emberNoiseSpeed = data_.fire.emberNoiseSpeed;
    fireParams.emberHeatMax = data_.fire.emberHeatMax;
    fireParams.bottomRowsForSparks = data_.fire.bottomRowsForSparks;
    fireParams.burstSparks = data_.fire.burstSparks;

    // Window/Range normalization parameters
    mic.peakTau = data_.mic.peakTau;
    mic.releaseTau = data_.mic.releaseTau;
    // Hardware AGC parameters (primary - raw input tracking)
    mic.hwTarget = data_.mic.hwTarget;

    // Simplified transient detection parameters
    mic.transientThreshold = data_.mic.transientThreshold;
    mic.attackMultiplier = data_.mic.attackMultiplier;
    mic.averageTau = data_.mic.averageTau;
    mic.cooldownMs = data_.mic.cooldownMs;
}

void ConfigStorage::saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic) {
    data_.fire.baseCooling = fireParams.baseCooling;
    data_.fire.sparkHeatMin = fireParams.sparkHeatMin;
    data_.fire.sparkHeatMax = fireParams.sparkHeatMax;
    data_.fire.sparkChance = fireParams.sparkChance;
    data_.fire.audioSparkBoost = fireParams.audioSparkBoost;
    data_.fire.coolingAudioBias = fireParams.coolingAudioBias;
    data_.fire.spreadDistance = fireParams.spreadDistance;
    data_.fire.heatDecay = fireParams.heatDecay;
    data_.fire.suppressionMs = fireParams.suppressionMs;
    data_.fire.emberNoiseSpeed = fireParams.emberNoiseSpeed;
    data_.fire.emberHeatMax = fireParams.emberHeatMax;
    data_.fire.bottomRowsForSparks = fireParams.bottomRowsForSparks;
    data_.fire.burstSparks = fireParams.burstSparks;

    // Window/Range normalization parameters
    data_.mic.peakTau = mic.peakTau;
    data_.mic.releaseTau = mic.releaseTau;
    // Hardware AGC parameters (primary - raw input tracking)
    data_.mic.hwTarget = mic.hwTarget;

    // Simplified transient detection parameters
    data_.mic.transientThreshold = mic.transientThreshold;
    data_.mic.attackMultiplier = mic.attackMultiplier;
    data_.mic.averageTau = mic.averageTau;
    data_.mic.cooldownMs = mic.cooldownMs;

    saveToFlash();
    dirty_ = false;
    lastSaveMs_ = millis();
}

void ConfigStorage::saveIfDirty(const FireParams& fireParams, const AdaptiveMic& mic) {
    if (dirty_ && (millis() - lastSaveMs_ > 5000)) {  // Debounce: save at most every 5 seconds
        saveConfiguration(fireParams, mic);
    }
}

void ConfigStorage::factoryReset() {
    Serial.println(F("[CONFIG] Factory reset"));
    loadDefaults();
    saveToFlash();
}
