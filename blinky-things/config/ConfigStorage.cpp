#include "ConfigStorage.h"
#include "../tests/SafetyTest.h"
#include "../audio/AudioController.h"
#include "../inputs/SerialConsole.h"

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

        if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
            Serial.print(F("[DEBUG] Flash at 0x")); Serial.println(flashAddr, HEX);
            Serial.print(F("[DEBUG] ConfigData: ")); Serial.print(sizeof(ConfigData));
            Serial.print(F("B (MicParams: ")); Serial.print(sizeof(StoredMicParams));
            Serial.println(F("B)"));
        }

        // CRITICAL: Validate flash address before ANY operations
        // This prevents bootloader corruption
        if (!SafetyTest::isFlashAddressSafe(flashAddr, 4096)) {
            SerialConsole::logError(F("UNSAFE FLASH ADDRESS - operations disabled"));
            flashOk = false;  // Disable all flash operations
        } else {
            SerialConsole::logDebug(F("Flash address validated"));

            if (loadFromFlash()) {
                SerialConsole::logDebug(F("Config loaded from flash"));
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

    if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
        Serial.print(F("[DEBUG] ConfigData: ")); Serial.print(sizeof(ConfigData));
        Serial.print(F("B (MicParams: ")); Serial.print(sizeof(StoredMicParams));
        Serial.println(F("B)"));
    }

    if (loadFromFlash()) {
        SerialConsole::logDebug(F("Config loaded from flash"));
        valid_ = true;
        return;
    }
#endif
    SerialConsole::logDebug(F("Using default config"));
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

    // Fast AGC parameters (accelerates calibration when signal is persistently low)
    data_.mic.fastAgcEnabled = true;        // Enable fast AGC when gain is high
    data_.mic.fastAgcThreshold = 0.15f;     // Raw level threshold to trigger fast mode
    data_.mic.fastAgcPeriodMs = 5000;       // 5s calibration period in fast mode
    data_.mic.fastAgcTrackingTau = 5.0f;    // 5s tracking tau in fast mode

    // LEGACY: Detection defaults (kept for backward compatibility with old configs)
    // These parameters are now handled by EnsembleDetector, not AdaptiveMic
    data_.mic.transientThreshold = 2.813f;
    data_.mic.attackMultiplier = 1.1f;
    data_.mic.averageTau = 0.8f;
    data_.mic.cooldownMs = 80;
    data_.mic.detectionMode = 4;
    data_.mic.bassFreq = 120.0f;
    data_.mic.bassQ = 1.0f;
    data_.mic.bassThresh = 3.0f;
    data_.mic.hfcWeight = 1.0f;
    data_.mic.hfcThresh = 3.0f;
    data_.mic.fluxThresh = 1.4f;
    data_.mic.fluxBins = 64;
    data_.mic.hybridFluxWeight = 0.5f;
    data_.mic.hybridDrumWeight = 0.5f;
    data_.mic.hybridBothBoost = 1.2f;

    // AudioController rhythm tracking defaults
    data_.music.activationThreshold = 0.4f;
    data_.music.bpmMin = 60.0f;
    data_.music.bpmMax = 200.0f;
    data_.music.phaseAdaptRate = 0.15f;

    data_.brightness = 100;
}

bool ConfigStorage::loadFromFlash() {
#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    if (!flashOk) return false;

    ConfigData temp;
    if (flash.read(&temp, flashAddr, sizeof(ConfigData)) != 0) return false;
    if (temp.magic != MAGIC_NUMBER) return false;
    // Version mismatch: intentionally discard old config and use defaults
    // See ConfigStorage.h for migration policy rationale
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
    // Version mismatch: intentionally discard old config and use defaults
    // See ConfigStorage.h for migration policy rationale
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
        SerialConsole::logWarn(F("Flash not available"));
        return;
    }

    // CRITICAL: Double-check flash address safety before EVERY write
    // This is the last line of defense against bootloader corruption
    uint32_t sectorSize = flash.get_sector_size(flashAddr);
    SafetyTest::assertFlashSafe(flashAddr, sectorSize);

    data_.magic = MAGIC_NUMBER;
    data_.version = CONFIG_VERSION;

    if (flash.erase(flashAddr, sectorSize) != 0) {
        SerialConsole::logError(F("Flash erase failed"));
        return;
    }

    if (flash.program(&data_, flashAddr, sizeof(ConfigData)) != 0) {
        SerialConsole::logError(F("Flash write failed"));
        return;
    }

    SerialConsole::logDebug(F("Config saved to flash"));
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
    if (!flashOk || configFile == nullptr) {
        SerialConsole::logWarn(F("Flash not available"));
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
        SerialConsole::logError(F("Failed to open config file"));
        return;
    }

    uint32_t bytesWritten = configFile->write((const uint8_t*)&data_, sizeof(ConfigData));
    configFile->close();

    if (bytesWritten != sizeof(ConfigData)) {
        SerialConsole::logError(F("Config write failed"));
        return;
    }

    SerialConsole::logDebug(F("Config saved to flash"));
#else
    SerialConsole::logWarn(F("No flash on this platform"));
#endif
}

void ConfigStorage::loadConfiguration(FireParams& fireParams, AdaptiveMic& mic, AudioController* audioCtrl) {
    // Validation helpers to reduce code duplication
    bool corrupt = false;

    auto validateFloat = [&](float value, float min, float max, const __FlashStringHelper* name) {
        if (value < min || value > max) {
            if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
                Serial.print(F("[WARN] Bad config "));
                Serial.print(name);
                Serial.print(F(": "));
                Serial.println(value);
            }
            corrupt = true;
        }
    };

    auto validateUint32 = [&](uint32_t value, uint32_t min, uint32_t max, const __FlashStringHelper* name) {
        if (value < min || value > max) {
            if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
                Serial.print(F("[WARN] Bad config "));
                Serial.print(name);
                Serial.print(F(": "));
                Serial.println(value);
            }
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

    // Validate fast AGC parameters
    validateFloat(data_.mic.fastAgcThreshold, 0.01f, 0.5f, F("fastAgcThresh"));
    validateFloat(data_.mic.fastAgcTrackingTau, 0.5f, 30.0f, F("fastAgcTau"));
    validateUint32(data_.mic.fastAgcPeriodMs, 500, 30000, F("fastAgcPeriod"));

    // LEGACY: Validate detection parameters (still needed for backward compatibility)
    validateFloat(data_.mic.transientThreshold, 1.5f, 10.0f, F("transientThreshold"));
    validateFloat(data_.mic.attackMultiplier, 1.1f, 2.0f, F("attackMultiplier"));
    validateFloat(data_.mic.averageTau, 0.1f, 5.0f, F("averageTau"));
    validateUint32(data_.mic.cooldownMs, 20, 500, F("cooldownMs"));
    validateUint32(data_.mic.detectionMode, 0, 4, F("detectionMode"));
    validateFloat(data_.mic.bassFreq, 40.0f, 200.0f, F("bassFreq"));
    validateFloat(data_.mic.bassQ, 0.5f, 3.0f, F("bassQ"));
    validateFloat(data_.mic.bassThresh, 1.5f, 10.0f, F("bassThresh"));
    validateFloat(data_.mic.hfcWeight, 0.5f, 5.0f, F("hfcWeight"));
    validateFloat(data_.mic.hfcThresh, 1.5f, 10.0f, F("hfcThresh"));
    validateFloat(data_.mic.fluxThresh, 1.0f, 10.0f, F("fluxThresh"));
    validateUint32(data_.mic.fluxBins, 4, 128, F("fluxBins"));
    validateFloat(data_.mic.hybridFluxWeight, 0.1f, 1.0f, F("hybridFluxWeight"));
    validateFloat(data_.mic.hybridDrumWeight, 0.1f, 1.0f, F("hybridDrumWeight"));
    validateFloat(data_.mic.hybridBothBoost, 1.0f, 2.0f, F("hybridBothBoost"));

    // AudioController validation (v23+)
    validateFloat(data_.music.activationThreshold, 0.0f, 1.0f, F("musicThresh"));
    validateFloat(data_.music.bpmMin, 40.0f, 120.0f, F("bpmMin"));
    validateFloat(data_.music.bpmMax, 120.0f, 240.0f, F("bpmMax"));
    validateFloat(data_.music.phaseAdaptRate, 0.01f, 1.0f, F("phaseAdaptRate"));

    // Validate BPM range consistency
    if (data_.music.bpmMin >= data_.music.bpmMax) {
        SerialConsole::logWarn(F("Invalid BPM range, using defaults"));
        data_.music.bpmMin = 60.0f;
        data_.music.bpmMax = 200.0f;
        corrupt = true;
    }

    if (corrupt) {
        SerialConsole::logWarn(F("Corrupt config detected, using defaults"));
        loadDefaults();
    }

    // Debug: show loaded values
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
        Serial.print(F("[DEBUG] heatDecay=")); Serial.print(data_.fire.heatDecay, 2);
        Serial.print(F(" cooling=")); Serial.print(data_.fire.baseCooling);
        Serial.print(F(" spread=")); Serial.println(data_.fire.spreadDistance);
    }

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

    // Fast AGC parameters
    mic.fastAgcEnabled = data_.mic.fastAgcEnabled;
    mic.fastAgcThreshold = data_.mic.fastAgcThreshold;
    mic.fastAgcPeriodMs = data_.mic.fastAgcPeriodMs;
    mic.fastAgcTrackingTau = data_.mic.fastAgcTrackingTau;

    // NOTE: Detection-specific parameters (transientThreshold, attackMultiplier, etc.)
    // are now handled by EnsembleDetector. The data_.mic fields are kept for
    // backward compatibility when reading old config files, but are not applied
    // to AdaptiveMic which now only handles audio input normalization.

    // AudioController parameters (v23+)
    // All rhythm tracking params are now public tunable members
    if (audioCtrl) {
        audioCtrl->bpmMin = data_.music.bpmMin;
        audioCtrl->bpmMax = data_.music.bpmMax;
        audioCtrl->activationThreshold = data_.music.activationThreshold;
        audioCtrl->phaseAdaptRate = data_.music.phaseAdaptRate;
    }
}

void ConfigStorage::saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic, const AudioController* audioCtrl) {
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

    // Fast AGC parameters
    data_.mic.fastAgcEnabled = mic.fastAgcEnabled;
    data_.mic.fastAgcThreshold = mic.fastAgcThreshold;
    data_.mic.fastAgcPeriodMs = mic.fastAgcPeriodMs;
    data_.mic.fastAgcTrackingTau = mic.fastAgcTrackingTau;

    // NOTE: Detection-specific parameters (transientThreshold, detectionMode, etc.)
    // are now handled by EnsembleDetector. The data_.mic fields are kept for
    // backward compatibility but are no longer saved from AdaptiveMic.
    // Future versions may save EnsembleDetector configuration separately.

    // AudioController parameters (v23+)
    // All rhythm tracking params are now public tunable members
    if (audioCtrl) {
        data_.music.bpmMin = audioCtrl->bpmMin;
        data_.music.bpmMax = audioCtrl->bpmMax;
        data_.music.activationThreshold = audioCtrl->activationThreshold;
        data_.music.phaseAdaptRate = audioCtrl->phaseAdaptRate;
    }

    saveToFlash();
    dirty_ = false;
    lastSaveMs_ = millis();
}

void ConfigStorage::saveIfDirty(const FireParams& fireParams, const AdaptiveMic& mic, const AudioController* audioCtrl) {
    if (dirty_ && (millis() - lastSaveMs_ > 5000)) {  // Debounce: save at most every 5 seconds
        saveConfiguration(fireParams, mic, audioCtrl);
    }
}

void ConfigStorage::factoryReset() {
    SerialConsole::logInfo(F("Factory reset"));
    loadDefaults();
    saveToFlash();
}
