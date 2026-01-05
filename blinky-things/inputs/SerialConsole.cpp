#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../audio/AudioController.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../types/Version.h"
#include "../render/RenderPipeline.h"
#include "../effects/HueRotationEffect.h"

extern const DeviceConfig& config;

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

// Static debug channels - default to NONE (no debug output)
DebugChannel SerialConsole::debugChannels_ = DebugChannel::NONE;

// File-scope storage for effect settings (accessible from both register and sync functions)
static float effectHueShift_ = 0.0f;
static float effectRotationSpeed_ = 0.0f;

// New constructor with RenderPipeline
SerialConsole::SerialConsole(RenderPipeline* pipeline, AdaptiveMic* mic)
    : pipeline_(pipeline), fireGenerator_(nullptr), waterGenerator_(nullptr),
      lightningGenerator_(nullptr), hueEffect_(nullptr), mic_(mic),
      battery_(nullptr), audioCtrl_(nullptr), configStorage_(nullptr) {
    instance_ = this;
    // Get generator pointers from pipeline
    if (pipeline_) {
        fireGenerator_ = pipeline_->getFireGenerator();
        waterGenerator_ = pipeline_->getWaterGenerator();
        lightningGenerator_ = pipeline_->getLightningGenerator();
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

// === FIRE SETTINGS ===
void SerialConsole::registerFireSettings(FireParams* fp) {
    if (!fp) return;

    settings_.registerUint8("cooling", &fp->baseCooling, "fire",
        "Base cooling rate", 0, 255);
    settings_.registerFloat("sparkchance", &fp->sparkChance, "fire",
        "Probability of sparks", 0.0f, 1.0f);
    settings_.registerUint8("sparkheatmin", &fp->sparkHeatMin, "fire",
        "Min spark heat", 0, 255);
    settings_.registerUint8("sparkheatmax", &fp->sparkHeatMax, "fire",
        "Max spark heat", 0, 255);
    settings_.registerFloat("audiosparkboost", &fp->audioSparkBoost, "fire",
        "Audio influence on sparks", 0.0f, 1.0f);
    settings_.registerInt8("coolingaudiobias", &fp->coolingAudioBias, "fire",
        "Audio cooling bias", -128, 127);
    settings_.registerUint8("bottomrows", &fp->bottomRowsForSparks, "fire",
        "Spark injection rows", 1, 8);
    settings_.registerUint8("burstsparks", &fp->burstSparks, "fire",
        "Sparks per burst", 1, 20);
    settings_.registerUint16("suppressionms", &fp->suppressionMs, "fire",
        "Burst suppression time", 50, 1000);
    settings_.registerFloat("heatdecay", &fp->heatDecay, "fire",
        "Heat decay rate", 0.5f, 0.99f);
    settings_.registerUint8("emberheatmax", &fp->emberHeatMax, "fire",
        "Max ember heat", 0, 50);
    settings_.registerUint8("spreaddistance", &fp->spreadDistance, "fire",
        "Heat spread distance", 1, 24);
    settings_.registerFloat("embernoisespeed", &fp->emberNoiseSpeed, "fire",
        "Ember animation speed", 0.0001f, 0.002f);
}

// === MUSIC MODE FIRE SETTINGS ===
// Controls fire behavior when music mode is active (beat-synced)
void SerialConsole::registerFireMusicSettings(FireParams* fp) {
    if (!fp) return;

    settings_.registerFloat("musicemberpulse", &fp->musicEmberPulse, "firemusic",
        "Ember pulse intensity on beat", 0.0f, 1.0f);
    settings_.registerFloat("musicsparkpulse", &fp->musicSparkPulse, "firemusic",
        "Spark heat pulse on beat", 0.0f, 1.0f);
    settings_.registerFloat("musiccoolpulse", &fp->musicCoolingPulse, "firemusic",
        "Cooling oscillation amplitude", 0.0f, 30.0f);
}

// === ORGANIC MODE FIRE SETTINGS ===
// Controls fire behavior when music mode is NOT active
void SerialConsole::registerFireOrganicSettings(FireParams* fp) {
    if (!fp) return;

    settings_.registerFloat("organicsparkchance", &fp->organicSparkChance, "fireorganic",
        "Baseline random spark rate", 0.0f, 0.5f);
    settings_.registerFloat("organictransmin", &fp->organicTransientMin, "fireorganic",
        "Min transient to trigger burst", 0.0f, 1.0f);
    settings_.registerFloat("organicaudiomix", &fp->organicAudioMix, "fireorganic",
        "Audio influence in organic mode", 0.0f, 1.0f);
    settings_.registerBool("organicburstsuppress", &fp->organicBurstSuppress, "fireorganic",
        "Suppress after bursts in organic mode");
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

    // Basic rhythm activation and output modulation
    settings_.registerFloat("musicthresh", &audioCtrl_->activationThreshold, "rhythm",
        "Rhythm activation threshold (0-1)", 0.0f, 1.0f);
    settings_.registerFloat("phaseadapt", &audioCtrl_->phaseAdaptRate, "rhythm",
        "Phase adaptation rate (0-1)", 0.01f, 1.0f);
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

    // Tempo prior (reduces half-time/double-time confusion)
    settings_.registerBool("priorenabled", &audioCtrl_->tempoPriorEnabled, "tempoprior",
        "Enable tempo prior weighting");
    settings_.registerFloat("priorcenter", &audioCtrl_->tempoPriorCenter, "tempoprior",
        "Prior center BPM", 60.0f, 180.0f);
    settings_.registerFloat("priorwidth", &audioCtrl_->tempoPriorWidth, "tempoprior",
        "Prior width (sigma BPM)", 10.0f, 80.0f);
    settings_.registerFloat("priorstrength", &audioCtrl_->tempoPriorStrength, "tempoprior",
        "Prior blend strength", 0.0f, 1.0f);

    // Beat stability tracking
    settings_.registerFloat("stabilitywin", &audioCtrl_->stabilityWindowBeats, "stability",
        "Stability window (beats)", 4.0f, 16.0f);

    // Beat lookahead (anticipatory effects)
    settings_.registerFloat("lookahead", &audioCtrl_->beatLookaheadMs, "lookahead",
        "Beat lookahead (ms)", 0.0f, 200.0f);

    // Continuous tempo estimation
    settings_.registerFloat("temposmooth", &audioCtrl_->tempoSmoothingFactor, "tempo",
        "Tempo smoothing factor", 0.5f, 0.99f);
    settings_.registerFloat("tempochgthresh", &audioCtrl_->tempoChangeThreshold, "tempo",
        "Tempo change threshold", 0.01f, 0.5f);

    // Multi-hypothesis tracker parameters
    MultiHypothesisTracker& mh = audioCtrl_->getMultiHypothesis();

    // Peak detection
    settings_.registerFloat("minpeakstr", &mh.minPeakStrength, "hypothesis",
        "Min autocorr peak strength", 0.1f, 0.8f);
    settings_.registerFloat("minrelheight", &mh.minRelativePeakHeight, "hypothesis",
        "Min relative peak height", 0.5f, 1.0f);

    // Hypothesis matching
    settings_.registerFloat("bpmmatchtol", &mh.bpmMatchTolerance, "hypothesis",
        "BPM match tolerance (fraction)", 0.01f, 0.2f);

    // Promotion
    settings_.registerFloat("promothresh", &mh.promotionThreshold, "hypothesis",
        "Confidence advantage for promotion", 0.05f, 0.5f);
    settings_.registerUint16("minbeats", &mh.minBeatsBeforePromotion, "hypothesis",
        "Min beats before promotion", 4, 32);

    // Decay
    settings_.registerFloat("phrasehalf", &mh.phraseHalfLifeBeats, "hypothesis",
        "Phrase decay half-life (beats)", 8.0f, 64.0f);
    settings_.registerFloat("minstr", &mh.minStrengthToKeep, "hypothesis",
        "Min strength to keep hypothesis", 0.05f, 0.3f);
    settings_.registerUint32("silencegrace", &mh.silenceGracePeriodMs, "hypothesis",
        "Silence grace period (ms)", 1000, 10000);
    settings_.registerFloat("silencehalf", &mh.silenceDecayHalfLifeSec, "hypothesis",
        "Silence decay half-life (s)", 2.0f, 15.0f);

    // Confidence weighting
    settings_.registerFloat("strweight", &mh.strengthWeight, "hypothesis",
        "Strength weight in confidence", 0.0f, 1.0f);
    settings_.registerFloat("consweight", &mh.consistencyWeight, "hypothesis",
        "Consistency weight in confidence", 0.0f, 1.0f);
    settings_.registerFloat("longweight", &mh.longevityWeight, "hypothesis",
        "Longevity weight in confidence", 0.0f, 1.0f);
}

void SerialConsole::update() {
    // Handle incoming commands
    if (Serial.available()) {
        static char buf[128];
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

    // Check for hypothesis debug command (uses "set hypodebug")
    if (handleHypothesisCommand(cmd)) {
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
    // NOTE: handleEnsembleCommand and handleHypothesisCommand are called
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
        Serial.print(F("{\"device\":\""));
        Serial.print(config.deviceName);
        Serial.print(F("\",\"version\":\""));
        Serial.print(F(BLINKY_VERSION_STRING));
        Serial.print(F("\",\"width\":"));
        Serial.print(config.matrix.width);
        Serial.print(F(",\"height\":"));
        Serial.print(config.matrix.height);
        Serial.print(F(",\"leds\":"));
        Serial.print(config.matrix.width * config.matrix.height);
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
            Serial.println(audio.hasRhythm() ? F("YES") : F("NO"));
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
            Serial.print(F("Tempo Prior: "));
            Serial.print(audioCtrl_->tempoPriorEnabled ? F("ON") : F("OFF"));
            Serial.print(F(" (center="));
            Serial.print(audioCtrl_->tempoPriorCenter, 0);
            Serial.print(F(", weight="));
            Serial.print(audioCtrl_->getLastTempoPriorWeight(), 2);
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
            Serial.println(F("/6"));
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
        if (configStorage_ && fireGenerator_ && mic_) {
            configStorage_->saveConfiguration(fireGenerator_->getParams(), *mic_, audioCtrl_);
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "load") == 0) {
        if (configStorage_ && fireGenerator_ && mic_) {
            configStorage_->loadConfiguration(fireGenerator_->getParamsMutable(), *mic_, audioCtrl_);
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

    return false;
}

void SerialConsole::restoreDefaults() {
    // Restore fire defaults
    if (fireGenerator_) {
        fireGenerator_->resetToDefaults();
    }

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
        audioCtrl_->phaseAdaptRate = 0.15f;
        audioCtrl_->pulseBoostOnBeat = 1.3f;
        audioCtrl_->pulseSuppressOffBeat = 0.6f;
        audioCtrl_->energyBoostOnBeat = 0.3f;
        audioCtrl_->bpmMin = 60.0f;
        audioCtrl_->bpmMax = 200.0f;
    }

    // Restore water defaults (settings now point directly to generator's params)
    if (waterGenerator_) {
        waterGenerator_->resetToDefaults();
    }

    // Restore lightning defaults (settings now point directly to generator's params)
    if (lightningGenerator_) {
        lightningGenerator_->resetToDefaults();
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
            Serial.println(F("Use: fire, water, lightning"));
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

// === WATER SETTINGS ===
void SerialConsole::registerWaterSettings(WaterParams* wp) {
    if (!wp) return;

    settings_.registerUint8("waterflow", &wp->baseFlow, "water",
        "Base flow speed", 0, 255);
    settings_.registerUint8("wavemin", &wp->waveHeightMin, "water",
        "Min wave height", 0, 255);
    settings_.registerUint8("wavemax", &wp->waveHeightMax, "water",
        "Max wave height", 0, 255);
    settings_.registerFloat("wavechance", &wp->waveChance, "water",
        "Probability of new wave", 0.0f, 1.0f);
    settings_.registerFloat("audiowaveboost", &wp->audioWaveBoost, "water",
        "Audio boost for waves", 0.0f, 1.0f);
    settings_.registerUint8("audioflowmax", &wp->audioFlowBoostMax, "water",
        "Max flow boost from audio", 0, 255);
    settings_.registerInt8("flowaudiobias", &wp->flowAudioBias, "water",
        "Flow speed audio bias", -128, 127);
}

// === LIGHTNING SETTINGS ===
void SerialConsole::registerLightningSettings(LightningParams* lp) {
    if (!lp) return;

    settings_.registerUint8("lightfade", &lp->baseFade, "lightning",
        "Base fade speed", 0, 255);
    settings_.registerUint8("boltmin", &lp->boltIntensityMin, "lightning",
        "Min bolt intensity", 0, 255);
    settings_.registerUint8("boltmax", &lp->boltIntensityMax, "lightning",
        "Max bolt intensity", 0, 255);
    settings_.registerFloat("boltchance", &lp->boltChance, "lightning",
        "Probability of new bolt", 0.0f, 1.0f);
    settings_.registerFloat("audioboltboost", &lp->audioBoltBoost, "lightning",
        "Audio boost for bolts", 0.0f, 1.0f);
    settings_.registerUint8("audiointensitymax", &lp->audioIntensityBoostMax, "lightning",
        "Max intensity boost from audio", 0, 255);
    settings_.registerInt8("fadeaudiobias", &lp->fadeAudioBias, "lightning",
        "Fade speed audio bias", -128, 127);
    settings_.registerUint8("branchchance", &lp->branchChance, "lightning",
        "Branch probability (%)", 0, 100);
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
        // agree → detector agreement : 0-6 (how many detectors fired)
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
        }

        Serial.print(F("}"));

        // AudioController telemetry (unified rhythm tracking)
        // Format: "m":{"a":1,"bpm":125.3,"ph":0.45,"str":0.82,"e":0.5,"p":0.8}
        // a = rhythm active, bpm = tempo, ph = phase, str = rhythm strength
        // e = energy, p = pulse
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();
            Serial.print(F(",\"m\":{\"a\":"));
            Serial.print(audio.hasRhythm() ? 1 : 0);
            Serial.print(F(",\"bpm\":"));
            Serial.print(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F(",\"ph\":"));
            Serial.print(audio.phase, 2);
            Serial.print(F(",\"str\":"));
            Serial.print(audio.rhythmStrength, 2);
            Serial.print(F(",\"e\":"));
            Serial.print(audio.energy, 2);
            Serial.print(F(",\"p\":"));
            Serial.print(audio.pulse, 2);

            // Debug mode: add internal state for tuning
            if (streamDebug_) {
                Serial.print(F(",\"ps\":"));
                Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
            }

            Serial.print(F("}"));
        }

        // LED brightness telemetry
        // Format: "led":{"tot":12345,"pct":37.5}
        // tot = total heat (sum of all heat values)
        // pct = brightness percent (0-100)
        if (fireGenerator_) {
            Serial.print(F(",\"led\":{\"tot\":"));
            Serial.print(fireGenerator_->getTotalHeat());
            Serial.print(F(",\"pct\":"));
            Serial.print(fireGenerator_->getBrightnessPercent(), 1);
            Serial.print(F("}"));
        }

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
        for (int i = 0; i <= 6; i++) {
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
        Serial.println(F("/6"));
        Serial.print(F("  Dominant: "));
        Serial.println(getDetectorName(static_cast<DetectorType>(output.dominantDetector)));
        Serial.println(F("\nFusion Parameters:"));
        Serial.print(F("  cooldown: "));
        Serial.print(fusion.getCooldownMs());
        Serial.println(F(" ms"));
        Serial.print(F("  minconf: "));
        Serial.println(fusion.getMinConfidence(), 3);
        Serial.print(F("  minlevel: "));
        Serial.println(fusion.getMinAudioLevel(), 3);
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
                Serial.println(F("'. Use: drummer, spectral, hfc, bass, complex, mel"));
            }
        } else {
            Serial.println(F("Usage: set detector_enable <type> <0|1>"));
            Serial.println(F("Types: drummer, spectral, hfc, bass, complex, mel"));
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
                Serial.println(F("'. Use: drummer, spectral, hfc, bass, complex, mel"));
            }
        } else {
            Serial.println(F("Usage: set detector_weight <type> <value>"));
            Serial.println(F("Types: drummer, spectral, hfc, bass, complex, mel"));
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
                Serial.println(F("'. Use: drummer, spectral, hfc, bass, complex, mel"));
            }
        } else {
            Serial.println(F("Usage: set detector_thresh <type> <value>"));
            Serial.println(F("Types: drummer, spectral, hfc, bass, complex, mel"));
        }
        return true;
    }

    // Handle "set agree_<n> <value>" for agreement boost values
    if (strncmp(cmd, "set agree_", 10) == 0) {
        if (!audioCtrl_) {
            Serial.println(F("ERROR: AudioController not available"));
            return true;
        }
        // Parse "set agree_N value" where N is 0-6
        // Format: "set agree_" (10 chars) + digit + space + value
        const char* args = cmd + 10;
        if (args[0] >= '0' && args[0] <= '6' && args[1] == ' ') {
            int n = args[0] - '0';
            float value = atof(args + 2);
            // Get current boosts, modify one, set all
            EnsembleFusion& fusion = audioCtrl_->getEnsemble().getFusion();
            float boosts[7];
            for (int i = 0; i <= 6; i++) {
                boosts[i] = fusion.getAgreementBoost(i);
            }
            boosts[n] = value;
            fusion.setAgreementBoosts(boosts);
            Serial.print(F("OK agree_"));
            Serial.print(n);
            Serial.print(F("="));
            Serial.println(value, 2);
        } else {
            Serial.println(F("Usage: set agree_<0-6> <value>"));
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

    // SpectralFlux: minbin, maxbin
    if (strncmp(cmd, "set spectral_minbin ", 20) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 20);
        SpectralFluxDetector& d = audioCtrl_->getEnsemble().getSpectralFlux();
        d.setAnalysisRange(value, d.getMaxBin());
        Serial.print(F("OK spectral_minbin="));
        Serial.println(value);
        return true;
    }
    if (strncmp(cmd, "set spectral_maxbin ", 20) == 0) {
        if (!audioCtrl_) return true;
        int value = atoi(cmd + 20);
        SpectralFluxDetector& d = audioCtrl_->getEnsemble().getSpectralFlux();
        d.setAnalysisRange(d.getMinBin(), value);
        Serial.print(F("OK spectral_maxbin="));
        Serial.println(value);
        return true;
    }
    if (strcmp(cmd, "show spectral_minbin") == 0 || strcmp(cmd, "spectral_minbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("spectral_minbin="));
        Serial.println(audioCtrl_->getEnsemble().getSpectralFlux().getMinBin());
        return true;
    }
    if (strcmp(cmd, "show spectral_maxbin") == 0 || strcmp(cmd, "spectral_maxbin") == 0) {
        if (!audioCtrl_) return true;
        Serial.print(F("spectral_maxbin="));
        Serial.println(audioCtrl_->getEnsemble().getSpectralFlux().getMaxBin());
        return true;
    }

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
        Serial.print(F("  hypothesis: ")); Serial.println(isDebugChannelEnabled(DebugChannel::HYPOTHESIS) ? F("ON") : F("off"));
        Serial.print(F("  audio:      ")); Serial.println(isDebugChannelEnabled(DebugChannel::AUDIO) ? F("ON") : F("off"));
        Serial.print(F("  generator:  ")); Serial.println(isDebugChannelEnabled(DebugChannel::GENERATOR) ? F("ON") : F("off"));
        Serial.print(F("  ensemble:   ")); Serial.println(isDebugChannelEnabled(DebugChannel::ENSEMBLE) ? F("ON") : F("off"));
        return true;
    }

    // Helper lambda to parse channel name
    auto parseChannel = [](const char* name) -> DebugChannel {
        if (strcmp(name, "transient") == 0)  return DebugChannel::TRANSIENT;
        if (strcmp(name, "rhythm") == 0)     return DebugChannel::RHYTHM;
        if (strcmp(name, "hypothesis") == 0) return DebugChannel::HYPOTHESIS;
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
                Serial.println(F("Valid: transient, rhythm, hypothesis, audio, generator, ensemble, all"));
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
            }
        }

        Serial.println(F("Usage: debug <channel> on|off"));
        Serial.println(F("Channels: transient, rhythm, hypothesis, audio, generator, ensemble, all"));
        return true;
    }

    return false;
}

// === MULTI-HYPOTHESIS TRACKING COMMANDS ===
bool SerialConsole::handleHypothesisCommand(const char* cmd) {
    if (!audioCtrl_) {
        Serial.println(F("Audio controller not available"));
        return false;
    }

    MultiHypothesisTracker& tracker = audioCtrl_->getMultiHypothesis();

    // "show hypotheses" or "show hypo" - print all active hypotheses
    if (strcmp(cmd, "show hypotheses") == 0 || strcmp(cmd, "show hypo") == 0) {
        Serial.println(F("=== Multi-Hypothesis Tracker ==="));

        bool anyActive = false;
        for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
            const TempoHypothesis& hypo = tracker.hypotheses[i];
            if (hypo.active) {
                anyActive = true;
                Serial.print(F("Slot "));
                Serial.print(i);
                Serial.print(i == 0 ? F(" [PRIMARY]: ") :
                           i == 1 ? F(" [SECONDARY]: ") :
                           i == 2 ? F(" [TERTIARY]: ") : F(" [CANDIDATE]: "));
                Serial.print(hypo.bpm, 1);
                Serial.print(F(" BPM, phase="));
                Serial.print(hypo.phase, 2);
                Serial.print(F(", str="));
                Serial.print(hypo.strength, 2);
                Serial.print(F(", conf="));
                Serial.print(hypo.confidence, 2);
                Serial.print(F(", beats="));
                Serial.println(hypo.beatCount);
            }
        }

        if (!anyActive) {
            Serial.println(F("No active hypotheses"));
        }

        Serial.println();
        return true;
    }

    // "show primary" - print primary hypothesis only
    if (strcmp(cmd, "show primary") == 0) {
        const TempoHypothesis& primary = tracker.getPrimary();
        Serial.println(F("=== Primary Hypothesis ==="));
        if (primary.active) {
            Serial.print(F("BPM: "));
            Serial.println(primary.bpm, 1);
            Serial.print(F("Phase: "));
            Serial.println(primary.phase, 2);
            Serial.print(F("Strength: "));
            Serial.println(primary.strength, 2);
            Serial.print(F("Confidence: "));
            Serial.println(primary.confidence, 2);
            Serial.print(F("Beat Count: "));
            Serial.println(primary.beatCount);
        } else {
            Serial.println(F("No active primary hypothesis"));
        }
        Serial.println();
        return true;
    }

    // "set hypodebug <0-3>" - set hypothesis debug level
    if (strncmp(cmd, "set hypodebug ", 14) == 0) {
        int level = atoi(cmd + 14);
        if (level >= 0 && level <= 3) {
            tracker.debugLevel = static_cast<HypothesisDebugLevel>(level);
            Serial.print(F("OK hypodebug="));
            Serial.print(level);
            Serial.print(F(" ("));
            switch (tracker.debugLevel) {
                case HypothesisDebugLevel::OFF: Serial.print(F("OFF")); break;
                case HypothesisDebugLevel::EVENTS: Serial.print(F("EVENTS")); break;
                case HypothesisDebugLevel::SUMMARY: Serial.print(F("SUMMARY")); break;
                case HypothesisDebugLevel::DETAILED: Serial.print(F("DETAILED")); break;
            }
            Serial.println(F(")"));
        } else {
            Serial.println(F("ERROR: hypodebug must be 0-3 (OFF/EVENTS/SUMMARY/DETAILED)"));
        }
        return true;
    }

    // "get hypodebug" - show current debug level
    if (strcmp(cmd, "get hypodebug") == 0) {
        Serial.print(F("hypodebug="));
        Serial.print(static_cast<int>(tracker.debugLevel));
        Serial.print(F(" ("));
        switch (tracker.debugLevel) {
            case HypothesisDebugLevel::OFF: Serial.print(F("OFF")); break;
            case HypothesisDebugLevel::EVENTS: Serial.print(F("EVENTS")); break;
            case HypothesisDebugLevel::SUMMARY: Serial.print(F("SUMMARY")); break;
            case HypothesisDebugLevel::DETAILED: Serial.print(F("DETAILED")); break;
        }
        Serial.println(F(")"));
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
        Serial.print(F(",\"tempoPriorWeight\":"));
        Serial.print(audioCtrl_->getLastTempoPriorWeight(), 3);
        Serial.print(F(",\"phase\":"));
        Serial.print(audioCtrl_->getControl().phase, 3);
        Serial.print(F(",\"rhythmStrength\":"));
        Serial.print(audioCtrl_->getControl().rhythmStrength, 3);
        Serial.println(F("}"));
        return true;
    }

    // "json hypotheses" - output all hypotheses as JSON
    if (strcmp(cmd, "json hypotheses") == 0) {
        Serial.print(F("{\"hypotheses\":["));
        bool first = true;
        for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
            if (!first) Serial.print(F(","));
            first = false;

            const TempoHypothesis& h = tracker.hypotheses[i];
            Serial.print(F("{\"slot\":"));
            Serial.print(i);
            Serial.print(F(",\"active\":"));
            Serial.print(h.active ? F("true") : F("false"));
            Serial.print(F(",\"bpm\":"));
            Serial.print(h.bpm, 1);
            Serial.print(F(",\"phase\":"));
            Serial.print(h.phase, 3);
            Serial.print(F(",\"strength\":"));
            Serial.print(h.strength, 3);
            Serial.print(F(",\"confidence\":"));
            Serial.print(h.confidence, 3);
            Serial.print(F(",\"beatCount\":"));
            Serial.print(h.beatCount);
            Serial.print(F(",\"avgPhaseError\":"));
            Serial.print(h.avgPhaseError, 4);
            Serial.print(F(",\"priority\":"));
            Serial.print(h.priority);
            Serial.print(F("}"));
        }

        // Find primary hypothesis (priority == 0)
        int primaryIndex = 0;
        for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
            if (tracker.hypotheses[i].priority == 0) {
                primaryIndex = i;
                break;
            }
        }

        Serial.print(F("],\"primaryIndex\":"));
        Serial.print(primaryIndex);
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
