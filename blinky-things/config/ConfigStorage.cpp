#include "ConfigStorage.h"
#include "../tests/SafetyTest.h"

// Flash storage for nRF52 mbed core
#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
#include "FlashIAP.h"
static mbed::FlashIAP flash;
static bool flashOk = false;
static uint32_t flashAddr = 0;
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
    data_.fire.audioHeatBoostMax = 150;
    data_.fire.coolingAudioBias = -70;
    data_.fire.transientHeatMax = 200;
    data_.fire.spreadDistance = 3;
    data_.fire.heatDecay = 0.60f;
    data_.fire.suppressionMs = 300;

    // Mic defaults (hardware-primary AGC architecture)
    data_.mic.noiseGate = 0.04f;
    data_.mic.globalGain = 1.0f;     // Start at unity (hardware does primary gain)
    data_.mic.transientFactor = 1.5f;
    data_.mic.loudFloor = 0.05f;
    // Software AGC time constants (secondary - fine adjustments only)
    data_.mic.agcAttackTau = 0.1f;   // 100ms peak attack
    data_.mic.agcReleaseTau = 2.0f;  // 2s peak release
    data_.mic.agcGainTau = 5.0f;     // 5s gain adjustment
    // Hardware AGC parameters (primary - optimizes raw ADC input)
    data_.mic.hwTargetLow = 0.15f;   // Increase HW gain if raw < 15%
    data_.mic.hwTargetHigh = 0.35f;  // Decrease HW gain if raw > 35%
    data_.mic.hwTrackingTau = 10.0f; // 10s tracking of raw input
    // Timing parameters
    data_.mic.transientCooldownMs = 60;
    data_.mic.hwCalibPeriodMs = 30000;  // 30s between HW gain checks

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
    validateFloat(data_.mic.globalGain, 0.1f, 10.0f, F("globalGain"));

    // Validate software AGC time constants (secondary - fine adjustments)
    validateFloat(data_.mic.agcAttackTau, 0.01f, 5.0f, F("agcAttackTau"));
    validateFloat(data_.mic.agcReleaseTau, 0.1f, 10.0f, F("agcReleaseTau"));
    validateFloat(data_.mic.agcGainTau, 0.1f, 30.0f, F("agcGainTau"));

    // Validate hardware AGC parameters (primary - raw input tracking)
    validateFloat(data_.mic.hwTargetLow, 0.05f, 0.5f, F("hwTargetLow"));
    validateFloat(data_.mic.hwTargetHigh, 0.1f, 0.8f, F("hwTargetHigh"));
    validateFloat(data_.mic.hwTrackingTau, 1.0f, 60.0f, F("hwTrackingTau"));

    // Validate timing parameters (match SerialConsole ranges)
    validateUint32(data_.mic.transientCooldownMs, 10, 10000, F("transientCooldownMs"));
    validateUint32(data_.mic.hwCalibPeriodMs, 5000, 600000, F("hwCalibPeriodMs"));

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
    fireParams.audioHeatBoostMax = data_.fire.audioHeatBoostMax;
    fireParams.coolingAudioBias = data_.fire.coolingAudioBias;
    fireParams.transientHeatMax = data_.fire.transientHeatMax;
    fireParams.spreadDistance = data_.fire.spreadDistance;
    fireParams.heatDecay = data_.fire.heatDecay;
    fireParams.suppressionMs = data_.fire.suppressionMs;

    mic.noiseGate = data_.mic.noiseGate;
    mic.globalGain = data_.mic.globalGain;
    mic.transientFactor = data_.mic.transientFactor;
    mic.loudFloor = data_.mic.loudFloor;
    // Software AGC time constants (secondary - fine adjustments)
    mic.agcAttackTau = data_.mic.agcAttackTau;
    mic.agcReleaseTau = data_.mic.agcReleaseTau;
    mic.agcGainTau = data_.mic.agcGainTau;
    // Hardware AGC parameters (primary - raw input tracking)
    mic.hwTargetLow = data_.mic.hwTargetLow;
    mic.hwTargetHigh = data_.mic.hwTargetHigh;
    mic.hwTrackingTau = data_.mic.hwTrackingTau;
    // Timing parameters
    mic.transientCooldownMs = data_.mic.transientCooldownMs;
    mic.hwCalibPeriodMs = data_.mic.hwCalibPeriodMs;
}

void ConfigStorage::saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic) {
    data_.fire.baseCooling = fireParams.baseCooling;
    data_.fire.sparkHeatMin = fireParams.sparkHeatMin;
    data_.fire.sparkHeatMax = fireParams.sparkHeatMax;
    data_.fire.sparkChance = fireParams.sparkChance;
    data_.fire.audioSparkBoost = fireParams.audioSparkBoost;
    data_.fire.audioHeatBoostMax = fireParams.audioHeatBoostMax;
    data_.fire.coolingAudioBias = fireParams.coolingAudioBias;
    data_.fire.transientHeatMax = fireParams.transientHeatMax;
    data_.fire.spreadDistance = fireParams.spreadDistance;
    data_.fire.heatDecay = fireParams.heatDecay;
    data_.fire.suppressionMs = fireParams.suppressionMs;

    data_.mic.noiseGate = mic.noiseGate;
    data_.mic.globalGain = mic.globalGain;
    data_.mic.transientFactor = mic.transientFactor;
    data_.mic.loudFloor = mic.loudFloor;
    // Software AGC time constants (secondary - fine adjustments)
    data_.mic.agcAttackTau = mic.agcAttackTau;
    data_.mic.agcReleaseTau = mic.agcReleaseTau;
    data_.mic.agcGainTau = mic.agcGainTau;
    // Hardware AGC parameters (primary - raw input tracking)
    data_.mic.hwTargetLow = mic.hwTargetLow;
    data_.mic.hwTargetHigh = mic.hwTargetHigh;
    data_.mic.hwTrackingTau = mic.hwTrackingTau;
    // Timing parameters
    data_.mic.transientCooldownMs = mic.transientCooldownMs;
    data_.mic.hwCalibPeriodMs = mic.hwCalibPeriodMs;

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
