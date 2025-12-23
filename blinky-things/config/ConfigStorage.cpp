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

    // Mic defaults
    data_.mic.noiseGate = 0.04f;
    data_.mic.globalGain = 3.0f;
    data_.mic.agTarget = 0.50f;
    data_.mic.agStrength = 0.25f;
    data_.mic.agMin = 1.0f;
    data_.mic.agMax = 12.0f;
    data_.mic.transientFactor = 1.5f;
    data_.mic.loudFloor = 0.05f;
    // New AGC time constants
    data_.mic.agcTauSeconds = 7.0f;
    data_.mic.agcAttackTau = 2.0f;
    data_.mic.agcReleaseTau = 10.0f;
    // Timing parameters
    data_.mic.transientCooldownMs = 60;
    data_.mic.hwCalibPeriodMs = 180000;

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
    // Validate critical floats - if garbage, use defaults
    bool corrupt = false;
    if (data_.fire.heatDecay <= 0.0f || data_.fire.heatDecay > 1.0f) {
        Serial.print(F("[CONFIG] BAD heatDecay: ")); Serial.println(data_.fire.heatDecay);
        corrupt = true;
    }
    if (data_.fire.sparkChance < 0.0f || data_.fire.sparkChance > 1.0f) {
        Serial.print(F("[CONFIG] BAD sparkChance: ")); Serial.println(data_.fire.sparkChance);
        corrupt = true;
    }
    if (data_.mic.globalGain <= 0.0f || data_.mic.globalGain > 50.0f) {
        Serial.print(F("[CONFIG] BAD globalGain: ")); Serial.println(data_.mic.globalGain);
        corrupt = true;
    }

    // Validate new AGC time constants (Phase 1)
    if (data_.mic.agcTauSeconds <= 0.0f || data_.mic.agcTauSeconds > 100.0f) {
        Serial.print(F("[CONFIG] BAD agcTauSeconds: ")); Serial.println(data_.mic.agcTauSeconds);
        corrupt = true;
    }
    if (data_.mic.agcAttackTau <= 0.0f || data_.mic.agcAttackTau > 100.0f) {
        Serial.print(F("[CONFIG] BAD agcAttackTau: ")); Serial.println(data_.mic.agcAttackTau);
        corrupt = true;
    }
    if (data_.mic.agcReleaseTau <= 0.0f || data_.mic.agcReleaseTau > 100.0f) {
        Serial.print(F("[CONFIG] BAD agcReleaseTau: ")); Serial.println(data_.mic.agcReleaseTau);
        corrupt = true;
    }

    // Validate timing parameters
    if (data_.mic.transientCooldownMs < 10 || data_.mic.transientCooldownMs > 10000) {
        Serial.print(F("[CONFIG] BAD transientCooldownMs: ")); Serial.println(data_.mic.transientCooldownMs);
        corrupt = true;
    }
    if (data_.mic.hwCalibPeriodMs < 1000 || data_.mic.hwCalibPeriodMs > 3600000) {
        Serial.print(F("[CONFIG] BAD hwCalibPeriodMs: ")); Serial.println(data_.mic.hwCalibPeriodMs);
        corrupt = true;
    }

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
    mic.agTarget = data_.mic.agTarget;
    mic.agStrength = data_.mic.agStrength;
    mic.agMin = data_.mic.agMin;
    mic.agMax = data_.mic.agMax;
    mic.transientFactor = data_.mic.transientFactor;
    mic.loudFloor = data_.mic.loudFloor;
    // New AGC time constants
    mic.agcTauSeconds = data_.mic.agcTauSeconds;
    mic.agcAttackTau = data_.mic.agcAttackTau;
    mic.agcReleaseTau = data_.mic.agcReleaseTau;
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
    data_.mic.agTarget = mic.agTarget;
    data_.mic.agStrength = mic.agStrength;
    data_.mic.agMin = mic.agMin;
    data_.mic.agMax = mic.agMax;
    data_.mic.transientFactor = mic.transientFactor;
    data_.mic.loudFloor = mic.loudFloor;
    // New AGC time constants
    data_.mic.agcTauSeconds = mic.agcTauSeconds;
    data_.mic.agcAttackTau = mic.agcAttackTau;
    data_.mic.agcReleaseTau = mic.agcReleaseTau;
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
