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

void ConfigStorage::loadDeviceDefaults() {
    // Device config defaults - UNCONFIGURED state
    memset(&data_.device, 0, sizeof(StoredDeviceConfig));
    data_.device.isValid = false;  // No device configured - triggers safe mode
    strncpy(data_.device.deviceName, "UNCONFIGURED", sizeof(data_.device.deviceName) - 1);
    strncpy(data_.device.deviceId, "none", sizeof(data_.device.deviceId) - 1);
}

void ConfigStorage::loadSettingsDefaults() {
    // Settings defaults - called when SETTINGS_VERSION changes
    // Device config is preserved separately

    // Fire defaults (particle-based) - matrix-appropriate defaults Feb 2026
    // These allow wind turbulence to be visibly effective (discrete sparks, fast heat decay)
    data_.fire.baseSpawnChance = 0.5f;      // Continuous sparks for constant fire
    data_.fire.audioSpawnBoost = 1.5f;      // Strong audio response
    data_.fire.gravity = 0.0f;              // No gravity (thermal force provides upward push)
    data_.fire.windBase = 0.0f;
    data_.fire.windVariation = 25.0f;       // Turbulence as LEDs/sec advection (visible swirl)
    data_.fire.drag = 0.985f;               // Smoother flow
    data_.fire.sparkVelocityMin = 5.0f;     // Slower sparks = more time in frame = wind has more effect
    data_.fire.sparkVelocityMax = 10.0f;    // Varied speeds
    data_.fire.sparkSpread = 4.0f;          // Good spread
    data_.fire.musicSpawnPulse = 0.95f;     // Tight beat sync
    data_.fire.organicTransientMin = 0.25f; // Responsive to softer transients
    data_.fire.backgroundIntensity = 0.15f; // Subtle noise background
    data_.fire.fastSparkRatio = 0.7f;       // 70% fast sparks, 30% embers
    data_.fire.thermalForce = 30.0f;        // Thermal buoyancy strength (LEDs/sec^2)
    data_.fire.maxParticles = 48;
    data_.fire.defaultLifespan = 170;       // 1.7 seconds (170 centiseconds)
    data_.fire.intensityMin = 150;
    data_.fire.intensityMax = 220;
    data_.fire.burstSparks = 10;            // Moderate transient bursts

    // Water defaults (particle-based)
    data_.water.baseSpawnChance = 0.25f;
    data_.water.audioSpawnBoost = 0.4f;
    data_.water.gravity = 5.0f;
    data_.water.windBase = 0.0f;
    data_.water.windVariation = 0.3f;
    data_.water.drag = 0.99f;
    data_.water.dropVelocityMin = 0.5f;
    data_.water.dropVelocityMax = 1.5f;
    data_.water.dropSpread = 0.3f;
    data_.water.splashVelocityMin = 0.5f;
    data_.water.splashVelocityMax = 2.0f;
    data_.water.musicSpawnPulse = 0.5f;
    data_.water.organicTransientMin = 0.3f;
    data_.water.backgroundIntensity = 0.15f;
    data_.water.maxParticles = 64;
    data_.water.defaultLifespan = 90;
    data_.water.intensityMin = 80;
    data_.water.intensityMax = 200;
    data_.water.splashParticles = 6;
    data_.water.splashIntensity = 120;

    // Lightning defaults (particle-based)
    data_.lightning.baseSpawnChance = 0.15f;
    data_.lightning.audioSpawnBoost = 0.5f;
    data_.lightning.boltVelocityMin = 4.0f;
    data_.lightning.boltVelocityMax = 8.0f;
    data_.lightning.branchAngleSpread = PI / 4.0f;  // 45 degree spread
    data_.lightning.musicSpawnPulse = 0.6f;
    data_.lightning.organicTransientMin = 0.3f;
    data_.lightning.backgroundIntensity = 0.15f;
    data_.lightning.maxParticles = 32;
    data_.lightning.defaultLifespan = 20;
    data_.lightning.intensityMin = 180;
    data_.lightning.intensityMax = 255;
    data_.lightning.fadeRate = 160;
    data_.lightning.branchChance = 30;
    data_.lightning.branchCount = 2;
    data_.lightning.branchIntensityLoss = 40;

    // Mic defaults (hardware-primary, window/range normalization)
    // Window/Range normalization parameters
    data_.mic.peakTau = 1.0f;        // 1s peak adaptation (fast response)
    data_.mic.releaseTau = 3.0f;     // 3s peak release (quick recovery)
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
    data_.music.phaseAdaptRate = 0.7f;   // Fast phase adaptation for tight beat sync

    // Tempo prior (CRITICAL: must be enabled for correct BPM tracking)
    data_.music.tempoPriorEnabled = true;    // MUST be true
    data_.music.tempoPriorCenter = 120.0f;   // Typical music tempo
    data_.music.tempoPriorWidth = 50.0f;     // Balanced width
    data_.music.tempoPriorStrength = 0.5f;   // 50% blend

    // Pulse modulation
    data_.music.pulseBoostOnBeat = 1.3f;
    data_.music.pulseSuppressOffBeat = 0.6f;
    data_.music.energyBoostOnBeat = 0.3f;

    // Stability and smoothing
    data_.music.stabilityWindowBeats = 8.0f;
    data_.music.beatLookaheadMs = 120.0f;  // Predict beats 120ms ahead to reduce perceived latency
    data_.music.tempoSmoothingFactor = 0.85f;
    data_.music.tempoChangeThreshold = 0.1f;

    // Transient-based phase correction (PLL) - calibrated Feb 2026
    data_.music.transientCorrectionRate = 0.15f;  // How fast to nudge phase toward transients
    data_.music.transientCorrectionMin = 0.42f;   // Min transient strength to trigger correction

    data_.brightness = 100;
}

void ConfigStorage::loadDefaults() {
    data_.magic = MAGIC_NUMBER;
    data_.deviceVersion = DEVICE_VERSION;
    data_.settingsVersion = SETTINGS_VERSION;

    loadDeviceDefaults();
    loadSettingsDefaults();
}

bool ConfigStorage::loadFromFlash() {
    // Zero-initialize so unread bytes (when file is smaller than current struct)
    // are deterministic rather than garbage stack values.
    ConfigData temp;
    memset(&temp, 0, sizeof(temp));

    // Minimum bytes required to read magic + both version fields + device config.
    // Device config lives immediately after the 4-byte header and must be fully
    // present for recovery to make sense.
    // cppcheck-suppress unreadVariable
    static const uint32_t MIN_DEVICE_BYTES =
        sizeof(uint16_t) +              // magic
        sizeof(uint8_t) +               // deviceVersion
        sizeof(uint8_t) +               // settingsVersion
        sizeof(StoredDeviceConfig);     // device config block

    uint32_t bytesRead = 0;

#if defined(ARDUINO_ARCH_MBED) || defined(TARGET_NAME) || defined(MBED_CONF_TARGET_NAME)
    if (!flashOk) return false;
    if (flash.read(&temp, flashAddr, sizeof(ConfigData)) != 0) return false;
    bytesRead = sizeof(ConfigData);  // FlashIAP reads exactly what is asked
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
    if (!flashOk || configFile == nullptr) return false;

    // Open config file for reading
    configFile->open(CONFIG_FILENAME, FILE_O_READ);
    if (!(*configFile)) return false;

    // Read however many bytes are stored (may be less than sizeof(ConfigData)
    // when the struct grew due to a settings version bump).
    bytesRead = configFile->read((uint8_t*)&temp, sizeof(ConfigData));
    configFile->close();

    // Nothing useful was read
    if (bytesRead < MIN_DEVICE_BYTES) return false;
#endif

    // Magic number mismatch: complete corruption, reset everything
    if (temp.magic != MAGIC_NUMBER) return false;

    // Start fresh with current defaults for both sections
    data_.magic = MAGIC_NUMBER;
    data_.deviceVersion = DEVICE_VERSION;
    data_.settingsVersion = SETTINGS_VERSION;

    // Handle device config version.
    // Device config bytes are always present as long as bytesRead >= MIN_DEVICE_BYTES
    // (checked above), so we only gate on version match, not on total file size.
    if (temp.deviceVersion == DEVICE_VERSION) {
        // Device config version matches - preserve it
        memcpy(&data_.device, &temp.device, sizeof(StoredDeviceConfig));
        SerialConsole::logDebug(F("Device config loaded from flash"));
    } else {
        // Device config version mismatch - use defaults (rare)
        loadDeviceDefaults();
        SerialConsole::logWarn(F("Device config version mismatch, using defaults"));
    }

    // Handle settings version.
    // Settings are only valid if both the version matches AND the file was large
    // enough to contain the full settings structs (i.e. not written by an older
    // firmware with a smaller ConfigData).
    // cppcheck-suppress unsignedLessThanZero
    if (temp.settingsVersion == SETTINGS_VERSION && bytesRead >= sizeof(ConfigData)) {
        // Settings version matches and file is the right size - preserve all settings
        memcpy(&data_.fire, &temp.fire, sizeof(StoredFireParams));
        memcpy(&data_.water, &temp.water, sizeof(StoredWaterParams));
        memcpy(&data_.lightning, &temp.lightning, sizeof(StoredLightningParams));
        memcpy(&data_.mic, &temp.mic, sizeof(StoredMicParams));
        memcpy(&data_.music, &temp.music, sizeof(StoredMusicParams));
        data_.brightness = temp.brightness;
        SerialConsole::logDebug(F("Settings loaded from flash"));
    } else {
        // Settings version mismatch or struct grew - use defaults.
        // Device config was already recovered above.
        loadSettingsDefaults();
        SerialConsole::logWarn(F("Settings version mismatch, using defaults (device config preserved)"));
    }

    return true;
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
    data_.deviceVersion = DEVICE_VERSION;
    data_.settingsVersion = SETTINGS_VERSION;

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
    data_.deviceVersion = DEVICE_VERSION;
    data_.settingsVersion = SETTINGS_VERSION;

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

void ConfigStorage::loadConfiguration(FireParams& fireParams, WaterParams& waterParams, LightningParams& lightningParams,
                                      AdaptiveMic& mic, AudioController* audioCtrl) {
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
    validateFloat(data_.fire.baseSpawnChance, 0.0f, 1.0f, F("baseSpawnChance"));
    validateFloat(data_.fire.audioSpawnBoost, 0.0f, 2.0f, F("audioSpawnBoost"));

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

    // Tempo prior validation (v25+)
    validateFloat(data_.music.tempoPriorCenter, 60.0f, 200.0f, F("priorcenter"));
    validateFloat(data_.music.tempoPriorWidth, 10.0f, 100.0f, F("priorwidth"));
    validateFloat(data_.music.tempoPriorStrength, 0.0f, 1.0f, F("priorstrength"));

    // Pulse modulation validation (v25+)
    validateFloat(data_.music.pulseBoostOnBeat, 1.0f, 3.0f, F("pulseboost"));
    validateFloat(data_.music.pulseSuppressOffBeat, 0.1f, 1.0f, F("pulsesuppress"));
    validateFloat(data_.music.energyBoostOnBeat, 0.0f, 1.0f, F("energyboost"));

    // Stability and smoothing validation (v25+)
    validateFloat(data_.music.stabilityWindowBeats, 2.0f, 32.0f, F("stabilitywin"));
    validateFloat(data_.music.beatLookaheadMs, 0.0f, 200.0f, F("lookahead"));
    validateFloat(data_.music.tempoSmoothingFactor, 0.5f, 0.99f, F("temposmooth"));
    validateFloat(data_.music.tempoChangeThreshold, 0.01f, 0.5f, F("tempochgthresh"));

    // Transient-based phase correction validation (v26+)
    validateFloat(data_.music.transientCorrectionRate, 0.0f, 1.0f, F("transcorrrate"));
    validateFloat(data_.music.transientCorrectionMin, 0.0f, 1.0f, F("transcorrmin"));

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
        Serial.print(F("[DEBUG] baseSpawnChance=")); Serial.print(data_.fire.baseSpawnChance, 2);
        Serial.print(F(" gravity=")); Serial.println(data_.fire.gravity);
    }

    // Spawn behavior
    fireParams.baseSpawnChance = data_.fire.baseSpawnChance;
    fireParams.audioSpawnBoost = data_.fire.audioSpawnBoost;
    // Physics
    fireParams.gravity = data_.fire.gravity;
    fireParams.windBase = data_.fire.windBase;
    fireParams.windVariation = data_.fire.windVariation;
    fireParams.drag = data_.fire.drag;
    // Spark appearance
    fireParams.sparkVelocityMin = data_.fire.sparkVelocityMin;
    fireParams.sparkVelocityMax = data_.fire.sparkVelocityMax;
    fireParams.sparkSpread = data_.fire.sparkSpread;
    // Audio reactivity
    fireParams.musicSpawnPulse = data_.fire.musicSpawnPulse;
    fireParams.organicTransientMin = data_.fire.organicTransientMin;
    // Background
    fireParams.backgroundIntensity = data_.fire.backgroundIntensity;
    // Particle variety
    fireParams.fastSparkRatio = data_.fire.fastSparkRatio;
    fireParams.thermalForce = data_.fire.thermalForce;
    // Lifecycle
    fireParams.maxParticles = data_.fire.maxParticles;
    fireParams.defaultLifespan = data_.fire.defaultLifespan;
    fireParams.intensityMin = data_.fire.intensityMin;
    fireParams.intensityMax = data_.fire.intensityMax;
    fireParams.burstSparks = data_.fire.burstSparks;

    // === WATER PARAMETERS ===
    // Spawn behavior
    waterParams.baseSpawnChance = data_.water.baseSpawnChance;
    waterParams.audioSpawnBoost = data_.water.audioSpawnBoost;
    // Physics
    waterParams.gravity = data_.water.gravity;
    waterParams.windBase = data_.water.windBase;
    waterParams.windVariation = data_.water.windVariation;
    waterParams.drag = data_.water.drag;
    // Drop appearance
    waterParams.dropVelocityMin = data_.water.dropVelocityMin;
    waterParams.dropVelocityMax = data_.water.dropVelocityMax;
    waterParams.dropSpread = data_.water.dropSpread;
    // Splash behavior
    waterParams.splashVelocityMin = data_.water.splashVelocityMin;
    waterParams.splashVelocityMax = data_.water.splashVelocityMax;
    // Audio reactivity
    waterParams.musicSpawnPulse = data_.water.musicSpawnPulse;
    waterParams.organicTransientMin = data_.water.organicTransientMin;
    // Background
    waterParams.backgroundIntensity = data_.water.backgroundIntensity;
    // Lifecycle
    waterParams.defaultLifespan = data_.water.defaultLifespan;
    waterParams.intensityMin = data_.water.intensityMin;
    waterParams.intensityMax = data_.water.intensityMax;
    waterParams.splashParticles = data_.water.splashParticles;
    waterParams.splashIntensity = data_.water.splashIntensity;

    // === LIGHTNING PARAMETERS ===
    // Spawn behavior
    lightningParams.baseSpawnChance = data_.lightning.baseSpawnChance;
    lightningParams.audioSpawnBoost = data_.lightning.audioSpawnBoost;
    // Bolt appearance
    lightningParams.boltVelocityMin = data_.lightning.boltVelocityMin;
    lightningParams.boltVelocityMax = data_.lightning.boltVelocityMax;
    // Branching
    lightningParams.branchAngleSpread = data_.lightning.branchAngleSpread;
    // Audio reactivity
    lightningParams.musicSpawnPulse = data_.lightning.musicSpawnPulse;
    lightningParams.organicTransientMin = data_.lightning.organicTransientMin;
    // Background
    lightningParams.backgroundIntensity = data_.lightning.backgroundIntensity;
    // Lifecycle
    lightningParams.defaultLifespan = data_.lightning.defaultLifespan;
    lightningParams.intensityMin = data_.lightning.intensityMin;
    lightningParams.intensityMax = data_.lightning.intensityMax;
    lightningParams.fadeRate = data_.lightning.fadeRate;
    lightningParams.branchChance = data_.lightning.branchChance;
    lightningParams.branchCount = data_.lightning.branchCount;
    lightningParams.branchIntensityLoss = data_.lightning.branchIntensityLoss;

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
        // Basic rhythm parameters
        audioCtrl->bpmMin = data_.music.bpmMin;
        audioCtrl->bpmMax = data_.music.bpmMax;
        audioCtrl->activationThreshold = data_.music.activationThreshold;
        audioCtrl->phaseAdaptRate = data_.music.phaseAdaptRate;

        // Tempo prior (v25+) - CRITICAL for correct BPM tracking
        audioCtrl->tempoPriorEnabled = data_.music.tempoPriorEnabled;
        audioCtrl->tempoPriorCenter = data_.music.tempoPriorCenter;
        audioCtrl->tempoPriorWidth = data_.music.tempoPriorWidth;
        audioCtrl->tempoPriorStrength = data_.music.tempoPriorStrength;

        // Pulse modulation (v25+)
        audioCtrl->pulseBoostOnBeat = data_.music.pulseBoostOnBeat;
        audioCtrl->pulseSuppressOffBeat = data_.music.pulseSuppressOffBeat;
        audioCtrl->energyBoostOnBeat = data_.music.energyBoostOnBeat;

        // Stability and smoothing (v25+)
        audioCtrl->stabilityWindowBeats = data_.music.stabilityWindowBeats;
        audioCtrl->beatLookaheadMs = data_.music.beatLookaheadMs;
        audioCtrl->tempoSmoothingFactor = data_.music.tempoSmoothingFactor;
        audioCtrl->tempoChangeThreshold = data_.music.tempoChangeThreshold;

        // Transient-based phase correction (v26+)
        audioCtrl->transientCorrectionRate = data_.music.transientCorrectionRate;
        audioCtrl->transientCorrectionMin = data_.music.transientCorrectionMin;
    }
}

void ConfigStorage::saveConfiguration(const FireParams& fireParams, const WaterParams& waterParams, const LightningParams& lightningParams,
                                      const AdaptiveMic& mic, const AudioController* audioCtrl) {
    // Spawn behavior
    data_.fire.baseSpawnChance = fireParams.baseSpawnChance;
    data_.fire.audioSpawnBoost = fireParams.audioSpawnBoost;
    // Physics
    data_.fire.gravity = fireParams.gravity;
    data_.fire.windBase = fireParams.windBase;
    data_.fire.windVariation = fireParams.windVariation;
    data_.fire.drag = fireParams.drag;
    // Spark appearance
    data_.fire.sparkVelocityMin = fireParams.sparkVelocityMin;
    data_.fire.sparkVelocityMax = fireParams.sparkVelocityMax;
    data_.fire.sparkSpread = fireParams.sparkSpread;
    // Audio reactivity
    data_.fire.musicSpawnPulse = fireParams.musicSpawnPulse;
    data_.fire.organicTransientMin = fireParams.organicTransientMin;
    // Background
    data_.fire.backgroundIntensity = fireParams.backgroundIntensity;
    // Particle variety
    data_.fire.fastSparkRatio = fireParams.fastSparkRatio;
    data_.fire.thermalForce = fireParams.thermalForce;
    // Lifecycle
    data_.fire.maxParticles = fireParams.maxParticles;
    data_.fire.defaultLifespan = fireParams.defaultLifespan;
    data_.fire.intensityMin = fireParams.intensityMin;
    data_.fire.intensityMax = fireParams.intensityMax;
    data_.fire.burstSparks = fireParams.burstSparks;

    // === WATER PARAMETERS ===
    // Spawn behavior
    data_.water.baseSpawnChance = waterParams.baseSpawnChance;
    data_.water.audioSpawnBoost = waterParams.audioSpawnBoost;
    // Physics
    data_.water.gravity = waterParams.gravity;
    data_.water.windBase = waterParams.windBase;
    data_.water.windVariation = waterParams.windVariation;
    data_.water.drag = waterParams.drag;
    // Drop appearance
    data_.water.dropVelocityMin = waterParams.dropVelocityMin;
    data_.water.dropVelocityMax = waterParams.dropVelocityMax;
    data_.water.dropSpread = waterParams.dropSpread;
    // Splash behavior
    data_.water.splashVelocityMin = waterParams.splashVelocityMin;
    data_.water.splashVelocityMax = waterParams.splashVelocityMax;
    // Audio reactivity
    data_.water.musicSpawnPulse = waterParams.musicSpawnPulse;
    data_.water.organicTransientMin = waterParams.organicTransientMin;
    // Background
    data_.water.backgroundIntensity = waterParams.backgroundIntensity;
    // Lifecycle
    data_.water.defaultLifespan = waterParams.defaultLifespan;
    data_.water.intensityMin = waterParams.intensityMin;
    data_.water.intensityMax = waterParams.intensityMax;
    data_.water.splashParticles = waterParams.splashParticles;
    data_.water.splashIntensity = waterParams.splashIntensity;

    // === LIGHTNING PARAMETERS ===
    // Spawn behavior
    data_.lightning.baseSpawnChance = lightningParams.baseSpawnChance;
    data_.lightning.audioSpawnBoost = lightningParams.audioSpawnBoost;
    // Bolt appearance
    data_.lightning.boltVelocityMin = lightningParams.boltVelocityMin;
    data_.lightning.boltVelocityMax = lightningParams.boltVelocityMax;
    // Branching
    data_.lightning.branchAngleSpread = lightningParams.branchAngleSpread;
    // Audio reactivity
    data_.lightning.musicSpawnPulse = lightningParams.musicSpawnPulse;
    data_.lightning.organicTransientMin = lightningParams.organicTransientMin;
    // Background
    data_.lightning.backgroundIntensity = lightningParams.backgroundIntensity;
    // Lifecycle
    data_.lightning.defaultLifespan = lightningParams.defaultLifespan;
    data_.lightning.intensityMin = lightningParams.intensityMin;
    data_.lightning.intensityMax = lightningParams.intensityMax;
    data_.lightning.fadeRate = lightningParams.fadeRate;
    data_.lightning.branchChance = lightningParams.branchChance;
    data_.lightning.branchCount = lightningParams.branchCount;
    data_.lightning.branchIntensityLoss = lightningParams.branchIntensityLoss;

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
        // Basic rhythm parameters
        data_.music.bpmMin = audioCtrl->bpmMin;
        data_.music.bpmMax = audioCtrl->bpmMax;
        data_.music.activationThreshold = audioCtrl->activationThreshold;
        data_.music.phaseAdaptRate = audioCtrl->phaseAdaptRate;

        // Tempo prior (v25+) - CRITICAL for correct BPM tracking
        data_.music.tempoPriorEnabled = audioCtrl->tempoPriorEnabled;
        data_.music.tempoPriorCenter = audioCtrl->tempoPriorCenter;
        data_.music.tempoPriorWidth = audioCtrl->tempoPriorWidth;
        data_.music.tempoPriorStrength = audioCtrl->tempoPriorStrength;

        // Pulse modulation (v25+)
        data_.music.pulseBoostOnBeat = audioCtrl->pulseBoostOnBeat;
        data_.music.pulseSuppressOffBeat = audioCtrl->pulseSuppressOffBeat;
        data_.music.energyBoostOnBeat = audioCtrl->energyBoostOnBeat;

        // Stability and smoothing (v25+)
        data_.music.stabilityWindowBeats = audioCtrl->stabilityWindowBeats;
        data_.music.beatLookaheadMs = audioCtrl->beatLookaheadMs;
        data_.music.tempoSmoothingFactor = audioCtrl->tempoSmoothingFactor;
        data_.music.tempoChangeThreshold = audioCtrl->tempoChangeThreshold;

        // Transient-based phase correction (v26+)
        data_.music.transientCorrectionRate = audioCtrl->transientCorrectionRate;
        data_.music.transientCorrectionMin = audioCtrl->transientCorrectionMin;
    }

    saveToFlash();
    dirty_ = false;
    lastSaveMs_ = millis();
}

void ConfigStorage::saveIfDirty(const FireParams& fireParams, const WaterParams& waterParams, const LightningParams& lightningParams,
                                const AdaptiveMic& mic, const AudioController* audioCtrl) {
    if (dirty_ && (millis() - lastSaveMs_ > 5000)) {  // Debounce: save at most every 5 seconds
        saveConfiguration(fireParams, waterParams, lightningParams, mic, audioCtrl);
    }
}

void ConfigStorage::factoryReset() {
    SerialConsole::logInfo(F("Factory reset"));
    loadDefaults();
    saveToFlash();
}
