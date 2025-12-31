#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "../config/Presets.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../audio/AudioController.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../types/Version.h"

extern const DeviceConfig& config;

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

SerialConsole::SerialConsole(Fire* fireGen, AdaptiveMic* mic)
    : fireGenerator_(fireGen), mic_(mic), battery_(nullptr), audioCtrl_(nullptr), configStorage_(nullptr) {
    instance_ = this;
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
    registerAudioSettings();
    registerAgcSettings();
    registerTransientSettings();
    registerDetectionSettings();
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
void SerialConsole::registerTransientSettings() {
    if (!mic_) return;

    settings_.registerFloat("hitthresh", &mic_->transientThreshold, "transient",
        "Hit threshold (multiples of recent average)", 1.5f, 10.0f);
    settings_.registerFloat("attackmult", &mic_->attackMultiplier, "transient",
        "Attack multiplier (sudden rise ratio)", 1.1f, 2.0f);
    settings_.registerFloat("avgtau", &mic_->averageTau, "transient",
        "Recent average tracking time (s)", 0.1f, 5.0f);
    settings_.registerUint16("cooldown", &mic_->cooldownMs, "transient",
        "Cooldown between hits (ms)", 20, 500);

    // Adaptive threshold for low-level audio
    settings_.registerBool("adaptthresh", &mic_->adaptiveThresholdEnabled, "transient",
        "Enable adaptive threshold scaling");
    settings_.registerFloat("adaptminraw", &mic_->adaptiveMinRaw, "transient",
        "Raw level to start threshold scaling", 0.01f, 0.5f);
    settings_.registerFloat("adaptmaxscale", &mic_->adaptiveMaxScale, "transient",
        "Minimum threshold scale factor", 0.3f, 1.0f);
    settings_.registerFloat("adaptblend", &mic_->adaptiveBlendTau, "transient",
        "Adaptive threshold blend time (s)", 1.0f, 15.0f);
}

// === DETECTION MODE SETTINGS ===
// Different onset detection algorithms
void SerialConsole::registerDetectionSettings() {
    if (!mic_) return;

    settings_.registerUint8("detectmode", &mic_->detectionMode, "detection",
        "Algorithm (0=drummer,1=bass,2=hfc,3=flux,4=hybrid)", 0, 4);

    // Bass Band Filter parameters (mode 1)
    settings_.registerFloat("bassfreq", &mic_->bassFreq, "detection",
        "Bass filter cutoff freq (Hz)", 40.0f, 200.0f);
    settings_.registerFloat("bassq", &mic_->bassQ, "detection",
        "Bass filter Q factor", 0.5f, 3.0f);
    settings_.registerFloat("bassthresh", &mic_->bassThresh, "detection",
        "Bass detection threshold", 1.5f, 10.0f);

    // HFC parameters (mode 2)
    settings_.registerFloat("hfcweight", &mic_->hfcWeight, "detection",
        "HFC weighting factor", 0.5f, 5.0f);
    settings_.registerFloat("hfcthresh", &mic_->hfcThresh, "detection",
        "HFC detection threshold", 1.5f, 10.0f);

    // Spectral Flux parameters (mode 3)
    settings_.registerFloat("fluxthresh", &mic_->fluxThresh, "detection",
        "Spectral flux threshold", 1.0f, 10.0f);
    settings_.registerUint8("fluxbins", &mic_->fluxBins, "detection",
        "FFT bins to analyze", 4, 128);

    // Hybrid parameters (mode 4) - confidence weights
    settings_.registerFloat("hyfluxwt", &mic_->hybridFluxWeight, "detection",
        "Hybrid: flux-only weight", 0.1f, 1.0f);
    settings_.registerFloat("hydrumwt", &mic_->hybridDrumWeight, "detection",
        "Hybrid: drummer-only weight", 0.1f, 1.0f);
    settings_.registerFloat("hybothboost", &mic_->hybridBothBoost, "detection",
        "Hybrid: both-agree boost", 1.0f, 2.0f);
}

// === RHYTHM TRACKING SETTINGS (AudioController) ===
void SerialConsole::registerRhythmSettings() {
    if (!audioCtrl_) return;

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
    // Try settings registry first (handles set/get/show/list/categories/settings)
    if (settings_.handleCommand(cmd)) {
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
    if (handleJsonCommand(cmd)) return true;
    if (handleBatteryCommand(cmd)) return true;
    if (handleStreamCommand(cmd)) return true;
    if (handleTestCommand(cmd)) return true;
    if (handleAudioStatusCommand(cmd)) return true;
    if (handlePresetCommand(cmd)) return true;
    if (handleModeCommand(cmd)) return true;
    if (handleConfigCommand(cmd)) return true;
    return false;
}

// === JSON API COMMANDS (for web app) ===
bool SerialConsole::handleJsonCommand(const char* cmd) {
    if (strcmp(cmd, "json settings") == 0) {
        settings_.printSettingsJson();
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

    if (strcmp(cmd, "json presets") == 0) {
        Serial.print(F("{\"presets\":["));
        for (uint8_t i = 0; i < PresetManager::getPresetCount(); i++) {
            if (i > 0) Serial.print(',');
            Serial.print('"');
            Serial.print(PresetManager::getPresetName(static_cast<PresetId>(i)));
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
        } else {
            Serial.println(F("Audio controller not available"));
        }
        return true;
    }

    return false;
}

// === PRESET COMMANDS ===
bool SerialConsole::handlePresetCommand(const char* cmd) {
    if (strncmp(cmd, "preset ", 7) == 0) {
        if (!mic_) {
            Serial.println(F("ERROR: Mic not available"));
            return true;
        }
        const char* presetName = cmd + 7;
        PresetId id = PresetManager::parsePresetName(presetName);
        if (id != PresetId::NUM_PRESETS) {
            PresetManager::applyPreset(id, *mic_, audioCtrl_);
            Serial.print(F("OK "));
            Serial.println(PresetManager::getPresetName(id));
        } else {
            Serial.println(F("Unknown preset. Use: default"));
        }
        return true;
    }

    if (strcmp(cmd, "presets") == 0) {
        Serial.println(F("Available presets:"));
        Serial.println(F("  default - Production defaults (only preset)"));
        Serial.println(F("Note: Quiet mode auto-activates when AGC gain is maxed."));
        return true;
    }

    return false;
}

// === DETECTION MODE STATUS ===
bool SerialConsole::handleModeCommand(const char* cmd) {
    if (strcmp(cmd, "mode") == 0) {
        if (mic_) {
            uint8_t mode = mic_->getDetectionMode();
            Serial.println(F("=== Detection Mode Status ==="));
            Serial.print(F("Current Mode: "));
            Serial.print(mode);
            Serial.print(F(" - "));
            switch(mode) {
                case 0: Serial.println(F("DRUMMER")); break;
                case 1: Serial.println(F("BASS_BAND")); break;
                case 2: Serial.println(F("HFC")); break;
                case 3: Serial.println(F("SPECTRAL_FLUX")); break;
                case 4: Serial.println(F("HYBRID")); break;
                default: Serial.println(F("UNKNOWN")); break;
            }
            if (mode == 1) {
                Serial.print(F("Bass Level: "));
                Serial.println(mic_->getBassLevel(), 3);
            } else if (mode == 3 || mode == 4) {
                Serial.print(F("Flux Value: "));
                Serial.println(mic_->getLastFluxValue(), 3);
            }
            Serial.print(F("Transient Threshold: "));
            Serial.println(mic_->transientThreshold, 2);
            Serial.print(F("Recent Average: "));
            Serial.println(mic_->getRecentAverage(), 3);
        } else {
            Serial.println(F("Microphone not available"));
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

    // Restore mic defaults (window/range normalization and simplified transient detection)
    // All values tuned via param-tuner 2024-12
    if (mic_) {
        mic_->peakTau = Defaults::PeakTau;              // 2s peak adaptation
        mic_->releaseTau = Defaults::ReleaseTau;        // 5s peak release
        mic_->hwTarget = 0.35f;                         // Target raw input level (±0.01 dead zone)
        mic_->transientThreshold = 2.0f;                // 2x louder than recent average
        mic_->attackMultiplier = 1.2f;                  // 20% sudden rise required
        mic_->averageTau = 0.8f;                        // Recent average tracking time
        mic_->cooldownMs = 80;                          // 80ms cooldown (tuned 2025-12-30)
        mic_->fluxThresh = 2.8f;                        // Spectral flux threshold
        mic_->detectionMode = 4;                        // Hybrid mode (best F1: 0.705)
        mic_->hybridFluxWeight = 0.5f;                  // Hybrid flux weight (tuned 2025-12-30)
        mic_->hybridDrumWeight = 0.5f;                  // Hybrid drum weight (tuned 2025-12-30)
        mic_->hybridBothBoost = 1.2f;                   // Hybrid both-agree boost

        // Adaptive threshold defaults (disabled by default for backwards compat)
        mic_->adaptiveThresholdEnabled = false;
        mic_->adaptiveMinRaw = 0.1f;
        mic_->adaptiveMaxScale = 0.6f;
        mic_->adaptiveBlendTau = 5.0f;

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
        Serial.print(F(",\"mode\":"));
        Serial.print(mic_->getDetectionMode());
        Serial.print(F(",\"hwGain\":"));
        Serial.print(mic_->getHwGain());
        Serial.print(F(",\"level\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"avgLevel\":"));
        Serial.print(mic_->getRecentAverage(), 2);
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
        // t     → transient        : 0-1 (simplified amplitude spike strength, LOUD + SUDDEN detection)
        // pk    → peak             : 0-1 (current tracked peak for window normalization, raw range)
        // vl    → valley           : 0-1 (current tracked valley for window normalization, raw range)
        // raw   → raw ADC level    : 0-1 (what HW gain targets, pre-normalization)
        // h     → hardware gain    : 0-80 (PDM gain setting)
        // alive → PDM alive status : 0 or 1 (microphone health: 0=dead, 1=working)
        // z     → zero-crossing    : 0-1 (zero-crossing rate, for frequency classification)
        //
        // Debug mode additional fields:
        // avg   → recent average   : float (rolling average for transient threshold)
        // prev  → previous level   : float (previous frame level for attack detection)
        Serial.print(F("{\"a\":{\"l\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"t\":"));
        Serial.print(mic_->getTransient(), 2);
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
        Serial.print(F(",\"z\":"));
        Serial.print(mic_->zeroCrossingRate, 2);

        // Debug mode: add transient detection internal state
        if (streamDebug_) {
            Serial.print(F(",\"avg\":"));
            Serial.print(mic_->getRecentAverage(), 4);
            Serial.print(F(",\"prev\":"));
            Serial.print(mic_->getPreviousLevel(), 4);
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
