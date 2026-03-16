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
    data_.fire.baseSpawnChance = 0.5f;      // Continuous sparks for constant fire
    data_.fire.audioSpawnBoost = 1.5f;      // Strong audio response
    data_.fire.gravity = 0.0f;              // No gravity (thermal force provides upward push)
    data_.fire.windBase = 0.0f;
    data_.fire.windVariation = 1.5f;        // × crossDim → turbulence amplitude
    data_.fire.drag = 0.985f;               // Smoother flow
    data_.fire.sparkVelocityMin = 0.33f;    // × traversalDim/sec → upward velocity
    data_.fire.sparkVelocityMax = 0.67f;    // × traversalDim/sec → upward velocity
    data_.fire.sparkSpread = 1.0f;          // × crossDim → horizontal scatter
    data_.fire.musicSpawnPulse = 0.95f;     // Tight beat sync
    data_.fire.organicTransientMin = 0.25f; // Responsive to softer transients
    data_.fire.backgroundIntensity = 0.15f; // Subtle noise background
    data_.fire.fastSparkRatio = 0.7f;       // 70% fast sparks, 30% embers
    data_.fire.thermalForce = 2.0f;          // × traversalDim → buoyancy LEDs/sec^2
    data_.fire.maxParticles = 0.75f;        // Fraction of numLeds (clamped to pool 64)
    data_.fire.defaultLifespan = 170;       // 1.7 seconds (170 centiseconds)
    data_.fire.intensityMin = 150;
    data_.fire.intensityMax = 220;
    data_.fire.burstSparks = 0.5f;          // × crossDim → sparks per burst

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
    data_.water.musicSpawnPulse = 0.4f;
    data_.water.organicTransientMin = 0.5f;
    data_.water.backgroundIntensity = 0.15f;
    data_.water.maxParticles = 0.5f;  // Fraction of numLeds (clamped to pool 30)
    data_.water.defaultLifespan = 90;
    data_.water.intensityMin = 80;
    data_.water.intensityMax = 200;
    data_.water.splashParticles = 0.75f; // × crossDim → particles per splash
    data_.water.splashIntensity = 120;

    // Lightning defaults (particle-based)
    data_.lightning.baseSpawnChance = 0.15f;
    data_.lightning.audioSpawnBoost = 0.5f;
    data_.lightning.branchAngleSpread = PI / 4.0f;  // 45 degree spread
    data_.lightning.musicSpawnPulse = 0.6f;
    data_.lightning.organicTransientMin = 0.3f;
    data_.lightning.backgroundIntensity = 0.15f;
    data_.lightning.maxParticles = 0.67f;  // Fraction of numLeds (clamped to pool 40)
    data_.lightning.defaultLifespan = 20;
    data_.lightning.intensityMin = 180;
    data_.lightning.intensityMax = 255;
    data_.lightning.fadeRate = 160;
    data_.lightning.branchChance = 30;
    data_.lightning.branchCount = 2;
    data_.lightning.branchIntensityLoss = 40;

    // Mic defaults (hardware-primary, window/range normalization)
    // Window/Range normalization (v72: AGC removed, gain fixed at platform default)
    // v72: changed from 1.0/3.0 to 2.0/5.0 — with fixed gain, slower tracking avoids
    // over-reacting to transients that the AGC would have absorbed. These values match
    // AdaptiveMic.h defaults and were tested on both nRF52840 and ESP32-S3.
    data_.mic.peakTau = 2.0f;        // 2s peak adaptation
    data_.mic.releaseTau = 5.0f;     // 5s peak release

    // AudioController rhythm tracking defaults
    data_.music.activationThreshold = 0.4f;
    data_.music.bpmMin = 60.0f;
    data_.music.bpmMax = 200.0f;
    data_.music.cbssAlpha = 0.9f;         // CBSS weighting (high = more predictive)

    // Tempo prior width (used by Bayesian static prior)
    data_.music.tempoPriorWidth = 50.0f;     // Balanced width

    // Pulse modulation
    data_.music.pulseBoostOnBeat = 1.3f;
    data_.music.pulseSuppressOffBeat = 0.6f;
    data_.music.energyBoostOnBeat = 0.3f;

    // Stability and smoothing
    data_.music.stabilityWindowBeats = 8.0f;
    data_.music.beatLookaheadMs = 120.0f;  // Predict beats 120ms ahead to reduce perceived latency
    data_.music.tempoSmoothingFactor = 0.85f;
    data_.music.tempoChangeThreshold = 0.1f;

    // CBSS beat tracking
    data_.music.cbssTightness = 8.0f;         // Log-Gaussian tightness (v40: raised from 5.0, +24% avg F1)
    data_.music.beatConfidenceDecay = 0.98f;   // Per-frame confidence decay
    data_.music.beatTimingOffset = 5.0f;       // Beat prediction advance (frames, ~83ms at 60Hz)
    data_.music.phaseCorrectionStrength = 0.0f; // Phase correction toward transients (disabled by default)
    data_.music.cbssThresholdFactor = 1.0f;    // CBSS adaptive threshold (0=off, beat fires only if CBSS > factor*mean)
    data_.music.cbssContrast = 2.0f;          // Power-law ODF contrast before CBSS (2=BTrack square, A/B tested 10-6 win)
    data_.music.cbssWarmupBeats = 0;          // CBSS warmup: lower alpha for first N beats (0=disabled)
    data_.music.onsetSnapWindow = 8;          // Snap beat to strongest OSS in last N frames (0=disabled)

    // Bayesian tempo fusion (v18+, defaults tuned Feb 2026 via 4-device sweep)
    // v25: BTrack-style harmonic comb ACF + Rayleigh prior + tighter lambda
    data_.music.bayesLambda = 0.60f;         // Wide transition (Viterbi max-product needs broad support)
    data_.music.bayesPriorCenter = 128.0f;   // Static prior center BPM (EDM midpoint)
    data_.music.bayesPriorWeight = 0.0f;     // Ongoing static prior strength (0=off, harmonic disambig handles sub-harmonics)
    data_.music.bayesAcfWeight = 0.8f;       // High weight: harmonic comb makes ACF reliable (v25)
    // (bayesFtWeight/bayesIoiWeight removed v52 — dead code since v28)
    data_.music.bayesCombWeight = 0.7f;      // Comb filter bank observation weight
    data_.music.posteriorFloor = 0.05f;      // 5% uniform mixing to prevent half-time mode lock (v30)
    data_.music.disambigNudge = 0.15f;       // Transfer 15% mass on disambiguation correction (v30)
    data_.music.harmonicTransWeight = 0.30f; // Harmonic transition shortcuts for 2:1/1:2/3:2 jumps (v30)

    // Onset-density octave discriminator (v31)
    data_.music.densityMinPerBeat = 0.5f;    // Min plausible transients per beat
    data_.music.densityMaxPerBeat = 5.0f;    // Max plausible transients per beat
    data_.music.densityPenaltyExp = 2.0f;    // Gaussian exponent for density penalty
    data_.music.densityTarget = 0.0f;        // Target transients/beat (0=disabled, uses min/max)
    data_.music.octaveScoreRatio = 1.3f;     // Shadow CBSS score ratio for octave switch (v32: was 1.5, aggressive works better)

    data_.music.odfSmoothWidth = 5;          // ODF smooth window (odd, 3-11)
    data_.music.octaveCheckBeats = 2;        // Check octave every N beats (v32: was 4, aggressive works better)
    // (ioiEnabled/ftEnabled removed v52 — dead code since v28)
    data_.music.odfMeanSubEnabled = false;   // ODF mean subtraction (v32: disabled — raw ODF +70% F1 vs global mean sub)
    data_.music.beatBoundaryTempo = true;    // Defer tempo to beat boundaries (v28, BTrack-style)
    // (unifiedOdf/onsetTrainOdf/odfDiffMode removed v67 — BandFlux pipeline removed)
    data_.music.adaptiveOdfThresh = false;   // Local-mean ODF threshold (v31, marginal benefit — keep off)
    data_.music.odfThreshWindow = 15;        // Half-window size (15 samples = ~250ms at 60Hz)
    // (odfSource removed v64 — experimental alternatives never used)
    data_.music.densityOctaveEnabled = true;  // Onset-density octave penalty (v32: enabled, +13% F1)
    data_.music.downwardCorrectEnabled = false; // Downward harmonic correction (experimental, overcorrects mid-tempo)
    data_.music.octaveCheckEnabled = true;   // Shadow CBSS octave check (v32: enabled, +13% F1)
    // (phaseCheck, PLP, harmonicSesqui defaults removed v44 — features deleted)
    data_.music.rayleighBpm = 140.0f;       // Rayleigh prior peak BPM (v63: 120→140, sweep shows fewer octave errors)
    data_.music.tempoNudge = 0.8f;          // switchTempo posterior mass transfer (v44: was hardcoded 0.3)
    data_.music.fold32Enabled = false;      // 3:2 octave folding (v44: OFF — no net benefit in 18-track sweep)
    data_.music.sesquiCheckEnabled = false; // 3:2 shadow octave check (v44: OFF — no net benefit in 18-track sweep)
    data_.music.bidirectionalSnap = true;   // Bidirectional onset snap (v44: delay beat by 3 frames for forward snap)
    // (harmonicSesqui default removed v44 — feature deleted)

    // Percival ACF harmonic pre-enhancement (v45)
    data_.music.percivalEnhance = true;     // Enable harmonic pre-enhancement
    data_.music.percivalWeight2 = 0.5f;     // 2nd harmonic fold weight
    data_.music.percivalWeight4 = 0.25f;    // 4th harmonic fold weight

    // PLL phase correction (v45)
    data_.music.pllEnabled = true;          // Enable PLL phase correction
    data_.music.pllKp = 0.15f;              // Proportional gain
    data_.music.pllKi = 0.005f;             // Integral gain

    // Adaptive CBSS tightness (v45)
    data_.music.adaptiveTightnessEnabled = true;  // Enable adaptive tightness
    data_.music.tightnessLowMult = 0.7f;    // Multiplier when onset confidence HIGH
    data_.music.tightnessHighMult = 1.3f;   // Multiplier when onset confidence LOW
    data_.music.tightnessConfThreshHigh = 3.0f;   // OSS/mean ratio for high confidence
    data_.music.tightnessConfThreshLow = 1.5f;    // OSS/mean ratio for low confidence

    data_.music.percivalWeight3 = 0.0f;      // Anti-harmonic 3rd comb (OFF by default)
    // (multiAgentEnabled/agentDecay/agentInitBeats removed v64)
    // (metricalCheckEnabled/metricalMinRatio/metricalCheckBeats removed v64)
    // (templateCheckEnabled/templateScoreRatio/templateCheckBeats removed v64)
    // (subbeatCheckEnabled/alternationThresh/subbeatCheckBeats removed v64)

    // Hidden calibration constants (v51)
    // (templateMinScore removed v64)
    data_.music.cbssMeanAlpha = 0.008f;       // CBSS running mean EMA alpha
    data_.music.harmonic2xThresh = 0.5f;      // ACF half-lag ratio for 2x BPM correction
    data_.music.harmonic15xThresh = 0.6f;     // ACF 2/3-lag ratio for 1.5x BPM correction
    data_.music.pllSmoother = 0.95f;          // PLL phase integral leaky decay
    data_.music.beatConfBoost = 0.15f;        // Confidence increment per beat fire
    data_.music.rhythmBlend = 0.6f;           // Periodicity weight in rhythmStrength
    data_.music.periodicityBlend = 0.7f;      // Periodicity strength EMA coefficient
    data_.music.onsetDensityBlend = 0.7f;    // Onset density EMA coefficient
    // (subbeatBins/templateHistBars removed v64)
    // (nnBeatActivation removed v68 — always on)

    // (forwardFilterEnabled and all fwd* params removed v64)
    // (fwdPhaseOnly removed v64)

    // v65 params (persisted v70)
    data_.music.snapHysteresis = 0.8f;       // Prefer previous snap if >0.8× best
    data_.music.dbEmaAlpha = 0.3f;           // Downbeat EMA smoothing alpha
    data_.music.dbThreshold = 0.5f;          // Smoothed downbeat activation threshold
    data_.music.dbDecay = 0.85f;             // Per-frame downbeat decay between beats
    data_.music.pllWarmupBeats = 5;          // PLL warmup: ±T/2 clamp for first 5 beats

    // Spectral noise estimation defaults (v56)
    data_.music.noiseEstEnabled = false;  // Default OFF until A/B validated
    data_.music.noiseSmoothAlpha = 0.92f;    // Power smoothing (~200ms at 62.5 Hz)
    data_.music.noiseReleaseFactor = 0.999f; // Noise floor release (~16s)
    data_.music.noiseOversubtract = 1.5f;    // Moderate oversubtraction
    data_.music.noiseFloorRatio = 0.02f;     // 2% spectral floor

    data_.music.btrkPipeline = true;         // BTrack pipeline (v33: Viterbi + comb-on-ACF, replaces multiplicative)
    data_.music.btrkThreshWindow = 0;        // Adaptive threshold OFF (too aggressive with 20 bins)
    // (barPointerHmm/hmmContrast/fwdObsLambda/fwdObsFloor/fwdWrapFraction removed v64)
    // (particleFilterEnabled and all pf* params removed v64)

    // Spectral processing defaults (v23+)
    data_.music.whitenEnabled = true;
    data_.music.compressorEnabled = true;
    data_.music.whitenBassBypass = false;     // Skip whitening for bass bins (v47)
    data_.music.whitenDecay = 0.997f;        // ~5s memory at 60fps
    data_.music.whitenFloor = 0.001f;        // Noise floor
    data_.music.compThresholdDb = -30.0f;    // dB threshold
    data_.music.compRatio = 3.0f;            // 3:1 compression
    data_.music.compKneeDb = 15.0f;          // Soft knee width
    data_.music.compMakeupDb = 6.0f;         // Makeup gain
    data_.music.compAttackTau = 0.001f;      // 1ms attack
    data_.music.compReleaseTau = 2.0f;       // 2s release

    // (BandFlux detector defaults removed v67 — BandFlux pipeline removed)

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
        memcpy(&data_.lightning, &temp.lightning, sizeof(StoredLightningParams));
        memcpy(&data_.mic, &temp.mic, sizeof(StoredMicParams));
        memcpy(&data_.music, &temp.music, sizeof(StoredMusicParams));
        // (StoredBandFluxParams memcpy removed v67 — struct removed)
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

void ConfigStorage::loadConfiguration(FireParams& fireParams, WaterParams& waterParams, LightningParams& lightningParams,
                                      AdaptiveMic& mic, AudioController* audioCtrl) {
    // Validation helpers — clamp individual bad params to nearest bound.
    // Preserves all other settings instead of wiping everything.
    int fixedCount = 0;

    auto validateFloat = [&](float& value, float min, float max, const __FlashStringHelper* name) {
        if (value < min || value > max) {
            float clamped = value < min ? min : max;
            if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
                Serial.print(F("[WARN] Bad config "));
                Serial.print(name);
                Serial.print(F(": "));
                Serial.print(value);
                Serial.print(F(" -> "));
                Serial.println(clamped);
            }
            value = clamped;
            fixedCount++;
        }
    };

    // Macro-based integer validator — works with uint8_t, uint16_t, uint32_t
    // (lambdas can't be templated in C++11)
    #define VALIDATE_INT(value, lo, hi, name) do { \
        if ((value) < (lo) || (value) > (hi)) { \
            auto _clamped = (value) < (lo) ? (lo) : (hi); \
            if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) { \
                Serial.print(F("[WARN] Bad config ")); \
                Serial.print(name); \
                Serial.print(F(": ")); \
                Serial.print(value); \
                Serial.print(F(" -> ")); \
                Serial.println(_clamped); \
            } \
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

    // v72: AGC removed — only window/range normalization params validated

    // AudioController validation (v23+)
    validateFloat(data_.music.activationThreshold, 0.0f, 1.0f, F("musicThresh"));
    validateFloat(data_.music.bpmMin, 40.0f, 120.0f, F("bpmMin"));
    validateFloat(data_.music.bpmMax, 120.0f, 240.0f, F("bpmMax"));
    validateFloat(data_.music.cbssAlpha, 0.5f, 0.99f, F("cbssAlpha"));

    // Tempo prior width validation
    validateFloat(data_.music.tempoPriorWidth, 10.0f, 100.0f, F("priorwidth"));

    // Pulse modulation validation (v25+)
    validateFloat(data_.music.pulseBoostOnBeat, 1.0f, 3.0f, F("pulseboost"));
    validateFloat(data_.music.pulseSuppressOffBeat, 0.1f, 1.0f, F("pulsesuppress"));
    validateFloat(data_.music.energyBoostOnBeat, 0.0f, 1.0f, F("energyboost"));

    // Stability and smoothing validation (v25+)
    validateFloat(data_.music.stabilityWindowBeats, 2.0f, 32.0f, F("stabilitywin"));
    validateFloat(data_.music.beatLookaheadMs, 0.0f, 200.0f, F("lookahead"));
    validateFloat(data_.music.tempoSmoothingFactor, 0.5f, 0.99f, F("temposmooth"));
    validateFloat(data_.music.tempoChangeThreshold, 0.01f, 0.5f, F("tempochgthresh"));

    // CBSS beat tracking validation
    validateFloat(data_.music.cbssTightness, 1.0f, 20.0f, F("cbssTightness"));
    validateFloat(data_.music.beatConfidenceDecay, 0.9f, 0.999f, F("beatConfDecay"));
    validateFloat(data_.music.beatTimingOffset, 0.0f, 15.0f, F("beatTimingOffset"));
    validateFloat(data_.music.phaseCorrectionStrength, 0.0f, 1.0f, F("phaseCorrStrength"));
    validateFloat(data_.music.cbssThresholdFactor, 0.0f, 2.0f, F("cbssThreshFactor"));
    validateFloat(data_.music.cbssContrast, 0.5f, 4.0f, F("cbssContrast"));
    // cppcheck-suppress unsignedLessThanZero
    VALIDATE_INT(data_.music.cbssWarmupBeats, 0, 32, F("cbssWarmupBeats"));
    // cppcheck-suppress unsignedLessThanZero
    VALIDATE_INT(data_.music.onsetSnapWindow, 0, 16, F("onsetSnapWindow"));

    // Bayesian tempo fusion validation (v18+)
    validateFloat(data_.music.bayesLambda, 0.01f, 1.0f, F("bayesLambda"));
    validateFloat(data_.music.bayesPriorCenter, 60.0f, 200.0f, F("bayesPriorCenter"));
    validateFloat(data_.music.bayesPriorWeight, 0.0f, 3.0f, F("bayesPriorWeight"));
    validateFloat(data_.music.bayesAcfWeight, 0.0f, 5.0f, F("bayesAcfWeight"));
    validateFloat(data_.music.bayesCombWeight, 0.0f, 5.0f, F("bayesCombWeight"));
    validateFloat(data_.music.posteriorFloor, 0.0f, 0.5f, F("posteriorFloor"));
    validateFloat(data_.music.disambigNudge, 0.0f, 0.5f, F("disambigNudge"));
    validateFloat(data_.music.harmonicTransWeight, 0.0f, 1.0f, F("harmonicTransWeight"));
    if (data_.music.odfSmoothWidth < 3 || data_.music.odfSmoothWidth > 11) {
        SerialConsole::logWarn(F("Invalid odfSmoothWidth, clamping"));
        data_.music.odfSmoothWidth = data_.music.odfSmoothWidth < 3 ? 3 : 11;
        fixedCount++;
    }
    // odfMeanSubEnabled, adaptiveOdfThresh, densityOctaveEnabled, octaveCheckEnabled are bools — no range validation needed
    // (onsetTrainOdf removed v67)
    validateFloat(data_.music.rayleighBpm, 60.0f, 180.0f, F("rayleighBpm"));
    validateFloat(data_.music.tempoNudge, 0.0f, 1.0f, F("tempoNudge"));
    // fold32Enabled, sesquiCheckEnabled, bidirectionalSnap, harmonicSesqui are bools — no range validation needed

    // Percival ACF harmonic pre-enhancement validation (v45)
    validateFloat(data_.music.percivalWeight2, 0.0f, 1.0f, F("percivalWeight2"));
    validateFloat(data_.music.percivalWeight4, 0.0f, 1.0f, F("percivalWeight4"));
    // percivalEnhance is bool — no range validation needed

    // PLL phase correction validation (v45)
    validateFloat(data_.music.pllKp, 0.0f, 1.0f, F("pllKp"));
    validateFloat(data_.music.pllKi, 0.0f, 0.1f, F("pllKi"));
    // pllEnabled is bool — no range validation needed

    // Adaptive tightness validation (v45)
    validateFloat(data_.music.tightnessLowMult, 0.3f, 1.0f, F("tightnessLowMult"));
    validateFloat(data_.music.tightnessHighMult, 1.0f, 3.0f, F("tightnessHighMult"));
    validateFloat(data_.music.tightnessConfThreshHigh, 1.5f, 10.0f, F("tightnessConfThreshHigh"));
    validateFloat(data_.music.tightnessConfThreshLow, 0.5f, 3.0f, F("tightnessConfThreshLow"));
    // Disable adaptive tightness if thresholds are inverted (high must exceed low)
    if (data_.music.tightnessConfThreshHigh <= data_.music.tightnessConfThreshLow) {
        data_.music.adaptiveTightnessEnabled = false;
    }
    // adaptiveTightnessEnabled is bool — no range validation needed

    validateFloat(data_.music.percivalWeight3, 0.0f, 1.0f, F("percivalWeight3"));
    // (multiAgent/metrical/template/subbeat validation removed v64 — features deleted)

    // Hidden calibration constants (v51)
    validateFloat(data_.music.cbssMeanAlpha, 0.001f, 0.1f, F("cbssMeanAlpha"));
    validateFloat(data_.music.harmonic2xThresh, 0.1f, 0.9f, F("harmonic2xThresh"));
    validateFloat(data_.music.harmonic15xThresh, 0.1f, 0.9f, F("harmonic15xThresh"));
    validateFloat(data_.music.pllSmoother, 0.8f, 0.99f, F("pllSmoother"));
    validateFloat(data_.music.beatConfBoost, 0.01f, 0.5f, F("beatConfBoost"));
    validateFloat(data_.music.rhythmBlend, 0.0f, 1.0f, F("rhythmBlend"));
    validateFloat(data_.music.periodicityBlend, 0.3f, 0.95f, F("periodicityBlend"));
    validateFloat(data_.music.onsetDensityBlend, 0.3f, 0.95f, F("onsetDensityBlend"));
    // (subbeatBins/templateHistBars/odfSource validation removed v64 — features deleted)
    if (data_.music.odfThreshWindow < 5 || data_.music.odfThreshWindow > 30) {
        SerialConsole::logWarn(F("Invalid odfThreshWindow, clamping"));
        data_.music.odfThreshWindow = data_.music.odfThreshWindow < 5 ? 5 : 30;
        fixedCount++;
    }

    // Onset-density octave discriminator validation (v31)
    validateFloat(data_.music.densityMinPerBeat, 0.1f, 3.0f, F("densityMinPerBeat"));
    validateFloat(data_.music.densityMaxPerBeat, 1.0f, 20.0f, F("densityMaxPerBeat"));
    validateFloat(data_.music.densityPenaltyExp, 1.0f, 20.0f, F("densityPenaltyExp"));
    validateFloat(data_.music.densityTarget, 0.0f, 10.0f, F("densityTarget"));
    if (data_.music.densityMinPerBeat >= data_.music.densityMaxPerBeat) {
        SerialConsole::logWarn(F("densityMinPerBeat >= densityMaxPerBeat, resetting"));
        data_.music.densityMinPerBeat = 0.5f;
        data_.music.densityMaxPerBeat = 5.0f;
        fixedCount++;
    }
    validateFloat(data_.music.octaveScoreRatio, 1.0f, 5.0f, F("octaveScoreRatio"));
    // (hmmContrast/fwdObsLambda/fwdObsFloor/fwdWrapFraction validation removed v64 — HMM/phase tracker deleted)
    // (particleFilter validation removed v64 — PF deleted)

    if (data_.music.octaveCheckBeats < 2 || data_.music.octaveCheckBeats > 16) {
        SerialConsole::logWarn(F("Invalid octaveCheckBeats, clamping"));
        data_.music.octaveCheckBeats = data_.music.octaveCheckBeats < 2 ? 2 : 16;
        fixedCount++;
    }

    // Spectral processing validation (v23+)
    validateFloat(data_.music.whitenDecay, 0.9f, 0.9999f, F("whitenDecay"));
    validateFloat(data_.music.whitenFloor, 0.0001f, 0.1f, F("whitenFloor"));
    validateFloat(data_.music.compThresholdDb, -60.0f, 0.0f, F("compThreshDb"));
    validateFloat(data_.music.compRatio, 1.0f, 20.0f, F("compRatio"));
    validateFloat(data_.music.compKneeDb, 0.0f, 30.0f, F("compKneeDb"));
    validateFloat(data_.music.compMakeupDb, -10.0f, 30.0f, F("compMakeupDb"));
    validateFloat(data_.music.compAttackTau, 0.0001f, 0.1f, F("compAttackTau"));
    validateFloat(data_.music.compReleaseTau, 0.01f, 10.0f, F("compReleaseTau"));
    // whitenEnabled, compressorEnabled, whitenBassBypass are bools — no range validation needed

    // v65 params (persisted v70)
    validateFloat(data_.music.snapHysteresis, 0.0f, 1.0f, F("snapHysteresis"));
    validateFloat(data_.music.dbEmaAlpha, 0.01f, 1.0f, F("dbEmaAlpha"));
    validateFloat(data_.music.dbThreshold, 0.0f, 1.0f, F("dbThreshold"));
    validateFloat(data_.music.dbDecay, 0.5f, 0.99f, F("dbDecay"));
    // cppcheck-suppress unsignedLessThanZero
    VALIDATE_INT(data_.music.pllWarmupBeats, 0, 32, F("pllWarmupBeats"));

    // Noise estimation validation (v56)
    validateFloat(data_.music.noiseSmoothAlpha, 0.8f, 0.999f, F("noiseSmoothAlpha"));
    validateFloat(data_.music.noiseReleaseFactor, 0.99f, 0.9999f, F("noiseRelease"));
    validateFloat(data_.music.noiseOversubtract, 0.5f, 5.0f, F("noiseOversubtract"));
    validateFloat(data_.music.noiseFloorRatio, 0.001f, 0.5f, F("noiseFloorRatio"));

    // (BandFlux detector validation removed v67 — struct removed)

    // Validate BPM range consistency
    if (data_.music.bpmMin >= data_.music.bpmMax) {
        SerialConsole::logWarn(F("Invalid BPM range, swapping"));
        float tmp = data_.music.bpmMin;
        data_.music.bpmMin = data_.music.bpmMax;
        data_.music.bpmMax = tmp;
        fixedCount++;
    }

    #undef VALIDATE_INT

    if (fixedCount > 0) {
        Serial.print(F("[WARN] Fixed "));
        Serial.print(fixedCount);
        Serial.println(F(" bad config param(s) (other settings preserved)"));
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
    waterParams.maxParticles = data_.water.maxParticles;

    // === LIGHTNING PARAMETERS ===
    // Spawn behavior
    lightningParams.baseSpawnChance = data_.lightning.baseSpawnChance;
    lightningParams.audioSpawnBoost = data_.lightning.audioSpawnBoost;
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
    lightningParams.maxParticles = data_.lightning.maxParticles;

    // Window/Range normalization parameters (v72: AGC removed, only these remain)
    mic.peakTau = data_.mic.peakTau;
    mic.releaseTau = data_.mic.releaseTau;

    // AudioController parameters (v23+)
    // All rhythm tracking params are now public tunable members
    if (audioCtrl) {
        // Basic rhythm parameters
        audioCtrl->bpmMin = data_.music.bpmMin;
        audioCtrl->bpmMax = data_.music.bpmMax;
        audioCtrl->activationThreshold = data_.music.activationThreshold;
        audioCtrl->cbssAlpha = data_.music.cbssAlpha;

        // Tempo prior width (used by Bayesian static prior)
        audioCtrl->tempoPriorWidth = data_.music.tempoPriorWidth;

        // Pulse modulation
        audioCtrl->pulseBoostOnBeat = data_.music.pulseBoostOnBeat;
        audioCtrl->pulseSuppressOffBeat = data_.music.pulseSuppressOffBeat;
        audioCtrl->energyBoostOnBeat = data_.music.energyBoostOnBeat;

        // Stability and smoothing
        audioCtrl->stabilityWindowBeats = data_.music.stabilityWindowBeats;
        audioCtrl->beatLookaheadMs = data_.music.beatLookaheadMs;
        audioCtrl->tempoSmoothingFactor = data_.music.tempoSmoothingFactor;
        audioCtrl->tempoChangeThreshold = data_.music.tempoChangeThreshold;

        // CBSS beat tracking
        audioCtrl->cbssTightness = data_.music.cbssTightness;
        audioCtrl->beatConfidenceDecay = data_.music.beatConfidenceDecay;
        audioCtrl->beatTimingOffset = data_.music.beatTimingOffset;
        audioCtrl->phaseCorrectionStrength = data_.music.phaseCorrectionStrength;
        audioCtrl->cbssThresholdFactor = data_.music.cbssThresholdFactor;
        audioCtrl->cbssContrast = data_.music.cbssContrast;
        audioCtrl->cbssWarmupBeats = data_.music.cbssWarmupBeats;
        audioCtrl->onsetSnapWindow = data_.music.onsetSnapWindow;

        // Bayesian tempo fusion (v18+)
        audioCtrl->bayesLambda = data_.music.bayesLambda;
        audioCtrl->bayesPriorCenter = data_.music.bayesPriorCenter;
        audioCtrl->bayesPriorWeight = data_.music.bayesPriorWeight;
        audioCtrl->bayesAcfWeight = data_.music.bayesAcfWeight;
        audioCtrl->bayesCombWeight = data_.music.bayesCombWeight;
        audioCtrl->posteriorFloor = data_.music.posteriorFloor;
        audioCtrl->disambigNudge = data_.music.disambigNudge;
        audioCtrl->harmonicTransWeight = data_.music.harmonicTransWeight;

        audioCtrl->odfSmoothWidth = data_.music.odfSmoothWidth;
        audioCtrl->octaveCheckBeats = data_.music.octaveCheckBeats;
        audioCtrl->odfMeanSubEnabled = data_.music.odfMeanSubEnabled;
        audioCtrl->beatBoundaryTempo = data_.music.beatBoundaryTempo;
        // (unifiedOdf/onsetTrainOdf/odfDiffMode load removed v67 — BandFlux pipeline removed)
        audioCtrl->adaptiveOdfThresh = data_.music.adaptiveOdfThresh;
        audioCtrl->odfThreshWindow = data_.music.odfThreshWindow;
        // (odfSource load removed v64 — experimental alternatives deleted)
        audioCtrl->densityOctaveEnabled = data_.music.densityOctaveEnabled;
        audioCtrl->densityMinPerBeat = data_.music.densityMinPerBeat;
        audioCtrl->densityMaxPerBeat = data_.music.densityMaxPerBeat;
        audioCtrl->densityPenaltyExp = data_.music.densityPenaltyExp;
        audioCtrl->densityTarget = data_.music.densityTarget;
        audioCtrl->downwardCorrectEnabled = data_.music.downwardCorrectEnabled;
        audioCtrl->octaveCheckEnabled = data_.music.octaveCheckEnabled;
        // (phaseCheck/PLP load removed v44 — features deleted)
        audioCtrl->rayleighBpm = data_.music.rayleighBpm;
        audioCtrl->tempoNudge = data_.music.tempoNudge;
        audioCtrl->fold32Enabled = data_.music.fold32Enabled;
        audioCtrl->sesquiCheckEnabled = data_.music.sesquiCheckEnabled;
        audioCtrl->bidirectionalSnap = data_.music.bidirectionalSnap;
        // (harmonicSesqui load removed v44 — feature deleted)

        // Percival ACF harmonic pre-enhancement (v45)
        audioCtrl->percivalEnhance = data_.music.percivalEnhance;
        audioCtrl->percivalWeight2 = data_.music.percivalWeight2;
        audioCtrl->percivalWeight4 = data_.music.percivalWeight4;

        // PLL phase correction (v45)
        audioCtrl->pllEnabled = data_.music.pllEnabled;
        audioCtrl->pllKp = data_.music.pllKp;
        audioCtrl->pllKi = data_.music.pllKi;

        // Adaptive CBSS tightness (v45)
        audioCtrl->adaptiveTightnessEnabled = data_.music.adaptiveTightnessEnabled;
        audioCtrl->tightnessLowMult = data_.music.tightnessLowMult;
        audioCtrl->tightnessHighMult = data_.music.tightnessHighMult;
        audioCtrl->tightnessConfThreshHigh = data_.music.tightnessConfThreshHigh;
        audioCtrl->tightnessConfThreshLow = data_.music.tightnessConfThreshLow;

        audioCtrl->percivalWeight3 = data_.music.percivalWeight3;
        // (multiAgent/metrical/template/subbeat load removed v64 — features deleted)

        // Hidden calibration constants (v51)
        audioCtrl->cbssMeanAlpha = data_.music.cbssMeanAlpha;
        audioCtrl->harmonic2xThresh = data_.music.harmonic2xThresh;
        audioCtrl->harmonic15xThresh = data_.music.harmonic15xThresh;
        audioCtrl->pllSmoother = data_.music.pllSmoother;
        audioCtrl->beatConfBoost = data_.music.beatConfBoost;
        audioCtrl->rhythmBlend = data_.music.rhythmBlend;
        audioCtrl->periodicityBlend = data_.music.periodicityBlend;
        audioCtrl->onsetDensityBlend = data_.music.onsetDensityBlend;
        // (subbeatBins/templateHistBars load removed v64 — features deleted)
        // (nnBeatActivation load removed v68 — always on)

        // (forwardFilter/fwd*/fwdPhaseOnly load removed v64 — forward filter deleted)

        audioCtrl->btrkPipeline = data_.music.btrkPipeline;
        audioCtrl->btrkThreshWindow = data_.music.btrkThreshWindow;
        // (barPointerHmm/hmmContrast/fwdObsLambda/fwdObsFloor/fwdWrapFraction load removed v64)
        audioCtrl->octaveScoreRatio = data_.music.octaveScoreRatio;

        // (particleFilter load removed v64 — PF deleted)

        // Spectral processing (v23+)
        SharedSpectralAnalysis& spectral = audioCtrl->getSpectral();
        spectral.whitenEnabled = data_.music.whitenEnabled;
        spectral.compressorEnabled = data_.music.compressorEnabled;
        spectral.whitenBassBypass = data_.music.whitenBassBypass;
        spectral.whitenDecay = data_.music.whitenDecay;
        spectral.whitenFloor = data_.music.whitenFloor;
        spectral.compThresholdDb = data_.music.compThresholdDb;
        spectral.compRatio = data_.music.compRatio;
        spectral.compKneeDb = data_.music.compKneeDb;
        spectral.compMakeupDb = data_.music.compMakeupDb;
        spectral.compAttackTau = data_.music.compAttackTau;
        spectral.compReleaseTau = data_.music.compReleaseTau;

        // Noise estimation (v56)
        spectral.noiseEstEnabled = data_.music.noiseEstEnabled;
        spectral.noiseSmoothAlpha = data_.music.noiseSmoothAlpha;
        spectral.noiseReleaseFactor = data_.music.noiseReleaseFactor;
        spectral.noiseOversubtract = data_.music.noiseOversubtract;
        spectral.noiseFloorRatio = data_.music.noiseFloorRatio;

        // v65 params (persisted v70)
        audioCtrl->pllWarmupBeats = data_.music.pllWarmupBeats;
        audioCtrl->snapHysteresis = data_.music.snapHysteresis;
        audioCtrl->dbEmaAlpha = data_.music.dbEmaAlpha;
        audioCtrl->dbThreshold = data_.music.dbThreshold;
        audioCtrl->dbDecay = data_.music.dbDecay;

        // (BandFlux detector load removed v67 — BandFlux pipeline removed)
        // (BassSpectralAnalysis sync removed v67 — hi-res bass removed)
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
    data_.water.maxParticles = waterParams.maxParticles;

    // === LIGHTNING PARAMETERS ===
    // Spawn behavior
    data_.lightning.baseSpawnChance = lightningParams.baseSpawnChance;
    data_.lightning.audioSpawnBoost = lightningParams.audioSpawnBoost;
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
    data_.lightning.maxParticles = lightningParams.maxParticles;

    // Window/Range normalization (v72: AGC removed)
    data_.mic.peakTau = mic.peakTau;
    data_.mic.releaseTau = mic.releaseTau;

    // NOTE: Detection-specific parameters (transientThreshold, detectionMode, etc.)
    // were historically handled by EnsembleDetector (removed v67). AdaptiveMic fields
    // are kept for backward compatibility but detection is now handled by FrameBeatNN.

    // AudioController parameters (v23+)
    // All rhythm tracking params are now public tunable members
    if (audioCtrl) {
        // Basic rhythm parameters
        data_.music.bpmMin = audioCtrl->bpmMin;
        data_.music.bpmMax = audioCtrl->bpmMax;
        data_.music.activationThreshold = audioCtrl->activationThreshold;
        data_.music.cbssAlpha = audioCtrl->cbssAlpha;

        // Tempo prior width
        data_.music.tempoPriorWidth = audioCtrl->tempoPriorWidth;

        // Pulse modulation
        data_.music.pulseBoostOnBeat = audioCtrl->pulseBoostOnBeat;
        data_.music.pulseSuppressOffBeat = audioCtrl->pulseSuppressOffBeat;
        data_.music.energyBoostOnBeat = audioCtrl->energyBoostOnBeat;

        // Stability and smoothing
        data_.music.stabilityWindowBeats = audioCtrl->stabilityWindowBeats;
        data_.music.beatLookaheadMs = audioCtrl->beatLookaheadMs;
        data_.music.tempoSmoothingFactor = audioCtrl->tempoSmoothingFactor;
        data_.music.tempoChangeThreshold = audioCtrl->tempoChangeThreshold;

        // CBSS beat tracking
        data_.music.cbssTightness = audioCtrl->cbssTightness;
        data_.music.beatConfidenceDecay = audioCtrl->beatConfidenceDecay;
        data_.music.beatTimingOffset = audioCtrl->beatTimingOffset;
        data_.music.phaseCorrectionStrength = audioCtrl->phaseCorrectionStrength;
        data_.music.cbssThresholdFactor = audioCtrl->cbssThresholdFactor;
        data_.music.cbssContrast = audioCtrl->cbssContrast;
        data_.music.cbssWarmupBeats = audioCtrl->cbssWarmupBeats;
        data_.music.onsetSnapWindow = audioCtrl->onsetSnapWindow;

        // Bayesian tempo fusion (v18+)
        data_.music.bayesLambda = audioCtrl->bayesLambda;
        data_.music.bayesPriorCenter = audioCtrl->bayesPriorCenter;
        data_.music.bayesPriorWeight = audioCtrl->bayesPriorWeight;
        data_.music.bayesAcfWeight = audioCtrl->bayesAcfWeight;
        data_.music.bayesCombWeight = audioCtrl->bayesCombWeight;
        data_.music.posteriorFloor = audioCtrl->posteriorFloor;
        data_.music.disambigNudge = audioCtrl->disambigNudge;
        data_.music.harmonicTransWeight = audioCtrl->harmonicTransWeight;

        data_.music.odfSmoothWidth = audioCtrl->odfSmoothWidth;
        data_.music.octaveCheckBeats = audioCtrl->octaveCheckBeats;
        data_.music.odfMeanSubEnabled = audioCtrl->odfMeanSubEnabled;
        data_.music.beatBoundaryTempo = audioCtrl->beatBoundaryTempo;
        // (unifiedOdf/onsetTrainOdf/odfDiffMode save removed v67 — BandFlux pipeline removed)
        data_.music.adaptiveOdfThresh = audioCtrl->adaptiveOdfThresh;
        data_.music.odfThreshWindow = audioCtrl->odfThreshWindow;
        // (odfSource save removed v64 — experimental alternatives deleted)
        data_.music.densityOctaveEnabled = audioCtrl->densityOctaveEnabled;
        data_.music.densityMinPerBeat = audioCtrl->densityMinPerBeat;
        data_.music.densityMaxPerBeat = audioCtrl->densityMaxPerBeat;
        data_.music.densityPenaltyExp = audioCtrl->densityPenaltyExp;
        data_.music.densityTarget = audioCtrl->densityTarget;
        data_.music.downwardCorrectEnabled = audioCtrl->downwardCorrectEnabled;
        data_.music.octaveCheckEnabled = audioCtrl->octaveCheckEnabled;
        // (phaseCheck/PLP save removed v44 — features deleted)
        data_.music.rayleighBpm = audioCtrl->rayleighBpm;
        data_.music.tempoNudge = audioCtrl->tempoNudge;
        data_.music.fold32Enabled = audioCtrl->fold32Enabled;
        data_.music.sesquiCheckEnabled = audioCtrl->sesquiCheckEnabled;
        data_.music.bidirectionalSnap = audioCtrl->bidirectionalSnap;
        // (harmonicSesqui save removed v44 — feature deleted)

        // Percival ACF harmonic pre-enhancement (v45)
        data_.music.percivalEnhance = audioCtrl->percivalEnhance;
        data_.music.percivalWeight2 = audioCtrl->percivalWeight2;
        data_.music.percivalWeight4 = audioCtrl->percivalWeight4;

        // PLL phase correction (v45)
        data_.music.pllEnabled = audioCtrl->pllEnabled;
        data_.music.pllKp = audioCtrl->pllKp;
        data_.music.pllKi = audioCtrl->pllKi;

        // Adaptive CBSS tightness (v45)
        data_.music.adaptiveTightnessEnabled = audioCtrl->adaptiveTightnessEnabled;
        data_.music.tightnessLowMult = audioCtrl->tightnessLowMult;
        data_.music.tightnessHighMult = audioCtrl->tightnessHighMult;
        data_.music.tightnessConfThreshHigh = audioCtrl->tightnessConfThreshHigh;
        data_.music.tightnessConfThreshLow = audioCtrl->tightnessConfThreshLow;

        data_.music.percivalWeight3 = audioCtrl->percivalWeight3;
        // (multiAgent/metrical/template/subbeat save removed v64 — features deleted)

        // Hidden calibration constants (v51)
        data_.music.cbssMeanAlpha = audioCtrl->cbssMeanAlpha;
        data_.music.harmonic2xThresh = audioCtrl->harmonic2xThresh;
        data_.music.harmonic15xThresh = audioCtrl->harmonic15xThresh;
        data_.music.pllSmoother = audioCtrl->pllSmoother;
        data_.music.beatConfBoost = audioCtrl->beatConfBoost;
        data_.music.rhythmBlend = audioCtrl->rhythmBlend;
        data_.music.periodicityBlend = audioCtrl->periodicityBlend;
        data_.music.onsetDensityBlend = audioCtrl->onsetDensityBlend;
        // (subbeatBins/templateHistBars save removed v64 — features deleted)
        // (nnBeatActivation save removed v68 — always on)

        // (forwardFilter/fwd*/fwdPhaseOnly save removed v64 — forward filter deleted)

        data_.music.btrkPipeline = audioCtrl->btrkPipeline;
        data_.music.btrkThreshWindow = audioCtrl->btrkThreshWindow;
        // (barPointerHmm/hmmContrast/fwdObsLambda/fwdObsFloor/fwdWrapFraction save removed v64)
        data_.music.octaveScoreRatio = audioCtrl->octaveScoreRatio;

        // (particleFilter save removed v64 — PF deleted)

        // Spectral processing (v23+)
        const SharedSpectralAnalysis& spectral = audioCtrl->getSpectral();
        data_.music.whitenEnabled = spectral.whitenEnabled;
        data_.music.compressorEnabled = spectral.compressorEnabled;
        data_.music.whitenBassBypass = spectral.whitenBassBypass;
        data_.music.whitenDecay = spectral.whitenDecay;
        data_.music.whitenFloor = spectral.whitenFloor;
        data_.music.compThresholdDb = spectral.compThresholdDb;
        data_.music.compRatio = spectral.compRatio;
        data_.music.compKneeDb = spectral.compKneeDb;
        data_.music.compMakeupDb = spectral.compMakeupDb;
        data_.music.compAttackTau = spectral.compAttackTau;
        data_.music.compReleaseTau = spectral.compReleaseTau;

        // Noise estimation (v56)
        data_.music.noiseEstEnabled = spectral.noiseEstEnabled;
        data_.music.noiseSmoothAlpha = spectral.noiseSmoothAlpha;
        data_.music.noiseReleaseFactor = spectral.noiseReleaseFactor;
        data_.music.noiseOversubtract = spectral.noiseOversubtract;
        data_.music.noiseFloorRatio = spectral.noiseFloorRatio;

        // v65 params (persisted v70)
        data_.music.pllWarmupBeats = audioCtrl->pllWarmupBeats;
        data_.music.snapHysteresis = audioCtrl->snapHysteresis;
        data_.music.dbEmaAlpha = audioCtrl->dbEmaAlpha;
        data_.music.dbThreshold = audioCtrl->dbThreshold;
        data_.music.dbDecay = audioCtrl->dbDecay;

        // (BandFlux detector save removed v67 — BandFlux pipeline removed)
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
