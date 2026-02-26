#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../audio/AudioController.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../config/DeviceConfigLoader.h"  // v28: Runtime device config loading
#include "../types/Version.h"
#include "../render/RenderPipeline.h"
#include "../effects/HueRotationEffect.h"
#include <ArduinoJson.h>  // v28: JSON parsing for device config upload

extern DeviceConfig config;  // v28: Changed to non-const for runtime loading

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

// Static debug channels - default to NONE (no debug output)
DebugChannel SerialConsole::debugChannels_ = DebugChannel::NONE;

// Callback for marking config dirty when parameters change
void onParamChanged() {
    if (SerialConsole::instance_) {
        ConfigStorage* storage = SerialConsole::instance_->getConfigStorage();
        if (storage) {
            storage->markDirty();
        }

        // CRITICAL: Update force adapters when wind/gravity/drag params change
        // The force adapter caches wind values via setWind(), so we must re-sync them
        Fire* fireGen = SerialConsole::instance_->fireGenerator_;
        Water* waterGen = SerialConsole::instance_->waterGenerator_;

        if (fireGen) {
            fireGen->syncPhysicsParams();
        }
        if (waterGen) {
            waterGen->syncPhysicsParams();
        }
    }
}

// File-scope storage for effect settings (accessible from both register and sync functions)
static float effectHueShift_ = 0.0f;
static float effectRotationSpeed_ = 0.0f;

// New constructor with RenderPipeline
SerialConsole::SerialConsole(RenderPipeline* pipeline, AdaptiveMic* mic)
    : pipeline_(pipeline), fireGenerator_(nullptr), waterGenerator_(nullptr),
      lightningGenerator_(nullptr), audioVisGenerator_(nullptr), hueEffect_(nullptr), mic_(mic),
      battery_(nullptr), audioCtrl_(nullptr), configStorage_(nullptr) {
    instance_ = this;
    // Get generator pointers from pipeline
    if (pipeline_) {
        fireGenerator_ = pipeline_->getFireGenerator();
        waterGenerator_ = pipeline_->getWaterGenerator();
        lightningGenerator_ = pipeline_->getLightningGenerator();
        audioVisGenerator_ = pipeline_->getAudioVisGenerator();
        hueEffect_ = pipeline_->getHueRotationEffect();
    }
}

void SerialConsole::begin() {
    // Note: Serial.begin() should be called by main setup() before this
    settings_.begin();
    registerSettings();

    Serial.println(F("Serial console ready."));
}

void SerialConsole::registerSettings() {
    // Get direct pointers to the fire generator's params
    FireParams* fp = nullptr;
    if (fireGenerator_) {
        fp = &fireGenerator_->getParamsMutable();
    }

    // Register all settings by category
    registerFireSettings(fp);
    registerFireMusicSettings(fp);
    registerFireOrganicSettings(fp);

    // Register Water generator settings (use mutable ref so changes apply directly)
    if (waterGenerator_) {
        registerWaterSettings(&waterGenerator_->getParamsMutable());
    }

    // Register Lightning generator settings (use mutable ref so changes apply directly)
    if (lightningGenerator_) {
        registerLightningSettings(&lightningGenerator_->getParamsMutable());
    }

    // Register Audio visualization generator settings
    if (audioVisGenerator_) {
        registerAudioVisSettings(&audioVisGenerator_->getParamsMutable());
    }

    // Register effect settings (HueRotation)
    registerEffectSettings();

    // Audio settings
    registerAudioSettings();
    registerAgcSettings();
    registerTransientSettings();
    registerDetectionSettings();
    registerEnsembleSettings();  // New: Ensemble detector configuration
    registerRhythmSettings();
}

// === FIRE SETTINGS (Particle-based) ===
void SerialConsole::registerFireSettings(FireParams* fp) {
    if (!fp) return;

    // Spawn behavior
    settings_.registerFloat("basespawnchance", &fp->baseSpawnChance, "fire",
        "Baseline spark spawn probability", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("audiospawnboost", &fp->audioSpawnBoost, "fire",
        "Audio reactivity multiplier", 0.0f, 2.0f, onParamChanged);
    settings_.registerUint8("burstsparks", &fp->burstSparks, "fire",
        "Sparks per beat burst", 1, 20, onParamChanged);

    // Physics
    settings_.registerFloat("gravity", &fp->gravity, "fire",
        "Gravity strength (negative=upward)", -200.0f, 200.0f, onParamChanged);
    settings_.registerFloat("windbase", &fp->windBase, "fire",
        "Base wind force", -50.0f, 50.0f, onParamChanged);
    settings_.registerFloat("windvariation", &fp->windVariation, "fire",
        "Wind variation amount", 0.0f, 100.0f, onParamChanged);
    settings_.registerFloat("drag", &fp->drag, "fire",
        "Drag coefficient", 0.0f, 1.0f, onParamChanged);

    // Spark appearance
    settings_.registerFloat("sparkvelmin", &fp->sparkVelocityMin, "fire",
        "Minimum upward velocity", 0.0f, 100.0f, onParamChanged);
    settings_.registerFloat("sparkvelmax", &fp->sparkVelocityMax, "fire",
        "Maximum upward velocity", 0.0f, 100.0f, onParamChanged);
    settings_.registerFloat("sparkspread", &fp->sparkSpread, "fire",
        "Horizontal velocity spread", 0.0f, 50.0f, onParamChanged);

    // Lifecycle
    settings_.registerUint8("maxparticles", &fp->maxParticles, "fire",
        "Maximum active particles", 1, 64, onParamChanged);
    settings_.registerUint8("defaultlifespan", &fp->defaultLifespan, "fire",
        "Default particle lifespan (centiseconds, 100=1s)", 1, 255, onParamChanged);
    settings_.registerUint8("intensitymin", &fp->intensityMin, "fire",
        "Minimum spawn intensity", 0, 255, onParamChanged);
    settings_.registerUint8("intensitymax", &fp->intensityMax, "fire",
        "Maximum spawn intensity", 0, 255, onParamChanged);

    // Background
    settings_.registerFloat("bgintensity", &fp->backgroundIntensity, "fire",
        "Noise background brightness", 0.0f, 1.0f, onParamChanged);

    // Particle variety
    settings_.registerFloat("fastsparks", &fp->fastSparkRatio, "fire",
        "Fast spark ratio (0=all embers, 1=all sparks)", 0.0f, 1.0f, onParamChanged);

    // Thermal physics
    settings_.registerFloat("thermalforce", &fp->thermalForce, "fire",
        "Thermal buoyancy strength (LEDs/sec^2)", 0.0f, 200.0f, onParamChanged);
}

// === MUSIC MODE FIRE SETTINGS ===
// Controls fire behavior when music mode is active (beat-synced)
void SerialConsole::registerFireMusicSettings(FireParams* fp) {
    if (!fp) return;

    settings_.registerFloat("musicspawnpulse", &fp->musicSpawnPulse, "firemusic",
        "Beat spawn depth (0=flat, 1=full breathing)", 0.0f, 1.0f, onParamChanged);
}

// === ORGANIC MODE FIRE SETTINGS ===
// Controls fire behavior when music mode is NOT active
void SerialConsole::registerFireOrganicSettings(FireParams* fp) {
    if (!fp) return;

    settings_.registerFloat("organictransmin", &fp->organicTransientMin, "fireorganic",
        "Min transient to trigger burst", 0.0f, 1.0f, onParamChanged);
}

// === AUDIO SETTINGS ===
// Window/Range normalization: peak/valley tracking adapts to signal
void SerialConsole::registerAudioSettings() {
    if (!mic_) return;

    settings_.registerFloat("peaktau", &mic_->peakTau, "audio",
        "Peak adaptation speed (s)", 0.5f, 10.0f);
    settings_.registerFloat("releasetau", &mic_->releaseTau, "audio",
        "Peak release speed (s)", 1.0f, 30.0f);
}

// === HARDWARE AGC SETTINGS ===
// Signal flow: Mic → HW Gain (PRIMARY) → ADC → Window/Range (SECONDARY) → Output
void SerialConsole::registerAgcSettings() {
    if (!mic_) return;

    settings_.registerFloat("hwtarget", &mic_->hwTarget, "agc",
        "HW target level (raw, ±0.01 dead zone)", 0.05f, 0.9f);
    settings_.registerBool("fastagc", &mic_->fastAgcEnabled, "agc",
        "Enable fast AGC for low-level sources");
    settings_.registerFloat("fastagcthresh", &mic_->fastAgcThreshold, "agc",
        "Raw level threshold for fast AGC", 0.05f, 0.3f);
    settings_.registerUint16("fastagcperiod", &mic_->fastAgcPeriodMs, "agc",
        "Fast AGC calibration period (ms)", 2000, 15000);
    settings_.registerFloat("fastagctau", &mic_->fastAgcTrackingTau, "agc",
        "Fast AGC tracking time (s)", 1.0f, 15.0f);
}

// === TRANSIENT DETECTION SETTINGS ===
// NOTE: Transient detection has been moved to EnsembleDetector
// These settings are now controlled via the ensemble configuration
void SerialConsole::registerTransientSettings() {
    // Settings moved to EnsembleDetector
    // Use 'show detectors' command to view/configure detector settings
}

// === DETECTION MODE SETTINGS ===
// NOTE: Detection modes replaced by ensemble architecture
// All 6 detectors run simultaneously with weighted fusion
void SerialConsole::registerDetectionSettings() {
    // Legacy detection mode settings removed
    // Use ensemble configuration via:
    //   set detector_enable <detector> <0|1>
    //   set detector_weight <detector> <weight>
    //   set detector_thresh <detector> <threshold>
}

// === ENSEMBLE DETECTOR SETTINGS ===
// New ensemble-based detection system with 6 concurrent detectors
// Detector-specific parameters are accessed via "show" and "set" commands in handleEnsembleCommand()
// Common parameters (weight, threshold, enabled) use setDetectorEnabled/Weight/Threshold
void SerialConsole::registerEnsembleSettings() {
    // Detector-specific parameters handled via handleEnsembleCommand()
    // See: set drummer_attackmult, set spectral_minbin, etc.
}

// === RHYTHM TRACKING SETTINGS (AudioController) ===
void SerialConsole::registerRhythmSettings() {
    if (!audioCtrl_) return;

    // Onset strength signal (OSS) generation
    settings_.registerFloat("ossfluxweight", &audioCtrl_->ossFluxWeight, "rhythm",
        "OSS flux weight (1=flux, 0=RMS)", 0.0f, 1.0f);
    settings_.registerBool("adaptivebandweight", &audioCtrl_->adaptiveBandWeightEnabled, "rhythm",
        "Enable adaptive band weighting");
    settings_.registerBool("combbankenabled", &audioCtrl_->combBankEnabled, "rhythm",
        "Enable comb filter bank for tempo validation");
    settings_.registerFloat("combbankfeedback", &audioCtrl_->combBankFeedback, "rhythm",
        "Comb bank resonance (0.85-0.98)", 0.85f, 0.98f);
    // (combxvalconf/combxvalcorr removed — comb bank feeds Bayesian fusion directly)

    // CBSS beat tracking parameters
    settings_.registerFloat("cbssalpha", &audioCtrl_->cbssAlpha, "rhythm",
        "CBSS weighting (0.8-0.95, higher=more predictive)", 0.5f, 0.99f);
    settings_.registerFloat("cbsstight", &audioCtrl_->cbssTightness, "rhythm",
        "CBSS log-Gaussian tightness (higher=stricter tempo)", 1.0f, 20.0f);
    settings_.registerFloat("beatconfdecay", &audioCtrl_->beatConfidenceDecay, "rhythm",
        "Beat confidence decay per frame", 0.9f, 0.999f);
    // (temposnap removed — Bayesian fusion handles tempo transitions)
    settings_.registerFloat("beatoffset", &audioCtrl_->beatTimingOffset, "rhythm",
        "Beat prediction advance in frames (ODF+CBSS delay compensation)", 0.0f, 15.0f);
    settings_.registerFloat("phasecorr", &audioCtrl_->phaseCorrectionStrength, "rhythm",
        "Phase correction toward transients (0=off, 1=full snap)", 0.0f, 1.0f);
    settings_.registerFloat("cbssthresh", &audioCtrl_->cbssThresholdFactor, "rhythm",
        "CBSS adaptive threshold factor (0=off, beat fires only if CBSS > factor*mean)", 0.0f, 2.0f);
    settings_.registerFloat("temposmooth", &audioCtrl_->tempoSmoothingFactor, "rhythm",
        "Tempo EMA smoothing (0.5=fast, 0.99=slow)", 0.5f, 0.99f);
    settings_.registerUint8("odfsmooth", &audioCtrl_->odfSmoothWidth, "rhythm",
        "ODF smooth window (3-11, odd)", 3, 11);
    settings_.registerBool("ioi", &audioCtrl_->ioiEnabled, "rhythm",
        "IOI histogram observation in Bayesian fusion");
    settings_.registerBool("odfmeansub", &audioCtrl_->odfMeanSubEnabled, "rhythm",
        "ODF mean subtraction before autocorrelation (BTrack-style detrending)");
    settings_.registerBool("ft", &audioCtrl_->ftEnabled, "rhythm",
        "Fourier tempogram observation in Bayesian fusion");

    // Bayesian tempo fusion weights (v18+)
    settings_.registerFloat("bayeslambda", &audioCtrl_->bayesLambda, "bayesian",
        "Transition tightness (0.01=rigid, 1.0=loose)", 0.01f, 1.0f);
    settings_.registerFloat("bayesprior", &audioCtrl_->bayesPriorCenter, "bayesian",
        "Static prior center BPM", 60.0f, 200.0f);
    settings_.registerFloat("bayespriorw", &audioCtrl_->bayesPriorWeight, "bayesian",
        "Ongoing static prior strength (0=off, 1=std, 2=strong)", 0.0f, 3.0f);
    settings_.registerFloat("bayesacf", &audioCtrl_->bayesAcfWeight, "bayesian",
        "Autocorrelation observation weight", 0.0f, 2.0f);
    settings_.registerFloat("bayesft", &audioCtrl_->bayesFtWeight, "bayesian",
        "Fourier tempogram observation weight", 0.0f, 2.0f);
    settings_.registerFloat("bayescomb", &audioCtrl_->bayesCombWeight, "bayesian",
        "Comb filter bank observation weight", 0.0f, 2.0f);
    settings_.registerFloat("bayesioi", &audioCtrl_->bayesIoiWeight, "bayesian",
        "IOI histogram observation weight", 0.0f, 2.0f);

    // Ensemble fusion parameters (detection gating)
    settings_.registerUint16("enscooldown", &audioCtrl_->getEnsemble().getFusion().cooldownMs, "ensemble",
        "Base ensemble cooldown (ms)", 20, 500);
    settings_.registerFloat("ensminconf", &audioCtrl_->getEnsemble().getFusion().minConfidence, "ensemble",
        "Minimum detector confidence", 0.0f, 1.0f);
    settings_.registerFloat("ensminlevel", &audioCtrl_->getEnsemble().getFusion().minAudioLevel, "ensemble",
        "Noise gate audio level", 0.0f, 0.5f);
    // Note: Adaptive cooldown enable/disable handled via "set ens_adaptcool 0|1" command
    // Effective cooldown (tempo-adjusted) shown via "show ens_effcool" command

    // Basic rhythm activation and output modulation
    settings_.registerFloat("musicthresh", &audioCtrl_->activationThreshold, "rhythm",
        "Rhythm activation threshold (0-1)", 0.0f, 1.0f);
    settings_.registerFloat("pulseboost", &audioCtrl_->pulseBoostOnBeat, "rhythm",
        "Pulse boost on beat", 1.0f, 2.0f);
    settings_.registerFloat("pulsesuppress", &audioCtrl_->pulseSuppressOffBeat, "rhythm",
        "Pulse suppress off beat", 0.3f, 1.0f);
    settings_.registerFloat("energyboost", &audioCtrl_->energyBoostOnBeat, "rhythm",
        "Energy boost on beat", 0.0f, 1.0f);
    settings_.registerFloat("bpmmin", &audioCtrl_->bpmMin, "rhythm",
        "Minimum BPM to detect", 40.0f, 120.0f);
    settings_.registerFloat("bpmmax", &audioCtrl_->bpmMax, "rhythm",
        "Maximum BPM to detect", 80.0f, 240.0f);

    // Autocorrelation timing
    settings_.registerUint16("autocorrperiod", &audioCtrl_->autocorrPeriodMs, "rhythm",
        "Autocorr period (ms)", 100, 1000);

    // Band weights (used when adaptive weighting disabled)
    settings_.registerBool("adaptbandweight", &audioCtrl_->adaptiveBandWeightEnabled, "rhythm",
        "Enable adaptive band weighting");
    settings_.registerFloat("bassbandweight", &audioCtrl_->bassBandWeight, "rhythm",
        "Bass band weight", 0.0f, 1.0f);
    settings_.registerFloat("midbandweight", &audioCtrl_->midBandWeight, "rhythm",
        "Mid band weight", 0.0f, 1.0f);
    settings_.registerFloat("highbandweight", &audioCtrl_->highBandWeight, "rhythm",
        "High band weight", 0.0f, 1.0f);

    // Tempo prior width (used by Bayesian static prior initialization)
    settings_.registerFloat("priorwidth", &audioCtrl_->tempoPriorWidth, "bayesian",
        "Prior width (sigma BPM)", 10.0f, 80.0f);

    // Beat stability tracking
    settings_.registerFloat("stabilitywin", &audioCtrl_->stabilityWindowBeats, "stability",
        "Stability window (beats)", 4.0f, 16.0f);

    // Beat lookahead (anticipatory effects)
    settings_.registerFloat("lookahead", &audioCtrl_->beatLookaheadMs, "lookahead",
        "Beat lookahead (ms)", 0.0f, 200.0f);

    // Continuous tempo estimation
    settings_.registerFloat("tempochgthresh", &audioCtrl_->tempoChangeThreshold, "tempo",
        "Tempo change threshold", 0.01f, 0.5f);
    // (maxbpmchg removed — Bayesian fusion handles tempo stability)

    // Spectral processing (whitening + compressor)
    SharedSpectralAnalysis& spectral = audioCtrl_->getEnsemble().getSpectral();
    settings_.registerBool("whitenenabled", &spectral.whitenEnabled, "spectral",
        "Per-bin spectral whitening");
    settings_.registerFloat("whitendecay", &spectral.whitenDecay, "spectral",
        "Whitening peak decay per frame (0.99-0.999)", 0.9f, 0.9999f);
    settings_.registerFloat("whitenfloor", &spectral.whitenFloor, "spectral",
        "Whitening noise floor", 0.0001f, 0.1f);
    settings_.registerBool("compenabled", &spectral.compressorEnabled, "spectral",
        "Soft-knee compressor");
    settings_.registerFloat("compthresh", &spectral.compThresholdDb, "spectral",
        "Compressor threshold (dB)", -60.0f, 0.0f);
    settings_.registerFloat("compratio", &spectral.compRatio, "spectral",
        "Compression ratio", 1.0f, 20.0f);
    settings_.registerFloat("compknee", &spectral.compKneeDb, "spectral",
        "Soft knee width (dB)", 0.0f, 30.0f);
    settings_.registerFloat("compmakeup", &spectral.compMakeupDb, "spectral",
        "Makeup gain (dB)", -10.0f, 30.0f);
    settings_.registerFloat("compattack", &spectral.compAttackTau, "spectral",
        "Attack time constant (s)", 0.0001f, 0.1f);
    settings_.registerFloat("comprelease", &spectral.compReleaseTau, "spectral",
        "Release time constant (s)", 0.01f, 10.0f);
}

void SerialConsole::update() {
    // Handle incoming commands
    if (Serial.available()) {
        // Buffer must accommodate full device config JSON (~550 bytes)
        static char buf[768];
        size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
        // Explicit bounds check for safety
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        buf[len] = '\0';
        // Trim CR/LF
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) {
            buf[--len] = '\0';
        }
        if (len > 0) {
            handleCommand(buf);
        }
    }

    // JSON streaming for web app
    streamTick();
}

void SerialConsole::handleCommand(const char* cmd) {
    // Check for ensemble/detector commands FIRST (before settings registry)
    // These use "set detector_*" and "set agree_*" which conflict with registry
    if (handleEnsembleCommand(cmd)) {
        return;
    }

    // Check for beat tracking commands
    if (handleBeatTrackingCommand(cmd)) {
        return;
    }

    // Try settings registry (handles set/get/show/list/categories/settings)
    if (settings_.handleCommand(cmd)) {
        // Sync effect settings to actual effect after any settings change
        syncEffectSettings();
        return;
    }

    // Then try special commands (JSON API, config management)
    if (handleSpecialCommand(cmd)) {
        return;
    }

    Serial.println(F("Unknown command. Try 'settings' for help."));
}

bool SerialConsole::handleSpecialCommand(const char* cmd) {
    // Dispatch to specialized handlers (order matters for prefix matching)
    // NOTE: handleEnsembleCommand and handleBeatTrackingCommand are called
    // BEFORE settings registry in handleCommand() to avoid "set" conflicts
    if (handleJsonCommand(cmd)) return true;
    if (handleGeneratorCommand(cmd)) return true;
    if (handleEffectCommand(cmd)) return true;
    if (handleBatteryCommand(cmd)) return true;
    if (handleStreamCommand(cmd)) return true;
    if (handleTestCommand(cmd)) return true;
    if (handleAudioStatusCommand(cmd)) return true;
    if (handleModeCommand(cmd)) return true;
    if (handleConfigCommand(cmd)) return true;
    if (handleDeviceConfigCommand(cmd)) return true;  // Device config commands (v28+)
    if (handleLogCommand(cmd)) return true;
    if (handleDebugCommand(cmd)) return true;     // Debug channel commands
    return false;
}

// === JSON API COMMANDS (for web app) ===
bool SerialConsole::handleJsonCommand(const char* cmd) {
    // Handle "json settings" or "json settings <category>"
    if (strncmp(cmd, "json settings", 13) == 0) {
        const char* category = cmd + 13;
        while (*category == ' ') category++;  // Skip whitespace

        if (*category == '\0') {
            settings_.printSettingsJson();    // All settings
        } else {
            settings_.printSettingsCategoryJson(category);  // Category only
        }
        return true;
    }

    if (strcmp(cmd, "json info") == 0) {
        Serial.print(F("{\"version\":\""));
        Serial.print(F(BLINKY_VERSION_STRING));
        Serial.print(F("\""));

        // Device configuration status (v28+)
        if (configStorage_ && configStorage_->isDeviceConfigValid()) {
            const ConfigStorage::StoredDeviceConfig& cfg = configStorage_->getDeviceConfig();
            Serial.print(F(",\"device\":{\"id\":\""));
            Serial.print(cfg.deviceId);
            Serial.print(F("\",\"name\":\""));
            Serial.print(cfg.deviceName);
            Serial.print(F("\",\"width\":"));
            Serial.print(cfg.ledWidth);
            Serial.print(F(",\"height\":"));
            Serial.print(cfg.ledHeight);
            Serial.print(F(",\"leds\":"));
            Serial.print(cfg.ledWidth * cfg.ledHeight);
            Serial.print(F(",\"configured\":true}"));
        } else {
            Serial.print(F(",\"device\":{\"configured\":false,\"safeMode\":true}"));
        }

        Serial.println(F("}"));
        return true;
    }

    if (strcmp(cmd, "json state") == 0) {
        if (!pipeline_) {
            Serial.println(F("{\"error\":\"Pipeline not available\"}"));
            return true;
        }
        Serial.print(F("{\"generator\":\""));
        Serial.print(pipeline_->getGeneratorName());
        Serial.print(F("\",\"effect\":\""));
        Serial.print(pipeline_->getEffectName());
        Serial.print(F("\",\"generators\":["));
        for (int i = 0; i < RenderPipeline::NUM_GENERATORS; i++) {
            if (i > 0) Serial.print(',');
            Serial.print('"');
            Serial.print(RenderPipeline::getGeneratorNameByIndex(i));
            Serial.print('"');
        }
        Serial.print(F("],\"effects\":["));
        for (int i = 0; i < RenderPipeline::NUM_EFFECTS; i++) {
            if (i > 0) Serial.print(',');
            Serial.print('"');
            Serial.print(RenderPipeline::getEffectNameByIndex(i));
            Serial.print('"');
        }
        Serial.println(F("]}"));
        return true;
    }

    return false;
}

// === BATTERY COMMANDS ===
bool SerialConsole::handleBatteryCommand(const char* cmd) {
    if (strcmp(cmd, "battery debug") == 0 || strcmp(cmd, "batt debug") == 0) {
        if (battery_) {
            Serial.println(F("=== Battery Debug Info ==="));
            Serial.print(F("Connected: "));
            Serial.println(battery_->isBatteryConnected() ? F("Yes") : F("No"));
            Serial.print(F("Voltage: "));
            Serial.print(battery_->getVoltage(), 3);
            Serial.println(F("V"));
            Serial.print(F("Percent: "));
            Serial.print(battery_->getPercent());
            Serial.println(F("%"));
            Serial.print(F("Charging: "));
            Serial.println(battery_->isCharging() ? F("Yes") : F("No"));
            Serial.println(F("(Use 'battery raw' for detailed ADC values)"));
        } else {
            Serial.println(F("Battery monitor not available"));
        }
        return true;
    }

    if (strcmp(cmd, "battery") == 0 || strcmp(cmd, "batt") == 0) {
        if (battery_) {
            float voltage = battery_->getVoltage();
            uint8_t percent = battery_->getPercent();
            bool charging = battery_->isCharging();
            bool connected = battery_->isBatteryConnected();

            Serial.print(F("{\"battery\":{"));
            Serial.print(F("\"voltage\":"));
            Serial.print(voltage, 2);
            Serial.print(F(",\"percent\":"));
            Serial.print(percent);
            Serial.print(F(",\"charging\":"));
            Serial.print(charging ? F("true") : F("false"));
            Serial.print(F(",\"connected\":"));
            Serial.print(connected ? F("true") : F("false"));
            Serial.println(F("}}"));
        } else {
            Serial.println(F("{\"error\":\"Battery monitor not available\"}"));
        }
        return true;
    }

    return false;
}

// === STREAM COMMANDS ===
bool SerialConsole::handleStreamCommand(const char* cmd) {
    if (strcmp(cmd, "stream on") == 0) {
        streamEnabled_ = true;
        Serial.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "stream off") == 0) {
        streamEnabled_ = false;
        Serial.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "stream debug") == 0) {
        streamEnabled_ = true;
        streamDebug_ = true;
        Serial.println(F("OK debug"));
        return true;
    }

    if (strcmp(cmd, "stream normal") == 0) {
        streamDebug_ = false;
        streamFast_ = false;
        Serial.println(F("OK normal"));
        return true;
    }

    if (strcmp(cmd, "stream fast") == 0) {
        streamEnabled_ = true;
        streamFast_ = true;
        Serial.println(F("OK fast"));
        return true;
    }

    return false;
}

// === TEST MODE COMMANDS ===
bool SerialConsole::handleTestCommand(const char* cmd) {
    if (strncmp(cmd, "test lock hwgain", 16) == 0) {
        // Ensure command is exact match or followed by space (not "test lock hwgainXYZ")
        if (cmd[16] != '\0' && cmd[16] != ' ') {
            return false;
        }
        if (!mic_) {
            Serial.println(F("ERROR: Microphone not available"));
            return true;
        }
        // Parse optional gain value (default to current gain)
        int gain = mic_->getHwGain();
        if (strlen(cmd) > 17) {
            gain = atoi(cmd + 17);
            if (gain < 0 || gain > 80) {
                Serial.print(F("WARNING: Gain "));
                Serial.print(gain);
                Serial.println(F(" out of range (0-80), will be clamped"));
            }
        }
        mic_->lockHwGain(gain);
        Serial.print(F("OK locked at "));
        Serial.println(mic_->getHwGain());
        return true;
    }

    if (strcmp(cmd, "test unlock hwgain") == 0) {
        if (!mic_) {
            Serial.println(F("ERROR: Microphone not available"));
            return true;
        }
        mic_->unlockHwGain();
        Serial.println(F("OK unlocked"));
        return true;
    }

    return false;
}

// === AUDIO CONTROLLER STATUS ===
bool SerialConsole::handleAudioStatusCommand(const char* cmd) {
    if (strcmp(cmd, "music") == 0 || strcmp(cmd, "rhythm") == 0 || strcmp(cmd, "audio") == 0) {
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();
            Serial.println(F("=== Audio Controller Status ==="));
            Serial.print(F("Rhythm Active: "));
            Serial.println(audio.rhythmStrength > audioCtrl_->activationThreshold ? F("YES") : F("NO"));
            Serial.print(F("BPM: "));
            Serial.println(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F("Phase: "));
            Serial.println(audio.phase, 2);
            Serial.print(F("Rhythm Strength: "));
            Serial.println(audio.rhythmStrength, 2);
            Serial.print(F("Periodicity: "));
            Serial.println(audioCtrl_->getPeriodicityStrength(), 2);
            Serial.print(F("Energy: "));
            Serial.println(audio.energy, 2);
            Serial.print(F("Pulse: "));
            Serial.println(audio.pulse, 2);
            Serial.print(F("Onset Density: "));
            Serial.print(audio.onsetDensity, 1);
            Serial.println(F(" /s"));
            Serial.print(F("BPM Range: "));
            Serial.print(audioCtrl_->getBpmMin(), 0);
            Serial.print(F("-"));
            Serial.println(audioCtrl_->getBpmMax(), 0);

            // New metrics from research-based improvements
            Serial.println(F("--- Advanced Metrics ---"));
            Serial.print(F("Beat Stability: "));
            Serial.println(audioCtrl_->getBeatStability(), 2);
            Serial.print(F("Tempo Velocity: "));
            Serial.print(audioCtrl_->getTempoVelocity(), 1);
            Serial.println(F(" BPM/s"));
            Serial.print(F("Next Beat In: "));
            uint32_t nowMs = millis();
            uint32_t nextMs = audioCtrl_->getNextBeatMs();
            Serial.print(nextMs > nowMs ? (nextMs - nowMs) : 0);
            Serial.println(F(" ms"));
            Serial.print(F("Bayesian Prior Center: "));
            Serial.print(audioCtrl_->bayesPriorCenter, 0);
            Serial.print(F(" BPM (best bin conf="));
            Serial.print(audioCtrl_->getBayesBestConf(), 2);
            Serial.println(F(")"));
        } else {
            Serial.println(F("Audio controller not available"));
        }
        return true;
    }

    return false;
}

// === DETECTION MODE STATUS ===
bool SerialConsole::handleModeCommand(const char* cmd) {
    if (strcmp(cmd, "mode") == 0) {
        Serial.println(F("=== Ensemble Detection Status ==="));
        if (audioCtrl_) {
            const EnsembleOutput& output = audioCtrl_->getLastEnsembleOutput();
            Serial.print(F("Transient Strength: "));
            Serial.println(output.transientStrength, 3);
            Serial.print(F("Ensemble Confidence: "));
            Serial.println(output.ensembleConfidence, 3);
            Serial.print(F("Detector Agreement: "));
            Serial.print(output.detectorAgreement);
            Serial.println(F("/7"));
            Serial.print(F("Dominant Detector: "));
            Serial.println(output.dominantDetector);
        } else {
            Serial.println(F("AudioController not available"));
        }
        if (mic_) {
            Serial.print(F("Audio Level: "));
            Serial.println(mic_->getLevel(), 3);
            Serial.print(F("Hardware Gain: "));
            Serial.println(mic_->getHwGain());
        }
        return true;
    }

    return false;
}

// === CONFIGURATION COMMANDS ===
bool SerialConsole::handleConfigCommand(const char* cmd) {
    if (strcmp(cmd, "save") == 0) {
        if (configStorage_ && fireGenerator_ && waterGenerator_ && lightningGenerator_ && mic_) {
            configStorage_->saveConfiguration(
                fireGenerator_->getParams(),
                waterGenerator_->getParams(),
                lightningGenerator_->getParams(),
                *mic_,
                audioCtrl_
            );
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "load") == 0) {
        if (configStorage_ && fireGenerator_ && waterGenerator_ && lightningGenerator_ && mic_) {
            configStorage_->loadConfiguration(
                fireGenerator_->getParamsMutable(),
                waterGenerator_->getParamsMutable(),
                lightningGenerator_->getParamsMutable(),
                *mic_,
                audioCtrl_
            );
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
        Serial.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "reset") == 0 || strcmp(cmd, "factory") == 0) {
        if (configStorage_) {
            configStorage_->factoryReset();
            restoreDefaults();
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "reboot") == 0) {
        Serial.println(F("Rebooting..."));
        Serial.flush();  // Ensure message is sent before reset
        delay(100);      // Brief delay for serial transmission
        NVIC_SystemReset();
        return true;  // Never reached
    }

    if (strcmp(cmd, "bootloader") == 0) {
#ifdef ARDUINO_ARCH_NRF52
        Serial.println(F("Entering UF2 bootloader..."));
        Serial.flush();  // Ensure message is sent before reset
        delay(100);      // Brief delay for serial transmission
        // Use SoftDevice API for GPREGRET when SoftDevice is enabled.
        // Direct NRF_POWER->GPREGRET writes are unreliable when SoftDevice
        // owns the POWER peripheral (register gets cleared during reset).
        {
            const uint8_t DFU_MAGIC_UF2 = 0x57;
            uint8_t sd_en = 0;
            sd_softdevice_is_enabled(&sd_en);
            if (sd_en) {
                sd_power_gpregret_clr(0, 0xFF);
                sd_power_gpregret_set(0, DFU_MAGIC_UF2);
            } else {
                NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
            }
        }
        NVIC_SystemReset();
#else
        Serial.println(F("UF2 bootloader not available on this platform"));
#endif
        return true;
    }

    return false;
}

// === DEVICE CONFIGURATION COMMANDS (v28+) ===
bool SerialConsole::handleDeviceConfigCommand(const char* cmd) {
    if (strcmp(cmd, "device show") == 0 || strcmp(cmd, "device") == 0) {
        showDeviceConfig();
        return true;
    }

    if (strncmp(cmd, "device upload ", 14) == 0) {
        uploadDeviceConfig(cmd + 14);
        return true;
    }

    Serial.println(F("Device configuration commands:"));
    Serial.println(F("  device show          - Display current device config"));
    Serial.println(F("  device upload <JSON> - Upload device config from JSON"));
    Serial.println(F("\nExample JSON at: devices/registry/README.md"));
    return false;
}

void SerialConsole::showDeviceConfig() {
    if (!configStorage_) {
        Serial.println(F("{\"error\":\"ConfigStorage not available\"}"));
        return;
    }

    if (!configStorage_->isDeviceConfigValid()) {
        Serial.println(F("{\"error\":\"No device config\",\"status\":\"unconfigured\",\"safeMode\":true}"));
        return;
    }

    const ConfigStorage::StoredDeviceConfig& cfg = configStorage_->getDeviceConfig();

    // Use ArduinoJson for clean, maintainable JSON serialization
    JsonDocument doc;

    // Device identification
    doc["deviceId"] = cfg.deviceId;
    doc["deviceName"] = cfg.deviceName;

    // Matrix/LED configuration
    doc["ledWidth"] = cfg.ledWidth;
    doc["ledHeight"] = cfg.ledHeight;
    doc["ledPin"] = cfg.ledPin;
    doc["brightness"] = cfg.brightness;
    doc["ledType"] = cfg.ledType;
    doc["orientation"] = cfg.orientation;
    doc["layoutType"] = cfg.layoutType;

    // Charging configuration
    doc["fastChargeEnabled"] = cfg.fastChargeEnabled;
    doc["lowBatteryThreshold"] = serialized(String(cfg.lowBatteryThreshold, 2));
    doc["criticalBatteryThreshold"] = serialized(String(cfg.criticalBatteryThreshold, 2));
    doc["minVoltage"] = serialized(String(cfg.minVoltage, 2));
    doc["maxVoltage"] = serialized(String(cfg.maxVoltage, 2));

    // IMU configuration
    doc["upVectorX"] = serialized(String(cfg.upVectorX, 2));
    doc["upVectorY"] = serialized(String(cfg.upVectorY, 2));
    doc["upVectorZ"] = serialized(String(cfg.upVectorZ, 2));
    doc["rotationDegrees"] = serialized(String(cfg.rotationDegrees, 2));
    doc["invertZ"] = cfg.invertZ;
    doc["swapXY"] = cfg.swapXY;
    doc["invertX"] = cfg.invertX;
    doc["invertY"] = cfg.invertY;

    // Serial configuration
    doc["baudRate"] = cfg.baudRate;
    doc["initTimeoutMs"] = cfg.initTimeoutMs;

    // Microphone configuration
    doc["sampleRate"] = cfg.sampleRate;
    doc["bufferSize"] = cfg.bufferSize;

    // Fire effect defaults
    doc["baseCooling"] = cfg.baseCooling;
    doc["sparkHeatMin"] = cfg.sparkHeatMin;
    doc["sparkHeatMax"] = cfg.sparkHeatMax;
    doc["sparkChance"] = serialized(String(cfg.sparkChance, 2));
    doc["audioSparkBoost"] = serialized(String(cfg.audioSparkBoost, 2));
    doc["coolingAudioBias"] = cfg.coolingAudioBias;
    doc["bottomRowsForSparks"] = cfg.bottomRowsForSparks;

    // Serialize with pretty printing for readability
    serializeJsonPretty(doc, Serial);
    Serial.println();
}

void SerialConsole::uploadDeviceConfig(const char* jsonStr) {
    if (!configStorage_) {
        Serial.println(F("ERROR: ConfigStorage not available"));
        return;
    }

    // Parse JSON using ArduinoJson (1024 bytes to accommodate full device configs ~600 bytes)
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (error) {
        Serial.print(F("ERROR: JSON parse failed - "));
        Serial.println(error.c_str());
        Serial.println(F("Example: device upload {\"deviceId\":\"hat_v1\",\"ledWidth\":89,...}"));
        return;
    }

    // Build StoredDeviceConfig from JSON
    ConfigStorage::StoredDeviceConfig newConfig = {};

    // Device identification
    strncpy(newConfig.deviceId, doc["deviceId"] | "unknown", sizeof(newConfig.deviceId) - 1);
    strncpy(newConfig.deviceName, doc["deviceName"] | "Unnamed Device", sizeof(newConfig.deviceName) - 1);

    // Matrix/LED configuration
    newConfig.ledWidth = doc["ledWidth"] | 0;
    newConfig.ledHeight = doc["ledHeight"] | 1;
    newConfig.ledPin = doc["ledPin"] | 10;
    newConfig.brightness = doc["brightness"] | 100;
    newConfig.ledType = doc["ledType"] | 12390;  // Default: NEO_GRB + NEO_KHZ800
    newConfig.orientation = doc["orientation"] | 0;
    newConfig.layoutType = doc["layoutType"] | 0;

    // Charging configuration
    newConfig.fastChargeEnabled = doc["fastChargeEnabled"] | false;
    newConfig.lowBatteryThreshold = doc["lowBatteryThreshold"] | 3.5f;
    newConfig.criticalBatteryThreshold = doc["criticalBatteryThreshold"] | 3.3f;
    newConfig.minVoltage = doc["minVoltage"] | 3.0f;
    newConfig.maxVoltage = doc["maxVoltage"] | 4.2f;

    // IMU configuration
    newConfig.upVectorX = doc["upVectorX"] | 0.0f;
    newConfig.upVectorY = doc["upVectorY"] | 0.0f;
    newConfig.upVectorZ = doc["upVectorZ"] | 1.0f;
    newConfig.rotationDegrees = doc["rotationDegrees"] | 0.0f;
    newConfig.invertZ = doc["invertZ"] | false;
    newConfig.swapXY = doc["swapXY"] | false;
    newConfig.invertX = doc["invertX"] | false;
    newConfig.invertY = doc["invertY"] | false;

    // Serial configuration
    newConfig.baudRate = doc["baudRate"] | 115200;
    newConfig.initTimeoutMs = doc["initTimeoutMs"] | 2000;

    // Microphone configuration
    newConfig.sampleRate = doc["sampleRate"] | 16000;
    newConfig.bufferSize = doc["bufferSize"] | 32;

    // Fire effect defaults
    newConfig.baseCooling = doc["baseCooling"] | 40;
    newConfig.sparkHeatMin = doc["sparkHeatMin"] | 120;
    newConfig.sparkHeatMax = doc["sparkHeatMax"] | 255;
    newConfig.sparkChance = doc["sparkChance"] | 0.2f;
    newConfig.audioSparkBoost = doc["audioSparkBoost"] | 0.5f;
    newConfig.coolingAudioBias = doc["coolingAudioBias"] | -30;
    newConfig.bottomRowsForSparks = doc["bottomRowsForSparks"] | 1;

    // Mark as valid
    newConfig.isValid = true;

    // Validate configuration
    if (!DeviceConfigLoader::validate(newConfig)) {
        Serial.println(F("ERROR: Device config validation failed"));
        Serial.println(F("Check LED count, pin numbers, and voltage ranges"));
        return;
    }

    // Save to flash
    configStorage_->setDeviceConfig(newConfig);

    // Trigger flash write by saving full configuration
    // Note: mic_ should always be available (audio initialized even in safe mode)
    // but generators may be null in safe mode
    if (fireGenerator_ && waterGenerator_ && lightningGenerator_ && mic_) {
        // Normal mode: save with actual generator params
        configStorage_->saveConfiguration(
            fireGenerator_->getParams(),
            waterGenerator_->getParams(),
            lightningGenerator_->getParams(),
            *mic_,
            audioCtrl_
        );
    } else if (mic_) {
        // Safe mode: generators null, but mic available
        // Save with default generator params (only device config matters)
        FireParams defaultFire;
        WaterParams defaultWater;
        LightningParams defaultLightning;
        configStorage_->saveConfiguration(
            defaultFire,
            defaultWater,
            defaultLightning,
            *mic_,
            audioCtrl_
        );
    } else {
        Serial.println(F("ERROR: Cannot save config - mic not initialized"));
        return;
    }

    Serial.println(F("✓ Device config saved to flash"));
    Serial.print(F("Device: "));
    Serial.print(newConfig.deviceName);
    Serial.print(F(" ("));
    Serial.print(newConfig.ledWidth * newConfig.ledHeight);
    Serial.println(F(" LEDs)"));
    Serial.println(F("\n**REBOOT DEVICE TO APPLY CONFIGURATION**"));
}

void SerialConsole::restoreDefaults() {
    // NOTE: Particle-based generators get defaults from their constructors
    // Generator parameter reset is handled by ConfigStorage::loadDefaults()
    // which will be applied on next load/save cycle

    // Restore mic defaults (window/range normalization)
    // Note: Transient detection settings moved to EnsembleDetector
    if (mic_) {
        mic_->peakTau = Defaults::PeakTau;              // 2s peak adaptation
        mic_->releaseTau = Defaults::ReleaseTau;        // 5s peak release
        mic_->hwTarget = 0.35f;                         // Target raw input level (±0.01 dead zone)

        // Fast AGC defaults (enabled by default for better low-level response)
        mic_->fastAgcEnabled = true;
        mic_->fastAgcThreshold = 0.15f;
        mic_->fastAgcPeriodMs = 5000;
        mic_->fastAgcTrackingTau = 5.0f;
    }

    // Restore audio controller defaults
    if (audioCtrl_) {
        audioCtrl_->activationThreshold = 0.4f;
        audioCtrl_->cbssAlpha = 0.9f;
        audioCtrl_->cbssTightness = 5.0f;
        audioCtrl_->beatConfidenceDecay = 0.98f;
        audioCtrl_->bayesLambda = 0.15f;
        audioCtrl_->bayesPriorCenter = 128.0f;
        audioCtrl_->bayesPriorWeight = 0.0f;
        audioCtrl_->bayesAcfWeight = 0.3f;
        audioCtrl_->bayesFtWeight = 0.0f;
        audioCtrl_->bayesCombWeight = 0.7f;
        audioCtrl_->bayesIoiWeight = 0.0f;
        audioCtrl_->cbssThresholdFactor = 1.0f;
        audioCtrl_->tempoSmoothingFactor = 0.85f;
        audioCtrl_->pulseBoostOnBeat = 1.3f;
        audioCtrl_->pulseSuppressOffBeat = 0.6f;
        audioCtrl_->energyBoostOnBeat = 0.3f;
        audioCtrl_->bpmMin = 60.0f;
        audioCtrl_->bpmMax = 200.0f;
    }

    // Restore effect defaults
    if (hueEffect_) {
        hueEffect_->setHueShift(0.0f);
        hueEffect_->setRotationSpeed(0.0f);
    }
}

// === GENERATOR COMMANDS ===
bool SerialConsole::handleGeneratorCommand(const char* cmd) {
    if (!pipeline_) return false;

    // "gen list" - list available generators
    if (strcmp(cmd, "gen list") == 0 || strcmp(cmd, "gen") == 0) {
        Serial.println(F("Available generators:"));
        for (int i = 0; i < RenderPipeline::NUM_GENERATORS; i++) {
            const char* name = RenderPipeline::getGeneratorNameByIndex(i);
            bool active = (RenderPipeline::getGeneratorTypeByIndex(i) == pipeline_->getGeneratorType());
            Serial.print(F("  "));
            Serial.print(name);
            if (active) Serial.print(F(" (active)"));
            Serial.println();
        }
        return true;
    }

    // "gen <name>" - switch to generator
    if (strncmp(cmd, "gen ", 4) == 0) {
        const char* name = cmd + 4;

        // Match generator by name
        GeneratorType type = GeneratorType::FIRE;  // Default
        bool found = false;

        if (strcmp(name, "fire") == 0) {
            type = GeneratorType::FIRE;
            found = true;
        } else if (strcmp(name, "water") == 0) {
            type = GeneratorType::WATER;
            found = true;
        } else if (strcmp(name, "lightning") == 0) {
            type = GeneratorType::LIGHTNING;
            found = true;
        } else if (strcmp(name, "audio") == 0) {
            type = GeneratorType::AUDIO;
            found = true;
        }

        if (found) {
            if (pipeline_->setGenerator(type)) {
                Serial.print(F("OK switched to "));
                Serial.println(pipeline_->getGeneratorName());
            } else {
                Serial.println(F("ERROR: Failed to switch generator"));
            }
        } else {
            Serial.print(F("Unknown generator: "));
            Serial.println(name);
            Serial.println(F("Use: fire, water, lightning, audio"));
        }
        return true;
    }

    return false;
}

// === EFFECT COMMANDS ===
bool SerialConsole::handleEffectCommand(const char* cmd) {
    if (!pipeline_) return false;

    // "effect list" - list available effects
    if (strcmp(cmd, "effect list") == 0 || strcmp(cmd, "effect") == 0) {
        Serial.println(F("Available effects:"));
        for (int i = 0; i < RenderPipeline::NUM_EFFECTS; i++) {
            const char* name = RenderPipeline::getEffectNameByIndex(i);
            bool active = (RenderPipeline::getEffectTypeByIndex(i) == pipeline_->getEffectType());
            Serial.print(F("  "));
            Serial.print(name);
            if (active) Serial.print(F(" (active)"));
            Serial.println();
        }
        return true;
    }

    // "effect <name>" - switch to effect (or disable with "none")
    if (strncmp(cmd, "effect ", 7) == 0) {
        const char* name = cmd + 7;

        // Match effect by name
        EffectType type = EffectType::NONE;
        bool found = false;

        if (strcmp(name, "none") == 0 || strcmp(name, "off") == 0) {
            type = EffectType::NONE;
            found = true;
        } else if (strcmp(name, "hue") == 0 || strcmp(name, "huerotation") == 0) {
            type = EffectType::HUE_ROTATION;
            found = true;
        }

        if (found) {
            if (pipeline_->setEffect(type)) {
                Serial.print(F("OK effect: "));
                Serial.println(pipeline_->getEffectName());
            } else {
                Serial.println(F("ERROR: Failed to set effect"));
            }
        } else {
            Serial.print(F("Unknown effect: "));
            Serial.println(name);
            Serial.println(F("Use: none, hue"));
        }
        return true;
    }

    return false;
}

// === WATER SETTINGS (Particle-based) ===
void SerialConsole::registerWaterSettings(WaterParams* wp) {
    if (!wp) return;

    // Spawn behavior
    settings_.registerFloat("basespawnchance", &wp->baseSpawnChance, "water",
        "Baseline drop spawn probability", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("audiospawnboost", &wp->audioSpawnBoost, "water",
        "Audio reactivity multiplier", 0.0f, 2.0f, onParamChanged);

    // Physics
    settings_.registerFloat("gravity", &wp->gravity, "water",
        "Gravity strength (positive=downward)", 0.0f, 20.0f, onParamChanged);
    settings_.registerFloat("windbase", &wp->windBase, "water",
        "Base wind force", -5.0f, 5.0f, onParamChanged);
    settings_.registerFloat("windvariation", &wp->windVariation, "water",
        "Wind variation amount", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("drag", &wp->drag, "water",
        "Drag coefficient", 0.9f, 1.0f, onParamChanged);

    // Drop appearance
    settings_.registerFloat("dropvelmin", &wp->dropVelocityMin, "water",
        "Minimum downward velocity", 0.0f, 10.0f, onParamChanged);
    settings_.registerFloat("dropvelmax", &wp->dropVelocityMax, "water",
        "Maximum downward velocity", 0.0f, 10.0f, onParamChanged);
    settings_.registerFloat("dropspread", &wp->dropSpread, "water",
        "Horizontal velocity spread", 0.0f, 5.0f, onParamChanged);

    // Splash behavior
    settings_.registerUint8("splashparticles", &wp->splashParticles, "water",
        "Particles spawned per splash", 0, 10, onParamChanged);
    settings_.registerFloat("splashvelmin", &wp->splashVelocityMin, "water",
        "Minimum splash velocity", 0.0f, 10.0f, onParamChanged);
    settings_.registerFloat("splashvelmax", &wp->splashVelocityMax, "water",
        "Maximum splash velocity", 0.0f, 10.0f, onParamChanged);
    settings_.registerUint8("splashintensity", &wp->splashIntensity, "water",
        "Splash particle intensity", 0, 255, onParamChanged);

    // Lifecycle
    settings_.registerUint8("maxparticles", &wp->maxParticles, "water",
        "Maximum active particles", 1, 64, onParamChanged);
    settings_.registerUint8("defaultlifespan", &wp->defaultLifespan, "water",
        "Default particle lifespan (frames)", 20, 180, onParamChanged);
    settings_.registerUint8("intensitymin", &wp->intensityMin, "water",
        "Minimum spawn intensity", 0, 255, onParamChanged);
    settings_.registerUint8("intensitymax", &wp->intensityMax, "water",
        "Maximum spawn intensity", 0, 255, onParamChanged);

    // Audio reactivity
    settings_.registerFloat("musicspawnpulse", &wp->musicSpawnPulse, "water",
        "Phase modulation for spawn rate", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("organictransmin", &wp->organicTransientMin, "water",
        "Min transient to trigger burst", 0.0f, 1.0f, onParamChanged);

    // Background
    settings_.registerFloat("bgintensity", &wp->backgroundIntensity, "water",
        "Noise background brightness", 0.0f, 1.0f, onParamChanged);
}

// === LIGHTNING SETTINGS (Particle-based) ===
void SerialConsole::registerLightningSettings(LightningParams* lp) {
    if (!lp) return;

    // Spawn behavior
    settings_.registerFloat("basespawnchance", &lp->baseSpawnChance, "lightning",
        "Baseline bolt spawn probability", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("audiospawnboost", &lp->audioSpawnBoost, "lightning",
        "Audio reactivity multiplier", 0.0f, 2.0f, onParamChanged);

    // Bolt appearance
    settings_.registerFloat("boltvelmin", &lp->boltVelocityMin, "lightning",
        "Minimum bolt speed", 0.0f, 20.0f, onParamChanged);
    settings_.registerFloat("boltvelmax", &lp->boltVelocityMax, "lightning",
        "Maximum bolt speed", 0.0f, 20.0f, onParamChanged);
    settings_.registerUint8("faderate", &lp->fadeRate, "lightning",
        "Intensity decay per frame", 0, 255, onParamChanged);

    // Branching behavior
    settings_.registerUint8("branchchance", &lp->branchChance, "lightning",
        "Branch probability (%)", 0, 100, onParamChanged);
    settings_.registerUint8("branchcount", &lp->branchCount, "lightning",
        "Branches per trigger", 1, 4, onParamChanged);
    settings_.registerFloat("branchspread", &lp->branchAngleSpread, "lightning",
        "Branch angle spread (radians)", 0.0f, 3.14159f, onParamChanged);
    settings_.registerUint8("branchintloss", &lp->branchIntensityLoss, "lightning",
        "Branch intensity reduction (%)", 0, 100, onParamChanged);

    // Lifecycle
    settings_.registerUint8("maxparticles", &lp->maxParticles, "lightning",
        "Maximum active particles", 1, 32, onParamChanged);
    settings_.registerUint8("defaultlifespan", &lp->defaultLifespan, "lightning",
        "Default particle lifespan (frames)", 10, 60, onParamChanged);
    settings_.registerUint8("intensitymin", &lp->intensityMin, "lightning",
        "Minimum spawn intensity", 0, 255, onParamChanged);
    settings_.registerUint8("intensitymax", &lp->intensityMax, "lightning",
        "Maximum spawn intensity", 0, 255, onParamChanged);

    // Audio reactivity
    settings_.registerFloat("musicspawnpulse", &lp->musicSpawnPulse, "lightning",
        "Phase modulation for spawn rate", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("organictransmin", &lp->organicTransientMin, "lightning",
        "Min transient to trigger burst", 0.0f, 1.0f, onParamChanged);

    // Background
    settings_.registerFloat("bgintensity", &lp->backgroundIntensity, "lightning",
        "Noise background brightness", 0.0f, 1.0f, onParamChanged);
}

// === AUDIO VISUALIZATION GENERATOR SETTINGS ===
void SerialConsole::registerAudioVisSettings(AudioParams* ap) {
    if (!ap) return;

    // Transient visualization (green gradient from top)
    settings_.registerFloat("transientrowfrac", &ap->transientRowFraction, "audiovis",
        "Fraction of height for transient indicator", 0.1f, 0.5f, onParamChanged);
    settings_.registerFloat("transientdecay", &ap->transientDecayRate, "audiovis",
        "Transient decay rate per frame", 0.01f, 0.5f, onParamChanged);
    settings_.registerUint8("transientbright", &ap->transientBrightness, "audiovis",
        "Maximum transient brightness", 0, 255, onParamChanged);

    // Energy level visualization (yellow row)
    settings_.registerUint8("levelbright", &ap->levelBrightness, "audiovis",
        "Energy level row brightness", 0, 255, onParamChanged);
    settings_.registerFloat("levelsmooth", &ap->levelSmoothing, "audiovis",
        "Energy level smoothing factor", 0.0f, 0.99f, onParamChanged);

    // Phase visualization (blue row, full height)
    settings_.registerUint8("phasebright", &ap->phaseBrightness, "audiovis",
        "Phase row maximum brightness", 0, 255, onParamChanged);
    settings_.registerFloat("musicmodethresh", &ap->musicModeThreshold, "audiovis",
        "Rhythm confidence threshold for phase display", 0.0f, 1.0f, onParamChanged);

    // Beat pulse (blue center band on beat)
    settings_.registerUint8("beatpulsebright", &ap->beatPulseBrightness, "audiovis",
        "Beat pulse band max brightness", 0, 255, onParamChanged);
    settings_.registerFloat("beatpulsedecay", &ap->beatPulseDecay, "audiovis",
        "Beat pulse decay rate per frame", 0.01f, 0.5f, onParamChanged);
    settings_.registerFloat("beatpulsewidth", &ap->beatPulseWidth, "audiovis",
        "Beat pulse band width as fraction of height", 0.05f, 0.5f, onParamChanged);

    // Background
    settings_.registerUint8("bgbright", &ap->backgroundBrightness, "audiovis",
        "Background brightness", 0, 255, onParamChanged);
}

// === EFFECT SETTINGS ===
void SerialConsole::registerEffectSettings() {
    if (!hueEffect_) return;

    // Initialize file-scope statics from current effect state
    effectHueShift_ = hueEffect_->getHueShift();
    effectRotationSpeed_ = hueEffect_->getRotationSpeed();

    settings_.registerFloat("hueshift", &effectHueShift_, "effect",
        "Static hue offset (0-1)", 0.0f, 1.0f);
    settings_.registerFloat("huespeed", &effectRotationSpeed_, "effect",
        "Auto-rotation speed (cycles/sec)", 0.0f, 2.0f);
}

void SerialConsole::syncEffectSettings() {
    if (!hueEffect_) return;

    // Apply file-scope statics (modified by SettingsRegistry) to the actual effect
    hueEffect_->setHueShift(effectHueShift_);
    hueEffect_->setRotationSpeed(effectRotationSpeed_);
}

void SerialConsole::streamTick() {
    if (!streamEnabled_) return;

    uint32_t now = millis();

    // STATUS update at ~1Hz
    static uint32_t lastStatusMs = 0;
    if (mic_ && (now - lastStatusMs >= 1000)) {
        lastStatusMs = now;
        Serial.print(F("{\"type\":\"STATUS\",\"ts\":"));
        Serial.print(now);
        Serial.print(F(",\"mode\":\"ensemble\""));
        Serial.print(F(",\"hwGain\":"));
        Serial.print(mic_->getHwGain());
        Serial.print(F(",\"level\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"peakLevel\":"));
        Serial.print(mic_->getPeakLevel(), 2);
        Serial.println(F("}"));
    }

    // Audio streaming at ~20Hz (normal) or ~100Hz (fast mode for testing)
    uint16_t period = streamFast_ ? STREAM_FAST_PERIOD_MS : STREAM_PERIOD_MS;
    if (mic_ && (now - streamLastMs_ >= period)) {
        streamLastMs_ = now;

        // Output compact JSON for web app (abbreviated field names for serial bandwidth)
        // Format: {"a":{"l":0.45,"t":0.85,"pk":0.32,"vl":0.04,"raw":0.12,"h":32,"alive":1,"z":0.15}}
        //
        // Field Mapping (abbreviated → full name : range):
        // l     → level            : 0-1 (post-range-mapping output, noise-gated)
        // t     → transient        : 0-1 (ensemble transient strength from all detectors)
        // pk    → peak             : 0-1 (current tracked peak for window normalization, raw range)
        // vl    → valley           : 0-1 (current tracked valley for window normalization, raw range)
        // raw   → raw ADC level    : 0-1 (what HW gain targets, pre-normalization)
        // h     → hardware gain    : 0-80 (PDM gain setting)
        // alive → PDM alive status : 0 or 1 (microphone health: 0=dead, 1=working)
        // z     → zero-crossing    : 0-1 (zero-crossing rate, for frequency classification)
        //
        // Debug mode additional fields:
        // agree → detector agreement : 0-7 (how many detectors fired)
        // conf  → ensemble confidence: 0-1 (combined confidence score)
        Serial.print(F("{\"a\":{\"l\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"t\":"));
        // Transient now comes from ensemble detector
        float transient = 0.0f;
        if (audioCtrl_) {
            transient = audioCtrl_->getLastEnsembleOutput().transientStrength;
        }
        Serial.print(transient, 2);
        Serial.print(F(",\"pk\":"));
        Serial.print(mic_->getPeakLevel(), 2);
        Serial.print(F(",\"vl\":"));
        Serial.print(mic_->getValleyLevel(), 2);
        Serial.print(F(",\"raw\":"));
        Serial.print(mic_->getRawLevel(), 2);
        Serial.print(F(",\"h\":"));
        Serial.print(mic_->getHwGain());
        Serial.print(F(",\"alive\":"));
        Serial.print(mic_->isPdmAlive() ? 1 : 0);

        // Debug mode: add ensemble detection internal state
        if (streamDebug_ && audioCtrl_) {
            const EnsembleOutput& ens = audioCtrl_->getLastEnsembleOutput();
            Serial.print(F(",\"agree\":"));
            Serial.print(ens.detectorAgreement);
            Serial.print(F(",\"conf\":"));
            Serial.print(ens.ensembleConfidence, 3);

            // Per-band flux from BandWeightedFlux detector
            const BandWeightedFluxDetector& bf = audioCtrl_->getEnsemble().getBandFlux();
            Serial.print(F(",\"bf\":"));
            Serial.print(bf.getBassFlux(), 3);
            Serial.print(F(",\"mf\":"));
            Serial.print(bf.getMidFlux(), 3);
            Serial.print(F(",\"hf\":"));
            Serial.print(bf.getHighFlux(), 3);
            Serial.print(F(",\"cf\":"));
            Serial.print(bf.getCombinedFlux(), 3);
            Serial.print(F(",\"af\":"));
            Serial.print(bf.getAverageFlux(), 3);

            // Spectral processing state (compressor + whitening)
            const SharedSpectralAnalysis& spectral = audioCtrl_->getEnsemble().getSpectral();
            Serial.print(F(",\"rms\":"));
            Serial.print(spectral.getFrameRmsDb(), 1);
            Serial.print(F(",\"cg\":"));
            Serial.print(spectral.getSmoothedGainDb(), 2);
        }

        Serial.print(F("}"));

        // AudioController telemetry (unified rhythm tracking)
        // Format: "m":{"a":1,"bpm":125.3,"ph":0.45,"str":0.82,"conf":0.75,"bc":42,"q":0,"e":0.5,"p":0.8,"cb":0.12,"oss":0.05,"ttb":18,"bp":1,"od":3.2}
        // a = rhythm active, bpm = tempo, ph = phase, str = rhythm strength
        // conf = CBSS confidence, bc = beat count, q = beat event (phase wrap)
        // e = energy, p = pulse, cb = CBSS value, oss = onset strength
        // ttb = frames until next beat, bp = last beat was predicted (1) vs fallback (0)
        // od = onset density (onsets/second, EMA smoothed)
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();

            // Detect beat events via phase wrapping (>0.8 → <0.2)
            static float lastStreamPhase = 0.0f;
            float currentPhase = audio.phase;
            int beatEvent = (lastStreamPhase > 0.8f && currentPhase < 0.2f && audio.rhythmStrength > audioCtrl_->activationThreshold) ? 1 : 0;
            lastStreamPhase = currentPhase;

            Serial.print(F(",\"m\":{\"a\":"));
            Serial.print(audio.rhythmStrength > audioCtrl_->activationThreshold ? 1 : 0);
            Serial.print(F(",\"bpm\":"));
            Serial.print(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F(",\"ph\":"));
            Serial.print(currentPhase, 2);
            Serial.print(F(",\"str\":"));
            Serial.print(audio.rhythmStrength, 2);
            Serial.print(F(",\"conf\":"));
            Serial.print(audioCtrl_->getCbssConfidence(), 2);
            Serial.print(F(",\"bc\":"));
            Serial.print(audioCtrl_->getBeatCount());
            Serial.print(F(",\"q\":"));
            Serial.print(beatEvent);
            Serial.print(F(",\"e\":"));
            Serial.print(audio.energy, 2);
            Serial.print(F(",\"p\":"));
            Serial.print(audio.pulse, 2);
            Serial.print(F(",\"cb\":"));
            Serial.print(audioCtrl_->getCurrentCBSS(), 3);
            Serial.print(F(",\"oss\":"));
            Serial.print(audioCtrl_->getLastOnsetStrength(), 3);
            Serial.print(F(",\"ttb\":"));
            Serial.print(audioCtrl_->getTimeToNextBeat());
            Serial.print(F(",\"bp\":"));
            Serial.print(audioCtrl_->wasLastBeatPredicted() ? 1 : 0);
            Serial.print(F(",\"od\":"));
            Serial.print(audioCtrl_->getOnsetDensity(), 1);

            // Debug mode: add Bayesian tempo state for tuning
            if (streamDebug_) {
                Serial.print(F(",\"ps\":"));
                Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
                Serial.print(F(",\"bb\":"));
                Serial.print(audioCtrl_->getBayesBestBin());
                Serial.print(F(",\"bbc\":"));
                Serial.print(audioCtrl_->getBayesBestConf(), 4);
                Serial.print(F(",\"bft\":"));
                Serial.print(audioCtrl_->getBayesFtObs(), 3);
                Serial.print(F(",\"bcb\":"));
                Serial.print(audioCtrl_->getBayesCombObs(), 3);
                Serial.print(F(",\"bio\":"));
                Serial.print(audioCtrl_->getBayesIoiObs(), 3);
            }

            Serial.print(F("}"));
        }

        // LED brightness telemetry
        // NOTE: Particle-based generators don't track total heat/brightness
        // in the same way, so these stats are not available
        // TODO: Add particle pool statistics (active count, etc.)

        Serial.println(F("}"));
    }

    // Battery streaming at ~1Hz
    if (battery_ && (now - batteryLastMs_ >= BATTERY_PERIOD_MS)) {
        batteryLastMs_ = now;

        // Output battery status JSON
        // Format: {"b":{"n":true,"c":false,"v":3.85,"p":72}}
        // n = connected (battery detected)
        // c = charging (true if charging)
        // v = voltage (in volts)
        // p = percent (0-100)
        Serial.print(F("{\"b\":{\"n\":"));
        Serial.print(battery_->isBatteryConnected() ? F("true") : F("false"));
        Serial.print(F(",\"c\":"));
        Serial.print(battery_->isCharging() ? F("true") : F("false"));
        Serial.print(F(",\"v\":"));
        Serial.print(battery_->getVoltage(), 2);
        Serial.print(F(",\"p\":"));
        Serial.print(battery_->getPercent());
        Serial.println(F("}}"));
    }
}

// === ENSEMBLE DETECTOR COMMANDS ===
bool SerialConsole::handleEnsembleCommand(const char* cmd) {
    // Handle "show detectors" - list all detector states
    if (strcmp(cmd, "show detectors") == 0 || strcmp(cmd, "detectors") == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        const EnsembleDetector& ens = audioCtrl_->getEnsemble();
        const EnsembleFusion& fusion = ens.getFusion();

        Serial.println(F("=== Ensemble Detectors ==="));
        Serial.println(F("Name      Weight  Thresh  Enabled  LastStrength"));
        Serial.println(F("--------  ------  ------  -------  ------------"));

        const DetectionResult* lastResults = ens.getLastResults();
        for (int i = 0; i < EnsembleDetector::NUM_DETECTORS; i++) {
            DetectorType type = static_cast<DetectorType>(i);
            const DetectorConfig& cfg = fusion.getConfig(type);
            const char* name = getDetectorName(type);

            // Pad name to 8 chars
            Serial.print(name);
            for (int j = strlen(name); j < 10; j++) Serial.print(' ');

            Serial.print(cfg.weight, 2);
            Serial.print(F("    "));
            Serial.print(cfg.threshold, 1);
            Serial.print(F("    "));
            Serial.print(cfg.enabled ? F("yes") : F("no "));
            Serial.print(F("      "));
            Serial.println(lastResults[i].strength, 3);
        }
        return true;
    }

    // Handle "show ensemble" - show fusion configuration
    if (strcmp(cmd, "show ensemble") == 0 || strcmp(cmd, "ensemble") == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        const EnsembleFusion& fusion = audioCtrl_->getEnsemble().getFusion();

        Serial.println(F("=== Ensemble Fusion Configuration ==="));
        Serial.println(F("Agreement Boost Values:"));
        for (int i = 0; i <= EnsembleDetector::NUM_DETECTORS; i++) {
            Serial.print(F("  "));
            Serial.print(i);
            Serial.print(F(" detector(s): "));
            Serial.println(fusion.getAgreementBoost(i), 2);
        }
        Serial.print(F("\nTotal Weight: "));
        Serial.println(fusion.getTotalWeight(), 3);

        // Show last output
        const EnsembleOutput& output = audioCtrl_->getLastEnsembleOutput();
        Serial.println(F("\nLast Output:"));
        Serial.print(F("  Strength: "));
        Serial.println(output.transientStrength, 3);
        Serial.print(F("  Confidence: "));
        Serial.println(output.ensembleConfidence, 3);
        Serial.print(F("  Agreement: "));
        Serial.print(output.detectorAgreement);
        Serial.println(F("/7"));
        Serial.print(F("  Dominant: "));
        Serial.println(getDetectorName(static_cast<DetectorType>(output.dominantDetector)));
        Serial.println(F("\nFusion Parameters:"));
        Serial.print(F("  cooldown: "));
        Serial.print(fusion.getCooldownMs());
        Serial.println(F(" ms (base)"));
        Serial.print(F("  adaptcool: "));
        Serial.println(fusion.isAdaptiveCooldownEnabled() ? F("on") : F("off"));
        Serial.print(F("  effcool: "));
        Serial.print(fusion.getEffectiveCooldownMs());
        Serial.print(F(" ms (tempo="));
        Serial.print(fusion.getTempoHint(), 1);
        Serial.println(F(" bpm)"));
        Serial.print(F("  minconf: "));
        Serial.println(fusion.getMinConfidence(), 3);
        Serial.print(F("  minlevel: "));
        Serial.println(fusion.getMinAudioLevel(), 3);

        // BassBand-specific parameters
        const BassBandDetector& bass = audioCtrl_->getEnsemble().getBassBand();
        Serial.println(F("\nBassBand Noise Rejection:"));
        Serial.print(F("  minflux: "));
        Serial.println(bass.getMinAbsoluteFlux(), 3);
        Serial.print(F("  sharpness: "));
        Serial.println(bass.getSharpnessThreshold(), 2);

        // BandFlux-specific parameters
        const BandWeightedFluxDetector& bf = audioCtrl_->getEnsemble().getBandFlux();
        Serial.println(F("\nBandFlux Parameters:"));
        Serial.print(F("  gamma: "));
        Serial.println(bf.getGamma(), 1);
        Serial.print(F("  bassweight: "));
        Serial.println(bf.getBassWeight(), 2);
        Serial.print(F("  midweight: "));
        Serial.println(bf.getMidWeight(), 2);
        Serial.print(F("  highweight: "));
        Serial.println(bf.getHighWeight(), 2);
        Serial.print(F("  maxbin: "));
        Serial.println(bf.getMaxBin());
        Serial.print(F("  onsetdelta: "));
        Serial.println(bf.getMinOnsetDelta(), 2);
        Serial.print(F("  perbandthresh: "));
        Serial.println(bf.getPerBandThresh() ? "on" : "off");
        Serial.print(F("  perbandmult: "));
        Serial.println(bf.getPerBandThreshMult(), 2);
        Serial.print(F("  diffframes: "));
        Serial.println(bf.getDiffFrames());
        return true;
    }

    // === ENSEMBLE FUSION PARAMETERS ===
    // ensemble_cooldown: Unified cooldown between ensemble detections (ms)
    if (strncmp(cmd, "set ensemble_cooldown ", 22) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 22);
        if (value >= 20 && value <= 500) {
            audioCtrl_->getEnsemble().getFusion().setCooldownMs(value);
            Serial.print(F("OK ensemble_cooldown="));
            Serial.println(value);
        } else {
            Serial.println(F("ERROR: Valid range 20-500 ms"));
        }
        return true;
    }
    if (strcmp(cmd, "show ensemble_cooldown") == 0 || strcmp(cmd, "ensemble_cooldown") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("ensemble_cooldown="));
        Serial.print(audioCtrl_->getEnsemble().getFusion().getCooldownMs());
        Serial.println(F(" ms"));
        return true;
    }

    // ensemble_minconf: Minimum confidence threshold for detection output
    if (strncmp(cmd, "set ensemble_minconf ", 21) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 21);
        if (value >= 0.0f && value <= 1.0f) {
            audioCtrl_->getEnsemble().getFusion().setMinConfidence(value);
            Serial.print(F("OK ensemble_minconf="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-1.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show ensemble_minconf") == 0 || strcmp(cmd, "ensemble_minconf") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("ensemble_minconf="));
        Serial.println(audioCtrl_->getEnsemble().getFusion().getMinConfidence(), 3);
        return true;
    }

    // ensemble_minlevel: Noise gate - minimum audio level for detection
    if (strncmp(cmd, "set ensemble_minlevel ", 22) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 22);
        if (value >= 0.0f && value <= 1.0f) {
            audioCtrl_->getEnsemble().getFusion().setMinAudioLevel(value);
            Serial.print(F("OK ensemble_minlevel="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-1.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show ensemble_minlevel") == 0 || strcmp(cmd, "ensemble_minlevel") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("ensemble_minlevel="));
        Serial.println(audioCtrl_->getEnsemble().getFusion().getMinAudioLevel(), 3);
        return true;
    }

    // ens_adaptcool: Enable/disable tempo-adaptive cooldown
    if (strncmp(cmd, "set ens_adaptcool ", 18) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 18);
        audioCtrl_->getEnsemble().getFusion().setAdaptiveCooldown(value != 0);
        Serial.print(F("OK ens_adaptcool="));
        Serial.println(value != 0 ? "on" : "off");
        return true;
    }
    if (strcmp(cmd, "show ens_adaptcool") == 0 || strcmp(cmd, "ens_adaptcool") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("ens_adaptcool="));
        Serial.println(audioCtrl_->getEnsemble().getFusion().isAdaptiveCooldownEnabled() ? "on" : "off");
        return true;
    }

    // ens_effcool: Show effective cooldown (read-only, affected by tempo)
    if (strcmp(cmd, "show ens_effcool") == 0 || strcmp(cmd, "ens_effcool") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("ens_effcool="));
        Serial.print(audioCtrl_->getEnsemble().getFusion().getEffectiveCooldownMs());
        Serial.print(F("ms (base="));
        Serial.print(audioCtrl_->getEnsemble().getFusion().getCooldownMs());
        Serial.print(F("ms, tempo="));
        Serial.print(audioCtrl_->getEnsemble().getFusion().getTempoHint(), 1);
        Serial.println(F("bpm)"));
        return true;
    }

    // === PULSE MODULATION THRESHOLDS (rhythm category) ===
    // pulsenear: Phase distance threshold for near-beat detection (boost transients)
    if (strncmp(cmd, "set pulsenear ", 14) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 14);
        if (value >= 0.0f && value <= 0.5f) {
            audioCtrl_->pulseNearBeatThreshold = value;
            Serial.print(F("OK pulsenear="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-0.5"));
        }
        return true;
    }
    if (strcmp(cmd, "show pulsenear") == 0 || strcmp(cmd, "pulsenear") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("pulsenear="));
        Serial.println(audioCtrl_->pulseNearBeatThreshold, 3);
        return true;
    }

    // pulsefar: Phase distance threshold for off-beat detection (suppress transients)
    if (strncmp(cmd, "set pulsefar ", 13) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 13);
        if (value >= 0.2f && value <= 0.5f) {
            audioCtrl_->pulseFarFromBeatThreshold = value;
            Serial.print(F("OK pulsefar="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.2-0.5"));
        }
        return true;
    }
    if (strcmp(cmd, "show pulsefar") == 0 || strcmp(cmd, "pulsefar") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("pulsefar="));
        Serial.println(audioCtrl_->pulseFarFromBeatThreshold, 3);
        return true;
    }

    // Handle "set detector_enable <type> <0|1>"
    if (strncmp(cmd, "set detector_enable ", 20) == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        const char* args = cmd + 20;
        char typeName[16];
        int enabled = 0;
        if (sscanf(args, "%15s %d", typeName, &enabled) == 2) {
            DetectorType type;
            if (parseDetectorType(typeName, type)) {
                audioCtrl_->setDetectorEnabled(type, enabled != 0);
                Serial.print(F("OK "));
                Serial.print(getDetectorName(type));
                Serial.print(F(" enabled="));
                Serial.println(enabled);
            } else {
                Serial.print(F("ERROR: Unknown detector '"));
                Serial.print(typeName);
                Serial.println(F("'. Use: drummer, spectral, hfc, bass, complex, novelty, bandflux"));
            }
        } else {
            Serial.println(F("Usage: set detector_enable <type> <0|1>"));
            Serial.println(F("Types: drummer, spectral, hfc, bass, complex, novelty, bandflux"));
        }
        return true;
    }

    // Handle "set detector_weight <type> <value>"
    if (strncmp(cmd, "set detector_weight ", 20) == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        const char* args = cmd + 20;
        // Parse type name (until space) and value (using atof, not sscanf %f)
        char typeName[16];
        int i = 0;
        while (args[i] && args[i] != ' ' && i < 15) {
            typeName[i] = args[i];
            i++;
        }
        typeName[i] = '\0';
        // Skip space and get value
        while (args[i] == ' ') i++;
        if (typeName[0] && args[i]) {
            float weight = atof(args + i);
            DetectorType type;
            if (parseDetectorType(typeName, type)) {
                if (weight >= 0.0f && weight <= 1.0f) {
                    audioCtrl_->setDetectorWeight(type, weight);
                    Serial.print(F("OK "));
                    Serial.print(getDetectorName(type));
                    Serial.print(F(" weight="));
                    Serial.println(weight, 3);
                } else {
                    Serial.println(F("ERROR: Weight must be 0.0-1.0"));
                }
            } else {
                Serial.print(F("ERROR: Unknown detector '"));
                Serial.print(typeName);
                Serial.println(F("'. Use: drummer, spectral, hfc, bass, complex, novelty, bandflux"));
            }
        } else {
            Serial.println(F("Usage: set detector_weight <type> <value>"));
            Serial.println(F("Types: drummer, spectral, hfc, bass, complex, novelty, bandflux"));
        }
        return true;
    }

    // Handle "set detector_thresh <type> <value>"
    if (strncmp(cmd, "set detector_thresh ", 20) == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        const char* args = cmd + 20;
        // Parse type name (until space) and value (using atof, not sscanf %f)
        char typeName[16];
        int i = 0;
        while (args[i] && args[i] != ' ' && i < 15) {
            typeName[i] = args[i];
            i++;
        }
        typeName[i] = '\0';
        // Skip space and get value
        while (args[i] == ' ') i++;
        if (typeName[0] && args[i]) {
            float threshold = atof(args + i);
            DetectorType type;
            if (parseDetectorType(typeName, type)) {
                if (threshold > 0.0f) {
                    audioCtrl_->setDetectorThreshold(type, threshold);
                    Serial.print(F("OK "));
                    Serial.print(getDetectorName(type));
                    Serial.print(F(" threshold="));
                    Serial.println(threshold, 2);
                } else {
                    Serial.println(F("ERROR: Threshold must be > 0"));
                }
            } else {
                Serial.print(F("ERROR: Unknown detector '"));
                Serial.print(typeName);
                Serial.println(F("'. Use: drummer, spectral, hfc, bass, complex, novelty, bandflux"));
            }
        } else {
            Serial.println(F("Usage: set detector_thresh <type> <value>"));
            Serial.println(F("Types: drummer, spectral, hfc, bass, complex, novelty, bandflux"));
        }
        return true;
    }

    // Handle "set agree_<n> <value>" for agreement boost values
    if (strncmp(cmd, "set agree_", 10) == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        // Parse "set agree_N value" where N is 0-7
        // Format: "set agree_" (10 chars) + digit + space + value
        const char* args = cmd + 10;
        if (args[0] >= '0' && args[0] <= '7' && args[1] == ' ') {
            int n = args[0] - '0';
            float value = atof(args + 2);
            // Get current boosts, modify one, set all
            EnsembleFusion& fusion = audioCtrl_->getEnsemble().getFusion();
            float boosts[EnsembleDetector::NUM_DETECTORS + 1];
            for (int i = 0; i <= EnsembleDetector::NUM_DETECTORS; i++) {
                boosts[i] = fusion.getAgreementBoost(i);
            }
            boosts[n] = value;
            fusion.setAgreementBoosts(boosts);
            Serial.print(F("OK agree_"));
            Serial.print(n);
            Serial.print(F("="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("Usage: set agree_<0-7> <value>"));
            Serial.println(F("Example: set agree_1 0.6"));
        }
        return true;
    }

    // === DETECTOR-SPECIFIC PARAMETERS ===

    // Drummer: attackmult, avgtau
    if (strncmp(cmd, "set drummer_attackmult ", 23) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 23);
        audioCtrl_->getEnsemble().getDrummer().setAttackMultiplier(value);
        Serial.print(F("OK drummer_attackmult="));
        Serial.println(value, 3);
        return true;
    }
    if (strncmp(cmd, "set drummer_avgtau ", 19) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 19);
        audioCtrl_->getEnsemble().getDrummer().setAverageTau(value);
        Serial.print(F("OK drummer_avgtau="));
        Serial.println(value, 3);
        return true;
    }
    if (strcmp(cmd, "show drummer_attackmult") == 0 || strcmp(cmd, "drummer_attackmult") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("drummer_attackmult="));
        Serial.println(audioCtrl_->getEnsemble().getDrummer().getAttackMultiplier(), 3);
        return true;
    }
    if (strcmp(cmd, "show drummer_avgtau") == 0 || strcmp(cmd, "drummer_avgtau") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("drummer_avgtau="));
        Serial.println(audioCtrl_->getEnsemble().getDrummer().getAverageTau(), 3);
        return true;
    }
    if (strncmp(cmd, "set drummer_minriserate ", 24) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 24);
        audioCtrl_->getEnsemble().getDrummer().setMinRiseRate(value);
        Serial.print(F("OK drummer_minriserate="));
        Serial.println(value, 3);
        return true;
    }
    if (strcmp(cmd, "show drummer_minriserate") == 0 || strcmp(cmd, "drummer_minriserate") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("drummer_minriserate="));
        Serial.println(audioCtrl_->getEnsemble().getDrummer().getMinRiseRate(), 3);
        return true;
    }

    // SpectralFlux: now operates on 26 mel bands (no configurable bin range)

    // HFC: minbin, maxbin, attackmult
    if (strncmp(cmd, "set hfc_minbin ", 15) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 15);
        HFCDetector& d = audioCtrl_->getEnsemble().getHFC();
        d.setAnalysisRange(value, d.getMaxBin());
        Serial.print(F("OK hfc_minbin="));
        Serial.println(value);
        return true;
    }
    if (strncmp(cmd, "set hfc_maxbin ", 15) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 15);
        HFCDetector& d = audioCtrl_->getEnsemble().getHFC();
        d.setAnalysisRange(d.getMinBin(), value);
        Serial.print(F("OK hfc_maxbin="));
        Serial.println(value);
        return true;
    }
    if (strncmp(cmd, "set hfc_attackmult ", 19) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 19);
        audioCtrl_->getEnsemble().getHFC().setAttackMultiplier(value);
        Serial.print(F("OK hfc_attackmult="));
        Serial.println(value, 3);
        return true;
    }
    if (strcmp(cmd, "show hfc_minbin") == 0 || strcmp(cmd, "hfc_minbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("hfc_minbin="));
        Serial.println(audioCtrl_->getEnsemble().getHFC().getMinBin());
        return true;
    }
    if (strcmp(cmd, "show hfc_maxbin") == 0 || strcmp(cmd, "hfc_maxbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("hfc_maxbin="));
        Serial.println(audioCtrl_->getEnsemble().getHFC().getMaxBin());
        return true;
    }
    if (strcmp(cmd, "show hfc_attackmult") == 0 || strcmp(cmd, "hfc_attackmult") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("hfc_attackmult="));
        Serial.println(audioCtrl_->getEnsemble().getHFC().getAttackMultiplier(), 3);
        return true;
    }
    if (strncmp(cmd, "set hfc_sustainreject ", 22) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 22);
        audioCtrl_->getEnsemble().getHFC().setSustainRejectFrames(value);
        Serial.print(F("OK hfc_sustainreject="));
        Serial.println(value);
        return true;
    }
    if (strcmp(cmd, "show hfc_sustainreject") == 0 || strcmp(cmd, "hfc_sustainreject") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("hfc_sustainreject="));
        Serial.println(audioCtrl_->getEnsemble().getHFC().getSustainRejectFrames());
        return true;
    }

    // BassBand: minbin, maxbin
    if (strncmp(cmd, "set bass_minbin ", 16) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 16);
        BassBandDetector& d = audioCtrl_->getEnsemble().getBassBand();
        d.setAnalysisRange(value, d.getMaxBin());
        Serial.print(F("OK bass_minbin="));
        Serial.println(value);
        return true;
    }
    if (strncmp(cmd, "set bass_maxbin ", 16) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 16);
        BassBandDetector& d = audioCtrl_->getEnsemble().getBassBand();
        d.setAnalysisRange(d.getMinBin(), value);
        Serial.print(F("OK bass_maxbin="));
        Serial.println(value);
        return true;
    }
    if (strcmp(cmd, "show bass_minbin") == 0 || strcmp(cmd, "bass_minbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bass_minbin="));
        Serial.println(audioCtrl_->getEnsemble().getBassBand().getMinBin());
        return true;
    }
    if (strcmp(cmd, "show bass_maxbin") == 0 || strcmp(cmd, "bass_maxbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bass_maxbin="));
        Serial.println(audioCtrl_->getEnsemble().getBassBand().getMaxBin());
        return true;
    }

    // BassBand: minflux (minimum absolute flux threshold)
    if (strncmp(cmd, "set bass_minflux ", 17) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 17);
        audioCtrl_->getEnsemble().getBassBand().setMinAbsoluteFlux(value);
        Serial.print(F("OK bass_minflux="));
        Serial.println(value, 3);
        return true;
    }
    if (strcmp(cmd, "show bass_minflux") == 0 || strcmp(cmd, "bass_minflux") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bass_minflux="));
        Serial.println(audioCtrl_->getEnsemble().getBassBand().getMinAbsoluteFlux(), 3);
        return true;
    }

    // BassBand: sharpness (minimum transient sharpness ratio)
    if (strncmp(cmd, "set bass_sharpness ", 19) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 19);
        audioCtrl_->getEnsemble().getBassBand().setSharpnessThreshold(value);
        Serial.print(F("OK bass_sharpness="));
        Serial.println(value, 2);
        return true;
    }
    if (strcmp(cmd, "show bass_sharpness") == 0 || strcmp(cmd, "bass_sharpness") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bass_sharpness="));
        Serial.println(audioCtrl_->getEnsemble().getBassBand().getSharpnessThreshold(), 2);
        return true;
    }

    // === BAND FLUX PARAMETERS ===
    // bandflux_gamma: Log compression strength
    if (strncmp(cmd, "set bandflux_gamma ", 19) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 19);
        if (value >= 1.0f && value <= 100.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setGamma(value);
            Serial.print(F("OK bandflux_gamma="));
            Serial.println(value, 1);
        } else {
            Serial.println(F("ERROR: Valid range 1.0-100.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_gamma") == 0 || strcmp(cmd, "bandflux_gamma") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_gamma="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getGamma(), 1);
        return true;
    }

    // bandflux_bassweight: Bass band weight
    if (strncmp(cmd, "set bandflux_bassweight ", 24) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 24);
        if (value >= 0.0f && value <= 5.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setBassWeight(value);
            Serial.print(F("OK bandflux_bassweight="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-5.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_bassweight") == 0 || strcmp(cmd, "bandflux_bassweight") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_bassweight="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getBassWeight(), 2);
        return true;
    }

    // bandflux_midweight: Mid band weight
    if (strncmp(cmd, "set bandflux_midweight ", 23) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 23);
        if (value >= 0.0f && value <= 5.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setMidWeight(value);
            Serial.print(F("OK bandflux_midweight="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-5.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_midweight") == 0 || strcmp(cmd, "bandflux_midweight") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_midweight="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getMidWeight(), 2);
        return true;
    }

    // bandflux_highweight: High band weight (suppression)
    if (strncmp(cmd, "set bandflux_highweight ", 24) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 24);
        if (value >= 0.0f && value <= 2.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setHighWeight(value);
            Serial.print(F("OK bandflux_highweight="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-2.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_highweight") == 0 || strcmp(cmd, "bandflux_highweight") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_highweight="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getHighWeight(), 2);
        return true;
    }

    // bandflux_maxbin: Max FFT bin to analyze
    if (strncmp(cmd, "set bandflux_maxbin ", 20) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 20);
        if (value >= 16 && value <= 128) {
            audioCtrl_->getEnsemble().getBandFlux().setMaxBin(value);
            Serial.print(F("OK bandflux_maxbin="));
            Serial.println(value);
        } else {
            Serial.println(F("ERROR: Valid range 16-128"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_maxbin") == 0 || strcmp(cmd, "bandflux_maxbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_maxbin="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getMaxBin());
        return true;
    }

    // bandflux_onsetdelta: Min flux jump for onset confirmation (pad rejection)
    if (strncmp(cmd, "set bandflux_onsetdelta ", 24) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 24);
        if (value >= 0.0f && value <= 2.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setMinOnsetDelta(value);
            Serial.print(F("OK bandflux_onsetdelta="));
            Serial.println(value, 3);
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_onsetdelta") == 0 || strcmp(cmd, "bandflux_onsetdelta") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_onsetdelta="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getMinOnsetDelta(), 3);
        return true;
    }

    // === Experimental BandFlux gates (all disabled by default, runtime-only — NOT persisted to flash) ===
    // These reset to defaults on power cycle. To persist, add to SettingsRegistry.

    // bandflux_dominance: Band-dominance gate — max(bass,mid,high)/total (0.0 = disabled)
    if (strncmp(cmd, "set bandflux_dominance ", 23) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 23);
        if (value >= 0.0f && value <= 1.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setBandDominanceGate(value);
            Serial.print(F("OK bandflux_dominance="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-1.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_dominance") == 0 || strcmp(cmd, "bandflux_dominance") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_dominance="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getBandDominanceGate(), 3);
        return true;
    }

    // bandflux_decayratio: Post-onset decay ratio threshold (0.0 = disabled)
    // Flux must drop to this fraction of onset flux within N frames to confirm percussive
    if (strncmp(cmd, "set bandflux_decayratio ", 24) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 24);
        if (value >= 0.0f && value <= 1.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setDecayRatio(value);
            Serial.print(F("OK bandflux_decayratio="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-1.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_decayratio") == 0 || strcmp(cmd, "bandflux_decayratio") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_decayratio="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getDecayRatio(), 3);
        return true;
    }

    // bandflux_decayframes: Frames to wait for decay confirmation (0-6)
    if (strncmp(cmd, "set bandflux_decayframes ", 25) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 25);
        if (value >= 0 && value <= 6) {
            audioCtrl_->getEnsemble().getBandFlux().setDecayFrames(value);
            Serial.print(F("OK bandflux_decayframes="));
            Serial.println(value);
        } else {
            Serial.println(F("ERROR: Valid range 0-6"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_decayframes") == 0 || strcmp(cmd, "bandflux_decayframes") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_decayframes="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getDecayFrames());
        return true;
    }

    // bandflux_crestgate: Spectral crest factor gate (0.0 = disabled)
    // Reject tonal onsets (pads/chords) with crest above this threshold
    if (strncmp(cmd, "set bandflux_crestgate ", 23) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 23);
        if (value >= 0.0f && value <= 20.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setCrestGate(value);
            Serial.print(F("OK bandflux_crestgate="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-20.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_crestgate") == 0 || strcmp(cmd, "bandflux_crestgate") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_crestgate="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getCrestGate(), 2);
        return true;
    }

    // bandflux_perbandthresh: Per-band independent detection (0=off, 1=on)
    if (strncmp(cmd, "set bandflux_perbandthresh ", 27) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 27);
        audioCtrl_->getEnsemble().getBandFlux().setPerBandThresh(value != 0);
        Serial.print(F("OK bandflux_perbandthresh="));
        Serial.println(value != 0 ? "on" : "off");
        return true;
    }
    if (strcmp(cmd, "show bandflux_perbandthresh") == 0 || strcmp(cmd, "bandflux_perbandthresh") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_perbandthresh="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getPerBandThresh() ? "on" : "off");
        return true;
    }

    // bandflux_perbandmult: Per-band threshold multiplier (0.5-5.0)
    if (strncmp(cmd, "set bandflux_perbandmult ", 25) == 0) {
        if (!audioCtrl_) return true;
        float value = atof(cmd + 25);
        if (value >= 0.5f && value <= 5.0f) {
            audioCtrl_->getEnsemble().getBandFlux().setPerBandThreshMult(value);
            Serial.print(F("OK bandflux_perbandmult="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("ERROR: Valid range 0.5-5.0"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_perbandmult") == 0 || strcmp(cmd, "bandflux_perbandmult") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_perbandmult="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getPerBandThreshMult(), 2);
        return true;
    }

    // bandflux_diffframes: Temporal reference depth (1-3, SuperFlux diff_frames)
    if (strncmp(cmd, "set bandflux_diffframes ", 24) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 24);
        if (value >= 1 && value <= 3) {
            audioCtrl_->getEnsemble().getBandFlux().setDiffFrames(value);
            Serial.print(F("OK bandflux_diffframes="));
            Serial.println(value);
        } else {
            Serial.println(F("ERROR: Valid range 1-3"));
        }
        return true;
    }
    if (strcmp(cmd, "show bandflux_diffframes") == 0 || strcmp(cmd, "bandflux_diffframes") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("bandflux_diffframes="));
        Serial.println(audioCtrl_->getEnsemble().getBandFlux().getDiffFrames());
        return true;
    }

    // ComplexDomain: minbin, maxbin
    if (strncmp(cmd, "set complex_minbin ", 19) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 19);
        ComplexDomainDetector& d = audioCtrl_->getEnsemble().getComplexDomain();
        d.setAnalysisRange(value, d.getMaxBin());
        Serial.print(F("OK complex_minbin="));
        Serial.println(value);
        return true;
    }
    if (strncmp(cmd, "set complex_maxbin ", 19) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 19);
        ComplexDomainDetector& d = audioCtrl_->getEnsemble().getComplexDomain();
        d.setAnalysisRange(d.getMinBin(), value);
        Serial.print(F("OK complex_maxbin="));
        Serial.println(value);
        return true;
    }
    if (strcmp(cmd, "show complex_minbin") == 0 || strcmp(cmd, "complex_minbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("complex_minbin="));
        Serial.println(audioCtrl_->getEnsemble().getComplexDomain().getMinBin());
        return true;
    }
    if (strcmp(cmd, "show complex_maxbin") == 0 || strcmp(cmd, "complex_maxbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("complex_maxbin="));
        Serial.println(audioCtrl_->getEnsemble().getComplexDomain().getMaxBin());
        return true;
    }

    return false;
}

// === LOG LEVEL COMMANDS ===
bool SerialConsole::handleLogCommand(const char* cmd) {
    // "log" - show current level
    if (strcmp(cmd, "log") == 0) {
        Serial.print(F("Log level: "));
        switch (logLevel_) {
            case LogLevel::OFF:   Serial.println(F("off")); break;
            case LogLevel::ERROR: Serial.println(F("error")); break;
            case LogLevel::WARN:  Serial.println(F("warn")); break;
            case LogLevel::INFO:  Serial.println(F("info")); break;
            case LogLevel::DEBUG: Serial.println(F("debug")); break;
        }
        return true;
    }

    // "log off" - disable logging
    if (strcmp(cmd, "log off") == 0) {
        logLevel_ = LogLevel::OFF;
        Serial.println(F("OK log off"));
        return true;
    }

    // "log error" - errors only
    if (strcmp(cmd, "log error") == 0) {
        logLevel_ = LogLevel::ERROR;
        Serial.println(F("OK log error"));
        return true;
    }

    // "log warn" - warnings and errors
    if (strcmp(cmd, "log warn") == 0) {
        logLevel_ = LogLevel::WARN;
        Serial.println(F("OK log warn"));
        return true;
    }

    // "log info" - info and above (default)
    if (strcmp(cmd, "log info") == 0) {
        logLevel_ = LogLevel::INFO;
        Serial.println(F("OK log info"));
        return true;
    }

    // "log debug" - all messages
    if (strcmp(cmd, "log debug") == 0) {
        logLevel_ = LogLevel::DEBUG;
        Serial.println(F("OK log debug"));
        return true;
    }

    return false;
}

// === DEBUG CHANNEL COMMANDS ===
// Controls per-subsystem JSON debug output independently from log levels
bool SerialConsole::handleDebugCommand(const char* cmd) {
    // "debug" - show enabled channels
    if (strcmp(cmd, "debug") == 0) {
        Serial.println(F("Debug channels:"));
        Serial.print(F("  transient:  ")); Serial.println(isDebugChannelEnabled(DebugChannel::TRANSIENT) ? F("ON") : F("off"));
        Serial.print(F("  rhythm:     ")); Serial.println(isDebugChannelEnabled(DebugChannel::RHYTHM) ? F("ON") : F("off"));
        Serial.print(F("  audio:      ")); Serial.println(isDebugChannelEnabled(DebugChannel::AUDIO) ? F("ON") : F("off"));
        Serial.print(F("  generator:  ")); Serial.println(isDebugChannelEnabled(DebugChannel::GENERATOR) ? F("ON") : F("off"));
        Serial.print(F("  ensemble:   ")); Serial.println(isDebugChannelEnabled(DebugChannel::ENSEMBLE) ? F("ON") : F("off"));
        return true;
    }

    // Helper lambda to parse channel name
    auto parseChannel = [](const char* name) -> DebugChannel {
        if (strcmp(name, "transient") == 0)  return DebugChannel::TRANSIENT;
        if (strcmp(name, "rhythm") == 0)     return DebugChannel::RHYTHM;
        if (strcmp(name, "audio") == 0)      return DebugChannel::AUDIO;
        if (strcmp(name, "generator") == 0)  return DebugChannel::GENERATOR;
        if (strcmp(name, "ensemble") == 0)   return DebugChannel::ENSEMBLE;
        if (strcmp(name, "all") == 0)        return DebugChannel::ALL;
        return DebugChannel::NONE;
    };

    // "debug <channel> on" or "debug <channel> off"
    // Also handles "debug all on/off" via parseChannel returning ALL
    if (strncmp(cmd, "debug ", 6) == 0) {
        const char* rest = cmd + 6;
        char channelName[16] = {0};
        const char* space = strchr(rest, ' ');

        if (space && static_cast<size_t>(space - rest) < sizeof(channelName)) {
            strncpy(channelName, rest, space - rest);
            channelName[space - rest] = '\0';

            DebugChannel channel = parseChannel(channelName);
            if (channel == DebugChannel::NONE) {
                Serial.print(F("Unknown channel: "));
                Serial.println(channelName);
                Serial.println(F("Valid: transient, rhythm, audio, generator, ensemble, all"));
                return true;
            }

            const char* action = space + 1;
            if (strcmp(action, "on") == 0) {
                enableDebugChannel(channel);
                Serial.print(F("OK debug "));
                Serial.print(channelName);
                Serial.println(F(" on"));
                return true;
            } else if (strcmp(action, "off") == 0) {
                disableDebugChannel(channel);
                Serial.print(F("OK debug "));
                Serial.print(channelName);
                Serial.println(F(" off"));
                return true;
            } else {
                Serial.print(F("Invalid action: "));
                Serial.println(action);
                Serial.println(F("Use 'on' or 'off'"));
                return true;
            }
        }

        Serial.println(F("Usage: debug <channel> on|off"));
        Serial.println(F("Channels: transient, rhythm, audio, generator, ensemble, all"));
        return true;
    }

    return false;
}

// === BEAT TRACKING COMMANDS ===
bool SerialConsole::handleBeatTrackingCommand(const char* cmd) {
    if (!audioCtrl_) {
        Serial.println(F("Audio controller not available"));
        return false;
    }

    // "show beat" - show CBSS beat tracking state
    if (strcmp(cmd, "show beat") == 0) {
        Serial.println(F("=== CBSS Beat Tracker ==="));
        Serial.print(F("BPM: "));
        Serial.println(audioCtrl_->getCurrentBpm(), 1);
        Serial.print(F("Phase: "));
        Serial.println(audioCtrl_->getControl().phase, 3);
        Serial.print(F("Confidence: "));
        Serial.println(audioCtrl_->getCbssConfidence(), 3);
        Serial.print(F("Beat Count: "));
        Serial.println(audioCtrl_->getBeatCount());
        Serial.print(F("Beat Period (samples): "));
        Serial.println(audioCtrl_->getBeatPeriodSamples());
        Serial.print(F("Periodicity: "));
        Serial.println(audioCtrl_->getPeriodicityStrength(), 3);
        Serial.print(F("Stability: "));
        Serial.println(audioCtrl_->getBeatStability(), 3);
        Serial.print(F("Onset Density: "));
        Serial.print(audioCtrl_->getOnsetDensity(), 1);
        Serial.println(F(" /s"));
        Serial.println();
        return true;
    }

    // "json rhythm" - output rhythm tracking state as JSON (for test automation)
    if (strcmp(cmd, "json rhythm") == 0) {
        Serial.print(F("{\"bpm\":"));
        Serial.print(audioCtrl_->getCurrentBpm(), 1);
        Serial.print(F(",\"periodicityStrength\":"));
        Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
        Serial.print(F(",\"beatStability\":"));
        Serial.print(audioCtrl_->getBeatStability(), 3);
        Serial.print(F(",\"tempoVelocity\":"));
        Serial.print(audioCtrl_->getTempoVelocity(), 2);
        Serial.print(F(",\"nextBeatMs\":"));
        Serial.print(audioCtrl_->getNextBeatMs());
        Serial.print(F(",\"bayesBestConf\":"));
        Serial.print(audioCtrl_->getBayesBestConf(), 3);
        Serial.print(F(",\"phase\":"));
        Serial.print(audioCtrl_->getControl().phase, 3);
        Serial.print(F(",\"rhythmStrength\":"));
        Serial.print(audioCtrl_->getControl().rhythmStrength, 3);
        Serial.print(F(",\"cbssConfidence\":"));
        Serial.print(audioCtrl_->getCbssConfidence(), 3);
        Serial.print(F(",\"beatCount\":"));
        Serial.print(audioCtrl_->getBeatCount());
        Serial.print(F(",\"onsetDensity\":"));
        Serial.print(audioCtrl_->getOnsetDensity(), 1);
        Serial.println(F("}"));
        return true;
    }

    // "json beat" - output CBSS beat tracker state as JSON
    if (strcmp(cmd, "json beat") == 0) {
        Serial.print(F("{\"bpm\":"));
        Serial.print(audioCtrl_->getCurrentBpm(), 1);
        Serial.print(F(",\"phase\":"));
        Serial.print(audioCtrl_->getControl().phase, 3);
        Serial.print(F(",\"periodicity\":"));
        Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
        Serial.print(F(",\"confidence\":"));
        Serial.print(audioCtrl_->getCbssConfidence(), 3);
        Serial.print(F(",\"beatCount\":"));
        Serial.print(audioCtrl_->getBeatCount());
        Serial.print(F(",\"beatPeriod\":"));
        Serial.print(audioCtrl_->getBeatPeriodSamples());
        Serial.print(F(",\"stability\":"));
        Serial.print(audioCtrl_->getBeatStability(), 3);
        Serial.println(F("}"));
        return true;
    }

    // "show spectral" - show spectral processing (compressor + whitening) state
    if (strcmp(cmd, "show spectral") == 0) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getEnsemble().getSpectral();
        Serial.println(F("=== Spectral Processing ==="));
        Serial.println(F("-- Compressor --"));
        Serial.print(F("  Enabled: ")); Serial.println(spectral.compressorEnabled ? "yes" : "no");
        Serial.print(F("  Threshold: ")); Serial.print(spectral.compThresholdDb, 1); Serial.println(F(" dB"));
        Serial.print(F("  Ratio: ")); Serial.print(spectral.compRatio, 1); Serial.println(F(":1"));
        Serial.print(F("  Knee: ")); Serial.print(spectral.compKneeDb, 1); Serial.println(F(" dB"));
        Serial.print(F("  Makeup: ")); Serial.print(spectral.compMakeupDb, 1); Serial.println(F(" dB"));
        Serial.print(F("  Attack: ")); Serial.print(spectral.compAttackTau * 1000.0f, 1); Serial.println(F(" ms"));
        Serial.print(F("  Release: ")); Serial.print(spectral.compReleaseTau, 2); Serial.println(F(" s"));
        Serial.print(F("  Frame RMS: ")); Serial.print(spectral.getFrameRmsDb(), 1); Serial.println(F(" dB"));
        Serial.print(F("  Smoothed Gain: ")); Serial.print(spectral.getSmoothedGainDb(), 2); Serial.println(F(" dB"));
        Serial.println(F("-- Whitening --"));
        Serial.print(F("  Enabled: ")); Serial.println(spectral.whitenEnabled ? "yes" : "no");
        Serial.print(F("  Decay: ")); Serial.println(spectral.whitenDecay, 4);
        Serial.print(F("  Floor: ")); Serial.println(spectral.whitenFloor, 4);
        Serial.println();
        return true;
    }

    // "json spectral" - spectral processing state as JSON (for test automation)
    if (strcmp(cmd, "json spectral") == 0) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getEnsemble().getSpectral();
        Serial.print(F("{\"compEnabled\":"));
        Serial.print(spectral.compressorEnabled ? 1 : 0);
        Serial.print(F(",\"compThreshDb\":"));
        Serial.print(spectral.compThresholdDb, 1);
        Serial.print(F(",\"compRatio\":"));
        Serial.print(spectral.compRatio, 1);
        Serial.print(F(",\"compKneeDb\":"));
        Serial.print(spectral.compKneeDb, 1);
        Serial.print(F(",\"compMakeupDb\":"));
        Serial.print(spectral.compMakeupDb, 1);
        Serial.print(F(",\"compAttackMs\":"));
        Serial.print(spectral.compAttackTau * 1000.0f, 2);
        Serial.print(F(",\"compReleaseS\":"));
        Serial.print(spectral.compReleaseTau, 2);
        Serial.print(F(",\"rmsDb\":"));
        Serial.print(spectral.getFrameRmsDb(), 1);
        Serial.print(F(",\"gainDb\":"));
        Serial.print(spectral.getSmoothedGainDb(), 2);
        Serial.print(F(",\"whitenEnabled\":"));
        Serial.print(spectral.whitenEnabled ? 1 : 0);
        Serial.print(F(",\"whitenDecay\":"));
        Serial.print(spectral.whitenDecay, 4);
        Serial.print(F(",\"whitenFloor\":"));
        Serial.print(spectral.whitenFloor, 4);
        Serial.println(F("}"));
        return true;
    }

    return false;
}

// === LOGGING HELPERS ===
void SerialConsole::logDebug(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::DEBUG) {
        Serial.print(F("[DEBUG] "));
        Serial.println(msg);
    }
}

void SerialConsole::logInfo(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::INFO) {
        Serial.print(F("[INFO] "));
        Serial.println(msg);
    }
}

void SerialConsole::logWarn(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::WARN) {
        Serial.print(F("[WARN] "));
        Serial.println(msg);
    }
}

void SerialConsole::logError(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::ERROR) {
        Serial.print(F("[ERROR] "));
        Serial.println(msg);
    }
}
