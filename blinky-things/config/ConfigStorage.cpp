#include "ConfigStorage.h"
#include "../tests/SafetyTest.h"
#include "../inputs/SerialConsole.h"
#include "../audio/AudioTracker.h"
#include "../types/BlinkyAssert.h"
#include "../types/Version.h"

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
// Flash-freshness marker (OPEN_ISSUES §3.4 / [[project-stale-flash-state-after-upgrade]]).
// Records the FIRMWARE_BUILD value that last successfully booted on this
// device. A mismatch on boot signals "first boot of a new build" and
// triggers a LittleFS reformat-and-restore — clearing accumulated journal-
// region cruft from older builds before the firmware tries to USE that
// flash. Self-contained byte layout (uint32 LE) — no version/magic; the
// presence of the file is the marker, and a corrupted/short read falls
// through to "missing" → triggers the same reformat path.
static const char* FW_BUILD_MARKER_FILE = "/.fw_build";
// Flash storage for ESP32-S3 via NVS (Non-Volatile Storage) Preferences API
#elif defined(ESP32)
#include <Preferences.h>
static Preferences prefs;
static bool flashOk = false;
static const char* NVS_NAMESPACE = "blinky";
static const char* NVS_KEY       = "cfg";
#endif

ConfigStorage::ConfigStorage() : valid_(false), dirty_(false), lastSaveMs_(0) {
    memset(&data_, 0, sizeof(data_));
}

#if defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
// Read the firmware-freshness marker. Returns true if the marker file
// exists and contains a complete uint32_t; sets *outBuild on success.
// A missing/short/unreadable marker is "first boot ever" semantically,
// which triggers the same reformat path as a build-number mismatch.
static bool readFwBuildMarker(uint32_t* outBuild) {
    File f(InternalFS);
    if (!f.open(FW_BUILD_MARKER_FILE, FILE_O_READ)) return false;
    uint32_t value = 0;
    size_t n = f.read(reinterpret_cast<uint8_t*>(&value), sizeof(value));
    f.close();
    if (n != sizeof(value)) return false;
    *outBuild = value;
    return true;
}

// Write the firmware-freshness marker. The remove+open dance forces a
// fresh write (LittleFS appends otherwise, growing the journal). MUST be
// the LAST step of the format-and-restore sequence — if a power-cut
// interrupts before this write completes, the next boot finds the
// marker still missing (or stale) and retries the format, which is
// idempotent against the just-saved /config.bin.
static bool writeFwBuildMarker(uint32_t build) {
    InternalFS.remove(FW_BUILD_MARKER_FILE);
    File f(InternalFS);
    if (!f.open(FW_BUILD_MARKER_FILE, FILE_O_WRITE)) return false;
    size_t n = f.write(reinterpret_cast<const uint8_t*>(&build), sizeof(build));
    f.close();
    return n == sizeof(build);
}
#endif

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

    // ── Flash-freshness check (OPEN_ISSUES §3.4) ───────────────────────
    // Fresh chips upgraded across many firmware builds were crashing on
    // first configured boot due to accumulated LittleFS journal-region
    // cruft from older builds — the residual "non-app flash regions"
    // captured in [[project-stale-flash-state-after-upgrade]]. The
    // mitigation: on first boot of a new FIRMWARE_BUILD, snapshot the
    // existing /config.bin, reformat LittleFS to clear ALL accumulated
    // state, then re-write /config.bin so the operator-visible config
    // survives. Marker file is written LAST so a power-cut leaves the
    // next boot still in "fresh firmware" mode, retrying the reformat
    // (idempotent against the saved config).
    {
        uint32_t recordedBuild = 0;
        bool haveMarker = readFwBuildMarker(&recordedBuild);
        if (!haveMarker || recordedBuild != FIRMWARE_BUILD) {
            Serial.print(F("[FRESH-BUILD] firmware build "));
            Serial.print(FIRMWARE_BUILD);
            Serial.print(F(" first boot (prev marker="));
            if (haveMarker) Serial.print(recordedBuild); else Serial.print(F("none"));
            Serial.println(F(") — reformatting LittleFS"));

            // 1. Snapshot the current /config.bin (if any) so we can
            //    restore device identity after the format. We read into
            //    a local ConfigData rather than into data_ because data_
            //    will be (re-)populated by loadFromFlash below if the
            //    restore path succeeds, OR by loadDefaults if it doesn't.
            ConfigData snapshot;
            memset(&snapshot, 0, sizeof(snapshot));
            bool haveSnapshot = false;
            if (InternalFS.exists(CONFIG_FILENAME)) {
                configFile->open(CONFIG_FILENAME, FILE_O_READ);
                if (*configFile) {
                    size_t bytesRead = configFile->read(
                        reinterpret_cast<uint8_t*>(&snapshot), sizeof(snapshot));
                    configFile->close();
                    if (bytesRead >= sizeof(uint16_t) && snapshot.magic == MAGIC_NUMBER) {
                        haveSnapshot = true;
                    }
                }
            }

            // 2. Reformat. LittleFS's format() unmounts → lfs_format →
            //    remounts; on failure the FS is left in an undefined state
            //    but the next reboot retries (marker unwritten).
            bool formatOk = InternalFS.format();
            if (!formatOk) {
                Serial.println(F("[FRESH-BUILD] InternalFS.format() FAILED — proceeding without reformat"));
            } else {
                Serial.println(F("[FRESH-BUILD] LittleFS reformatted"));
            }

            // 3. Restore /config.bin from snapshot if we have it. Even on
            //    format failure we still re-write the snapshot — the
            //    existing /config.bin may have been erased mid-format.
            //    Skip if no usable snapshot existed (fresh chip with no
            //    prior config: nothing to restore, the loadDefaults
            //    path below will handle it).
            if (haveSnapshot) {
                // Refresh configFile pointer's mount association after
                // format. Adafruit_LittleFS::format remounts internally,
                // but the cached File pointer may have stale handles —
                // delete + recreate to be safe.
                delete configFile;
                configFile = new File(InternalFS);
                configFile->open(CONFIG_FILENAME, FILE_O_WRITE);
                if (*configFile) {
                    size_t wrote = configFile->write(
                        reinterpret_cast<const uint8_t*>(&snapshot), sizeof(snapshot));
                    configFile->close();
                    if (wrote == sizeof(snapshot)) {
                        Serial.println(F("[FRESH-BUILD] /config.bin restored from snapshot"));
                    } else {
                        Serial.println(F("[FRESH-BUILD] /config.bin restore short-write; device will boot to safe mode"));
                    }
                } else {
                    Serial.println(F("[FRESH-BUILD] /config.bin restore open failed; device will boot to safe mode"));
                }
            }

            // 4. Write the freshness marker LAST. Until this lands, the
            //    next boot still sees stale-or-missing marker and runs
            //    the same path — idempotent against the just-restored
            //    /config.bin, so we never lose state if interrupted
            //    between steps 1-3 and now.
            if (writeFwBuildMarker(FIRMWARE_BUILD)) {
                Serial.println(F("[FRESH-BUILD] marker updated; fresh-build path complete"));
            } else {
                Serial.println(F("[FRESH-BUILD] marker write FAILED — next boot will repeat the fresh-build path"));
            }
        }
    }

    if (loadFromFlash()) {
        SerialConsole::logDebug(F("Config loaded from flash"));
        valid_ = true;
        return;
    }
#elif defined(ESP32)
    bool nvsOk = prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    flashOk = nvsOk;

    // Always print NVS diagnostics on ESP32 (not gated on log level)
    // to help diagnose persistence issues during early bring-up
    Serial.print(F("[NVS] begin="));
    Serial.print(nvsOk ? F("ok") : F("FAIL"));
    Serial.print(F(" storedBytes="));
    Serial.print(nvsOk ? (uint32_t)prefs.getBytesLength(NVS_KEY) : 0);
    Serial.print(F(" sizeof(ConfigData)="));
    Serial.println(sizeof(ConfigData));

    if (loadFromFlash()) {
        valid_ = true;
        Serial.print(F("[NVS] Loaded: magic=ok dev="));
        Serial.print(data_.device.deviceName);
        Serial.print(F(" isValid="));
        Serial.println(data_.device.isValid ? F("true") : F("false"));
        return;
    }
    Serial.println(F("[NVS] loadFromFlash failed - using defaults"));
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

    // Fire defaults (particle-based) - dimension-independent fractions (v69)
    // Values are multiplied by device dimensions at use-time.
    data_.fire.baseSpawnChance = 0.05f;   // density: expected sparks per crossDim-unit per frame
    data_.fire.audioSpawnBoost = 0.15f;   // additional density at max energy
    data_.fire.windVariation = 1.5f;        // × crossDim → curl turbulence amplitude
    data_.fire.drag = 0.985f;
    data_.fire.sparkVelocityMin = 0.33f;
    data_.fire.sparkVelocityMax = 0.67f;
    data_.fire.sparkSpread = 1.0f;
    data_.fire.organicTransientMin = 0.25f;
    data_.fire.thermalForce = 2.0f;         // × traversalDim → buoyancy LEDs/sec^2
    data_.fire.maxParticles = 0.75f;        // Pool sized at begin() only
    data_.fire.burstSparks = 0.5f;          // × crossDim → sparks per burst
    data_.fire.gridCoolRate = 0.88f;        // ~8-frame heat decay at 30fps
    data_.fire.buoyancyCoupling = 1.0f;     // Grid heat → plume upward reinforcement
    data_.fire.pressureCoupling = 0.5f;     // Lateral clustering toward hot columns
    data_.fire.defaultLifespan = 100;       // 1.0s (centiseconds)
    data_.fire.intensityMin = 150;
    data_.fire.intensityMax = 220;

    // Water defaults (particle-based) - dimension-independent fractions (v69)
    data_.water.baseSpawnChance = 0.8f;
    data_.water.audioSpawnBoost = 0.3f;
    data_.water.gravity = 1.67f;            // × traversalDim → downward acceleration
    data_.water.windBase = 0.0f;
    data_.water.windVariation = 0.2f;       // × crossDim → sway amplitude
    data_.water.drag = 0.995f;
    data_.water.dropVelocityMin = 0.4f;     // × traversalDim/sec
    data_.water.dropVelocityMax = 0.67f;    // × traversalDim/sec
    data_.water.dropSpread = 0.375f;        // × crossDim
    data_.water.splashVelocityMin = 0.27f;  // × traversalDim
    data_.water.splashVelocityMax = 0.53f;  // × traversalDim
    data_.water.organicTransientMin = 0.5f;
    data_.water.backgroundIntensity = 0.15f;
    data_.water.maxParticles = 0.5f;  // Fraction of numLeds (clamped to pool 30)
    data_.water.defaultLifespan = 90;
    data_.water.intensityMin = 80;
    data_.water.intensityMax = 200;
    data_.water.splashParticles = 0.75f; // × crossDim → particles per splash
    data_.water.splashIntensity = 120;

    // Plasma globe defaults (continuous field, replaces lightning)
    PlasmaGlobeParams plasmaDefaults;
    data_.plasma.backgroundDim = plasmaDefaults.backgroundDim;
    data_.plasma.orbBrightness = plasmaDefaults.orbBrightness;
    data_.plasma.orbRadius = plasmaDefaults.orbRadius;
    data_.plasma.driftSpeed = plasmaDefaults.driftSpeed;
    data_.plasma.pulseDecay = plasmaDefaults.pulseDecay;
    data_.plasma.pulseBrightness = plasmaDefaults.pulseBrightness;
    data_.plasma.pulseExpand = plasmaDefaults.pulseExpand;

    // Mic defaults (hardware-primary, window/range normalization)
    // Window/Range normalization (v72: AGC removed, gain fixed at platform default)
    // v72: changed from 1.0/3.0 to 2.0/5.0 — with fixed gain, slower tracking avoids
    // over-reacting to transients that the AGC would have absorbed. These values match
    // AdaptiveMic.h defaults and were tested on both nRF52840 and ESP32-S3.
    data_.mic.peakTau = 2.0f;        // 2s peak adaptation
    data_.mic.releaseTau = 5.0f;     // 5s peak release

    // (StoredMusicParams removed v76 — 312 bytes, 174 lines of dead AudioController defaults.
    //  See git history for v23-v75 content: CBSS, Bayesian fusion, octave checks,
    //  spectral processing, forward filter, particle filter, HMM, noise estimation, etc.)

    // AudioTracker defaults (v74+)
    data_.tracker.bpmMin = 15.0f;
    data_.tracker.bpmMax = 200.0f;
    data_.tracker.tempoSmoothing = 0.85f;
    data_.tracker.acfPeriodMs = 100;
    data_.tracker.activationThreshold = 0.3f;
    data_.tracker.pulseOnsetFloor = 0.2f;
    data_.tracker.odfContrast = 1.25f;
    data_.tracker.pulseThresholdMult = 2.0f;
    data_.tracker.pulseMinLevel = 0.03f;
    data_.tracker.pulseNNGate = 0.3f;
    data_.tracker.crestGateMin = 0.0f;  // v95: disabled by default; sweep to tune
    data_.tracker.baselineFastDrop = 0.05f;
    data_.tracker.baselineSlowRise = 0.005f;
    data_.tracker.odfPeakHoldDecay = 0.85f;
    data_.tracker.energyMicWeight = 0.30f;
    data_.tracker.energyMelWeight = 0.30f;
    data_.tracker.energyOdfWeight = 0.40f;
    data_.tracker.plpActivation = 0.3f;
    data_.tracker.plpConfAlpha = 0.25f;
    data_.tracker.plpNovGain = 1.0f;
    data_.tracker.plpSignalFloor = 0.10f;
    data_.tracker.plpVarianceSens = 0.0f;
    data_.tracker.plpDecayRate = 0.2f;
    data_.tracker.bassFluxWeight = 0.5f;
    data_.tracker.midFluxWeight = 0.2f;
    data_.tracker.highFluxWeight = 0.3f;

    // Pattern slot cache (v82)
    data_.tracker.slotSwitchThreshold = 0.70f;
    data_.tracker.slotNewThreshold = 0.40f;
    data_.tracker.slotUpdateRate = 0.15f;
    data_.tracker.slotSaveMinConf = 0.25f;
    data_.tracker.slotSeedBlend = 0.70f;

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
#elif defined(ESP32)
    if (!flashOk) return false;

    size_t stored = prefs.getBytesLength(NVS_KEY);
    if (stored < MIN_DEVICE_BYTES) return false;

    bytesRead = prefs.getBytes(NVS_KEY, &temp, sizeof(ConfigData));
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
        memcpy(&data_.plasma, &temp.plasma, sizeof(StoredPlasmaParams));
        memcpy(&data_.mic, &temp.mic, sizeof(StoredMicParams));
        // (StoredMusicParams memcpy removed v76 — struct deleted)
        memcpy(&data_.tracker, &temp.tracker, sizeof(StoredTrackerParams));
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
        if (!InternalFS.remove(CONFIG_FILENAME)) {
            SerialConsole::logError(F("Failed to remove old config file"));
            return;
        }
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
#elif defined(ESP32)
    if (!flashOk) {
        SerialConsole::logWarn(F("Flash not available"));
        return;
    }

    data_.magic = MAGIC_NUMBER;
    data_.deviceVersion = DEVICE_VERSION;
    data_.settingsVersion = SETTINGS_VERSION;

    size_t bytesWritten = prefs.putBytes(NVS_KEY, &data_, sizeof(ConfigData));
    if (bytesWritten != sizeof(ConfigData)) {
        SerialConsole::logError(F("Config write failed"));
        return;
    }

    SerialConsole::logDebug(F("Config saved to NVS"));
#else
    SerialConsole::logWarn(F("No flash on this platform"));
#endif
}

bool ConfigStorage::quarantineDeviceConfig() {
    // Crash-loop recovery: blow away the `isValid` flag on the stored device
    // config, write to flash synchronously, and verify by re-reading. After
    // this call the caller is expected to reboot (either NVIC_SystemReset or
    // via SafeBootWatchdog::enterBleDfuBootloader). On the next boot the
    // firmware sees no valid device config and falls through to safeMode —
    // the same path a freshly-flashed device takes when no config has ever
    // been uploaded.
    //
    // The caller (RebootFrequencyCounter::checkAndIncrement, on threshold)
    // is responsible for deciding when this fires. It must be rare —
    // wiping configs on transient blips would be worse than the disease.
    if (!data_.device.isValid) {
        // Already invalid; nothing to do.
        SerialConsole::logInfo(F("[QUARANTINE] device config already invalid; no-op"));
        return true;
    }

    SerialConsole::logError(F("[QUARANTINE] invalidating device config — next boot enters safeMode"));
    data_.device.isValid = false;

    // saveToFlash() is void on this platform; it logs internally on failure
    // but the caller can't see the result. Best we can do is verify the
    // write by re-reading the config from flash and confirming `isValid` is
    // now false on the persisted copy. If the persisted copy still reads
    // valid, the write either failed outright or was rolled back — log it
    // loudly so the operator knows the next boot will replay the crash
    // instead of falling into safeMode.
    saveToFlash();

    // Re-read into a scratch struct without disturbing the in-RAM `data_`
    // (which other code paths may still depend on this boot session).
    //
    // Pessimistic default: treat read failure as "still valid", which causes
    // the post-block to log loudly and return false. This is the
    // intentionally-safe behaviour on an unrecognised platform — cppcheck
    // sees the if/elif chain preprocess out under its default config and
    // warns the subsequent `if (persisted_valid)` is "always true". That's
    // correct under cppcheck's worldview, but exactly the desired runtime
    // behaviour: if we can't verify, treat as failure.
    bool persisted_valid = true;
#if defined(ARDUINO_ARCH_NRF52) || defined(NRF52) || defined(NRF52840_XXAA)
    if (flashOk && configFile != nullptr) {
        using namespace Adafruit_LittleFS_Namespace;
        ConfigData verify = {};
        configFile->open(CONFIG_FILENAME, FILE_O_READ);
        if (*configFile) {
            uint32_t n = configFile->read(reinterpret_cast<uint8_t*>(&verify), sizeof(ConfigData));
            configFile->close();
            if (n == sizeof(ConfigData)) {
                persisted_valid = verify.device.isValid;
            }
        }
    }
#elif defined(ESP32)
    // ESP32 path: read back via NVS to verify. Best-effort.
    ConfigData verify = {};
    if (prefs.getBytes(NVS_KEY, &verify, sizeof(ConfigData)) == sizeof(ConfigData)) {
        persisted_valid = verify.device.isValid;
    }
#endif

    // cppcheck-suppress knownConditionTrueFalse
    if (persisted_valid) {
        SerialConsole::logError(F("[QUARANTINE] FLASH WRITE DID NOT PERSIST — next "
                                  "boot will replay the crash. Recovery requires "
                                  "manual `factory` or BLE-DFU app reflash."));
        return false;
    }

    SerialConsole::logInfo(F("[QUARANTINE] flash write verified — safeMode armed"));
    return true;
}

void ConfigStorage::end() {
#ifdef ESP32
    // Closes the NVS Preferences handle and flushes any pending writes.
    // IMPORTANT: This is only called from SerialConsole's "reboot" command.
    // Watchdog resets, hard faults, stack overflows, and other uncontrolled
    // restarts will skip this call and may lose the last-written settings.
    // esp_register_shutdown_handler() could cover more paths but is not yet
    // wired up — track as a known limitation until NVS write-through is added.
    prefs.end();
    flashOk = false;
#endif
}

void ConfigStorage::loadConfiguration(FireParams& fireParams, WaterParams& waterParams, PlasmaGlobeParams& plasmaParams,
                                      AdaptiveMic& mic, AudioTracker* tracker) {
    // Validation helpers — clamp individual bad params to nearest bound.
    // Preserves all other settings instead of wiping everything.
    int fixedCount = 0;

    // Out-of-range stored config values are configuration corruption — flash
    // bit-rot, downgrade across an incompatible schema, or a bad save path.
    // Always log at ERROR (no opt-out) and bump the global error counter so
    // the corruption shows up in `show errors` even when WARN is gated off.
    // Clamp is the recovery path so the device boots with usable values; it
    // is not the silent-fallback path. See CLAUDE.md "No Silent Fallbacks".
    auto validateFloat = [&](float& value, float min, float max, const __FlashStringHelper* name) {
        if (value < min || value > max) {
            float clamped = value < min ? min : max;
            Serial.print(F("[ERROR] Corrupt config "));
            Serial.print(name);
            Serial.print(F(": "));
            Serial.print(value);
            Serial.print(F(" out of ["));
            Serial.print(min);
            Serial.print(F(", "));
            Serial.print(max);
            Serial.print(F("], clamped to "));
            Serial.println(clamped);
            BlinkyAssert::failCount++;
            value = clamped;
            fixedCount++;
        }
    };

    // Macro-based integer validator — works with uint8_t, uint16_t, uint32_t
    // (lambdas can't be templated in C++11)
    // Integer counterpart to validateFloat above. Same loud-failure rules:
    // ERROR-level log (no opt-out), bumps BlinkyAssert::failCount, then clamps
    // so the device still boots. See CLAUDE.md "No Silent Fallbacks".
    #define VALIDATE_INT(value, lo, hi, name) do { \
        if ((value) < (lo) || (value) > (hi)) { \
            auto _clamped = (value) < (lo) ? (lo) : (hi); \
            Serial.print(F("[ERROR] Corrupt config ")); \
            Serial.print(name); \
            Serial.print(F(": ")); \
            Serial.print(value); \
            Serial.print(F(" out of [")); \
            Serial.print(lo); \
            Serial.print(F(", ")); \
            Serial.print(hi); \
            Serial.print(F("], clamped to ")); \
            Serial.println(_clamped); \
            BlinkyAssert::failCount++; \
            (value) = _clamped; \
            fixedCount++; \
        } \
    } while(0)

    // Validate critical parameters - if out of range, use defaults
    validateFloat(data_.fire.baseSpawnChance, 0.0f, 1.0f, F("baseSpawnChance"));
    validateFloat(data_.fire.audioSpawnBoost, 0.0f, 2.0f, F("audioSpawnBoost"));

    // Validate window/range normalization parameters
    validateFloat(data_.mic.peakTau, 0.5f, 10.0f, F("peakTau"));
    validateFloat(data_.mic.releaseTau, 1.0f, 30.0f, F("releaseTau"));

    // (StoredMusicParams validation removed v76 — 174 lines of dead validation for
    //  CBSS, Bayesian, octave, spectral, noise estimation params. See git history.)

    if (fixedCount > 0) {
        Serial.print(F("[WARN] Fixed "));
        Serial.print(fixedCount);
        Serial.println(F(" bad config param(s) (other settings preserved)"));
    }

    // Debug: show loaded values
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
        Serial.print(F("[DEBUG] baseSpawnChance=")); Serial.println(data_.fire.baseSpawnChance, 2);
    }

    fireParams.baseSpawnChance = data_.fire.baseSpawnChance;
    fireParams.audioSpawnBoost = data_.fire.audioSpawnBoost;
    fireParams.windVariation = data_.fire.windVariation;
    fireParams.drag = data_.fire.drag;
    fireParams.sparkVelocityMin = data_.fire.sparkVelocityMin;
    fireParams.sparkVelocityMax = data_.fire.sparkVelocityMax;
    fireParams.sparkSpread = data_.fire.sparkSpread;
    fireParams.organicTransientMin = data_.fire.organicTransientMin;
    fireParams.thermalForce = data_.fire.thermalForce;
    fireParams.maxParticles = data_.fire.maxParticles;
    fireParams.burstSparks = data_.fire.burstSparks;
    fireParams.gridCoolRate = data_.fire.gridCoolRate;
    fireParams.buoyancyCoupling = data_.fire.buoyancyCoupling;
    fireParams.pressureCoupling = data_.fire.pressureCoupling;
    fireParams.defaultLifespan = data_.fire.defaultLifespan;
    fireParams.intensityMin = data_.fire.intensityMin;
    fireParams.intensityMax = data_.fire.intensityMax;

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
    waterParams.organicTransientMin = data_.water.organicTransientMin;
    // Background
    waterParams.backgroundIntensity = data_.water.backgroundIntensity;
    // Lifecycle
    waterParams.defaultLifespan = data_.water.defaultLifespan;
    waterParams.intensityMin = data_.water.intensityMin;
    waterParams.intensityMax = data_.water.intensityMax;
    waterParams.splashParticles = data_.water.splashParticles;
    waterParams.splashIntensity = data_.water.splashIntensity;
    waterParams.maxParticles = data_.water.maxParticles;

    // === PLASMA GLOBE PARAMETERS ===
    validateFloat(data_.plasma.backgroundDim, 0.0f, 0.1f, F("p_bgdim"));
    validateFloat(data_.plasma.orbBrightness, 0.0f, 1.0f, F("p_orbbright"));
    validateFloat(data_.plasma.orbRadius, 0.05f, 0.5f, F("p_orbradius"));
    validateFloat(data_.plasma.driftSpeed, 0.001f, 0.1f, F("p_driftspeed"));
    validateFloat(data_.plasma.pulseDecay, 0.8f, 0.99f, F("p_pulsedecay"));
    validateFloat(data_.plasma.pulseBrightness, 0.0f, 1.0f, F("p_pulsebright"));
    validateFloat(data_.plasma.pulseExpand, 0.0f, 1.0f, F("p_pulseexpand"));

    plasmaParams.backgroundDim = data_.plasma.backgroundDim;
    plasmaParams.orbBrightness = data_.plasma.orbBrightness;
    plasmaParams.orbRadius = data_.plasma.orbRadius;
    plasmaParams.driftSpeed = data_.plasma.driftSpeed;
    plasmaParams.pulseDecay = data_.plasma.pulseDecay;
    plasmaParams.pulseBrightness = data_.plasma.pulseBrightness;
    plasmaParams.pulseExpand = data_.plasma.pulseExpand;

    // Window/Range normalization parameters (v72: AGC removed, only these remain)
    mic.peakTau = data_.mic.peakTau;
    mic.releaseTau = data_.mic.releaseTau;

    // AudioTracker params (v74)
    if (tracker) {
        // Validate tracker params
        validateFloat(data_.tracker.bpmMin, 10.0f, 120.0f, F("tracker.bpmMin"));
        validateFloat(data_.tracker.bpmMax, 120.0f, 240.0f, F("tracker.bpmMax"));
        // Cross-field: ensure bpmMin < bpmMax after individual clamping
        if (data_.tracker.bpmMin >= data_.tracker.bpmMax) {
            data_.tracker.bpmMin = 15.0f;
            data_.tracker.bpmMax = 200.0f;
            fixedCount++;
        }
        validateFloat(data_.tracker.tempoSmoothing, 0.5f, 0.99f, F("tracker.tempoSmooth"));
        validateFloat(data_.tracker.activationThreshold, 0.0f, 1.0f, F("tracker.actThresh"));
        validateFloat(data_.tracker.pulseOnsetFloor, 0.0f, 0.5f, F("tracker.pulseOnsetFloor"));
        validateFloat(data_.tracker.odfContrast, 0.1f, 4.0f, F("tracker.odfContrast"));
        validateFloat(data_.tracker.pulseThresholdMult, 1.0f, 5.0f, F("tracker.pulseThrMult"));
        validateFloat(data_.tracker.pulseMinLevel, 0.0f, 0.2f, F("tracker.pulseMinLvl"));
        validateFloat(data_.tracker.pulseNNGate, 0.0f, 1.0f, F("tracker.pulseNNGate"));
        validateFloat(data_.tracker.crestGateMin, 0.0f, 20.0f, F("tracker.crestGateMin"));
        validateFloat(data_.tracker.baselineFastDrop, 0.01f, 0.2f, F("tracker.blFastDrop"));
        validateFloat(data_.tracker.baselineSlowRise, 0.001f, 0.05f, F("tracker.blSlowRise"));
        validateFloat(data_.tracker.odfPeakHoldDecay, 0.5f, 0.99f, F("tracker.odfPkDecay"));
        validateFloat(data_.tracker.energyMicWeight, 0.0f, 1.0f, F("tracker.eMicW"));
        validateFloat(data_.tracker.energyMelWeight, 0.0f, 1.0f, F("tracker.eMelW"));
        validateFloat(data_.tracker.energyOdfWeight, 0.0f, 1.0f, F("tracker.eOdfW"));
        validateFloat(data_.tracker.plpActivation, 0.0f, 1.0f, F("tracker.plpAct"));
        validateFloat(data_.tracker.plpConfAlpha, 0.01f, 0.5f, F("tracker.plpConfAlpha"));
        validateFloat(data_.tracker.plpNovGain, 0.1f, 5.0f, F("tracker.plpNovGain"));
        validateFloat(data_.tracker.plpSignalFloor, 0.01f, 0.5f, F("tracker.plpSigFloor"));
        validateFloat(data_.tracker.plpVarianceSens, 0.0f, 50.0f, F("tracker.plpVarSens"));
        validateFloat(data_.tracker.plpDecayRate, 0.05f, 1.0f, F("tracker.plpDecay"));
        validateFloat(data_.tracker.bassFluxWeight, 0.0f, 1.0f, F("tracker.bassFluxW"));
        validateFloat(data_.tracker.midFluxWeight, 0.0f, 1.0f, F("tracker.midFluxW"));
        validateFloat(data_.tracker.highFluxWeight, 0.0f, 1.0f, F("tracker.highFluxW"));
        VALIDATE_INT(data_.tracker.acfPeriodMs, 50, 500, F("tracker.acfPeriod"));

        // Pattern slot cache (v82)
        validateFloat(data_.tracker.slotSwitchThreshold, 0.50f, 0.95f, F("tracker.slotSwitch"));
        validateFloat(data_.tracker.slotNewThreshold, 0.20f, 0.60f, F("tracker.slotNew"));
        validateFloat(data_.tracker.slotUpdateRate, 0.05f, 0.40f, F("tracker.slotUpdate"));
        validateFloat(data_.tracker.slotSaveMinConf, 0.20f, 0.80f, F("tracker.slotSaveConf"));
        validateFloat(data_.tracker.slotSeedBlend, 0.30f, 0.95f, F("tracker.slotSeedBlend"));

        // Copy to AudioTracker
        tracker->bpmMin = data_.tracker.bpmMin;
        tracker->bpmMax = data_.tracker.bpmMax;
        tracker->tempoSmoothing = data_.tracker.tempoSmoothing;
        tracker->acfPeriodMs = data_.tracker.acfPeriodMs;
        tracker->activationThreshold = data_.tracker.activationThreshold;
        tracker->pulseOnsetFloor = data_.tracker.pulseOnsetFloor;
        tracker->odfContrast = data_.tracker.odfContrast;
        tracker->pulseThresholdMult = data_.tracker.pulseThresholdMult;
        tracker->pulseMinLevel = data_.tracker.pulseMinLevel;
        // pulseNNGate removed — NN is now primary signal, not gate. Field kept for binary compat.
        tracker->crestGateMin = data_.tracker.crestGateMin;  // v95
        tracker->baselineFastDrop = data_.tracker.baselineFastDrop;
        tracker->baselineSlowRise = data_.tracker.baselineSlowRise;
        tracker->odfPeakHoldDecay = data_.tracker.odfPeakHoldDecay;
        tracker->energyMicWeight = data_.tracker.energyMicWeight;
        tracker->energyMelWeight = data_.tracker.energyMelWeight;
        tracker->energyOdfWeight = data_.tracker.energyOdfWeight;
        // plpActivation removed v83 (vestigial since v81 soft blend). Field kept in StoredTrackerParams for binary compat.
        tracker->plpConfAlpha = data_.tracker.plpConfAlpha;
        tracker->plpNovGain = data_.tracker.plpNovGain;
        tracker->plpSignalFloor = data_.tracker.plpSignalFloor;
        tracker->plpVarianceSens = data_.tracker.plpVarianceSens;
        tracker->plpDecayRate = data_.tracker.plpDecayRate;

        // Spectral flux weights go to SharedSpectralAnalysis via tracker accessor
        tracker->getSpectral().bassFluxWeight = data_.tracker.bassFluxWeight;
        tracker->getSpectral().midFluxWeight = data_.tracker.midFluxWeight;
        tracker->getSpectral().highFluxWeight = data_.tracker.highFluxWeight;

        // Pattern slot cache (v82)
        tracker->slotSwitchThreshold = data_.tracker.slotSwitchThreshold;
        tracker->slotNewThreshold = data_.tracker.slotNewThreshold;
        tracker->slotUpdateRate = data_.tracker.slotUpdateRate;
        tracker->slotSaveMinConf = data_.tracker.slotSaveMinConf;
        tracker->slotSeedBlend = data_.tracker.slotSeedBlend;
    }

    #undef VALIDATE_INT
}

void ConfigStorage::saveConfiguration(const FireParams& fireParams, const WaterParams& waterParams, const PlasmaGlobeParams& plasmaParams,
                                      const AdaptiveMic& mic, AudioTracker* tracker) {
    data_.fire.baseSpawnChance = fireParams.baseSpawnChance;
    data_.fire.audioSpawnBoost = fireParams.audioSpawnBoost;
    data_.fire.windVariation = fireParams.windVariation;
    data_.fire.drag = fireParams.drag;
    data_.fire.sparkVelocityMin = fireParams.sparkVelocityMin;
    data_.fire.sparkVelocityMax = fireParams.sparkVelocityMax;
    data_.fire.sparkSpread = fireParams.sparkSpread;
    data_.fire.organicTransientMin = fireParams.organicTransientMin;
    data_.fire.thermalForce = fireParams.thermalForce;
    data_.fire.maxParticles = fireParams.maxParticles;
    data_.fire.burstSparks = fireParams.burstSparks;
    data_.fire.gridCoolRate = fireParams.gridCoolRate;
    data_.fire.buoyancyCoupling = fireParams.buoyancyCoupling;
    data_.fire.pressureCoupling = fireParams.pressureCoupling;
    data_.fire.defaultLifespan = fireParams.defaultLifespan;
    data_.fire.intensityMin = fireParams.intensityMin;
    data_.fire.intensityMax = fireParams.intensityMax;

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
    data_.water.organicTransientMin = waterParams.organicTransientMin;
    // Background
    data_.water.backgroundIntensity = waterParams.backgroundIntensity;
    // Lifecycle
    data_.water.defaultLifespan = waterParams.defaultLifespan;
    data_.water.intensityMin = waterParams.intensityMin;
    data_.water.intensityMax = waterParams.intensityMax;
    data_.water.splashParticles = waterParams.splashParticles;
    data_.water.splashIntensity = waterParams.splashIntensity;
    data_.water.maxParticles = waterParams.maxParticles;

    // === PLASMA GLOBE PARAMETERS ===
    data_.plasma.backgroundDim = plasmaParams.backgroundDim;
    data_.plasma.orbBrightness = plasmaParams.orbBrightness;
    data_.plasma.orbRadius = plasmaParams.orbRadius;
    data_.plasma.driftSpeed = plasmaParams.driftSpeed;
    data_.plasma.pulseDecay = plasmaParams.pulseDecay;
    data_.plasma.pulseBrightness = plasmaParams.pulseBrightness;
    data_.plasma.pulseExpand = plasmaParams.pulseExpand;

    // Window/Range normalization (v72: AGC removed)
    data_.mic.peakTau = mic.peakTau;
    data_.mic.releaseTau = mic.releaseTau;

    // AudioTracker params (v74)
    if (tracker) {
        data_.tracker.bpmMin = tracker->bpmMin;
        data_.tracker.bpmMax = tracker->bpmMax;
        data_.tracker.tempoSmoothing = tracker->tempoSmoothing;
        data_.tracker.acfPeriodMs = tracker->acfPeriodMs;
        data_.tracker.activationThreshold = tracker->activationThreshold;
        data_.tracker.pulseOnsetFloor = tracker->pulseOnsetFloor;
        data_.tracker.odfContrast = tracker->odfContrast;
        data_.tracker.pulseThresholdMult = tracker->pulseThresholdMult;
        data_.tracker.pulseMinLevel = tracker->pulseMinLevel;
        // pulseNNGate: field kept for binary compat, no longer written from tracker
        data_.tracker.crestGateMin = tracker->crestGateMin;  // v95
        data_.tracker.baselineFastDrop = tracker->baselineFastDrop;
        data_.tracker.baselineSlowRise = tracker->baselineSlowRise;
        data_.tracker.odfPeakHoldDecay = tracker->odfPeakHoldDecay;
        data_.tracker.energyMicWeight = tracker->energyMicWeight;
        data_.tracker.energyMelWeight = tracker->energyMelWeight;
        data_.tracker.energyOdfWeight = tracker->energyOdfWeight;
        // plpActivation: vestigial field kept for binary compat, not saved from tracker
        data_.tracker.plpConfAlpha = tracker->plpConfAlpha;
        data_.tracker.plpNovGain = tracker->plpNovGain;
        data_.tracker.plpSignalFloor = tracker->plpSignalFloor;
        data_.tracker.plpVarianceSens = tracker->plpVarianceSens;
        data_.tracker.plpDecayRate = tracker->plpDecayRate;
        data_.tracker.bassFluxWeight = tracker->getSpectral().bassFluxWeight;
        data_.tracker.midFluxWeight = tracker->getSpectral().midFluxWeight;
        data_.tracker.highFluxWeight = tracker->getSpectral().highFluxWeight;

        // Pattern slot cache (v82)
        data_.tracker.slotSwitchThreshold = tracker->slotSwitchThreshold;
        data_.tracker.slotNewThreshold = tracker->slotNewThreshold;
        data_.tracker.slotUpdateRate = tracker->slotUpdateRate;
        data_.tracker.slotSaveMinConf = tracker->slotSaveMinConf;
        data_.tracker.slotSeedBlend = tracker->slotSeedBlend;
    }

    saveToFlash();
    dirty_ = false;
    lastSaveMs_ = millis();
}

void ConfigStorage::saveIfDirty(const FireParams& fireParams, const WaterParams& waterParams, const PlasmaGlobeParams& plasmaParams,
                                const AdaptiveMic& mic, AudioTracker* tracker) {
    if (dirty_ && (millis() - lastSaveMs_ > 5000)) {  // Debounce: save at most every 5 seconds
        saveConfiguration(fireParams, waterParams, plasmaParams, mic, tracker);
    }
}

void ConfigStorage::factoryReset() {
    SerialConsole::logInfo(F("Factory reset"));
    loadDefaults();
    saveToFlash();
}
