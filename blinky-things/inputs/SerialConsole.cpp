#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../music/MusicMode.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../types/Version.h"

extern const DeviceConfig& config;

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

SerialConsole::SerialConsole(Fire* fireGen, AdaptiveMic* mic)
    : fireGenerator_(fireGen), mic_(mic), battery_(nullptr), music_(nullptr), configStorage_(nullptr) {
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

    // === FIRE SETTINGS ===
    if (fp) {
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

    // === AUDIO SETTINGS ===
    // Window/Range normalization settings
    // Peak/valley tracking adapts to signal (valley = adaptive noise floor)
    if (mic_) {
        settings_.registerFloat("peaktau", &mic_->peakTau, "audio",
            "Peak adaptation speed (s)", 0.5f, 10.0f);
        settings_.registerFloat("releasetau", &mic_->releaseTau, "audio",
            "Peak release speed (s)", 1.0f, 30.0f);
    }

    // === HARDWARE AGC SETTINGS (Primary gain control) ===
    // Signal flow: Mic → HW Gain (PRIMARY) → ADC → Window/Range (SECONDARY) → Output
    // HW gain optimizes raw ADC input for best SNR (adapts to keep raw in target range)
    // Window/range tracks peak/valley and maps to 0-1 output (no clipping)
    if (mic_) {
        // Hardware AGC parameters (primary - optimizes ADC signal quality)
        settings_.registerFloat("hwtarget", &mic_->hwTarget, "agc",
            "HW target level (raw, ±0.01 dead zone)", 0.05f, 0.9f);
    }

    // === SIMPLIFIED TRANSIENT DETECTION SETTINGS ===
    if (mic_) {
        settings_.registerFloat("hitthresh", &mic_->transientThreshold, "transient",
            "Hit threshold (multiples of recent average)", 1.5f, 10.0f);
        settings_.registerFloat("attackmult", &mic_->attackMultiplier, "transient",
            "Attack multiplier (sudden rise ratio)", 1.1f, 2.0f);
        settings_.registerFloat("avgtau", &mic_->averageTau, "transient",
            "Recent average tracking time (s)", 0.1f, 5.0f);
        settings_.registerUint16("cooldown", &mic_->cooldownMs, "transient",
            "Cooldown between hits (ms)", 20, 500);
    }

    // === DETECTION MODE SETTINGS ===
    // Switch between different onset detection algorithms
    if (mic_) {
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

    // === MUSIC MODE SETTINGS ===
    if (music_) {
        settings_.registerFloat("musicthresh", &music_->activationThreshold, "music",
            "Music mode activation threshold (0-1)", 0.0f, 1.0f);
        settings_.registerUint8("musicbeats", &music_->minBeatsToActivate, "music",
            "Stable beats to activate", 2, 16);
        settings_.registerUint8("musicmissed", &music_->maxMissedBeats, "music",
            "Missed beats before deactivation", 4, 16);
        settings_.registerFloat("bpmmin", &music_->bpmMin, "music",
            "Minimum BPM", 40.0f, 120.0f);
        settings_.registerFloat("bpmmax", &music_->bpmMax, "music",
            "Maximum BPM", 120.0f, 240.0f);
        settings_.registerFloat("pllkp", &music_->pllKp, "music",
            "PLL proportional gain (responsiveness)", 0.01f, 0.5f);
        settings_.registerFloat("pllki", &music_->pllKi, "music",
            "PLL integral gain (stability)", 0.001f, 0.1f);
    }

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
    // === JSON API COMMANDS (for web app) ===
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
            // Get battery status
            float voltage = battery_->getVoltage();
            uint8_t percent = battery_->getPercent();
            bool charging = battery_->isCharging();
            bool connected = battery_->isBatteryConnected();

            // Send as JSON
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

    // === TEST MODE COMMANDS ===
    if (strncmp(cmd, "test lock hwgain", 16) == 0) {
        // Ensure command is exact match or followed by space (not "test lock hwgainXYZ")
        if (cmd[16] != '\0' && cmd[16] != ' ') {
            return false;  // Not a valid command, fall through
        }
        if (!mic_) {
            Serial.println(F("ERROR: Microphone not available"));
            return true;
        }
        // Parse optional gain value (default to current gain)
        int gain = mic_->getHwGain();
        if (strlen(cmd) > 17) {
            gain = atoi(cmd + 17);
            // Validate gain range (0-80) and warn if out of bounds
            if (gain < 0 || gain > 80) {
                Serial.print(F("WARNING: Gain "));
                Serial.print(gain);
                Serial.println(F(" out of range (0-80), will be clamped"));
            }
        }
        // Lock hardware gain at specified value (disables AGC)
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
        // Unlock hardware gain (re-enables AGC)
        mic_->unlockHwGain();
        Serial.println(F("OK unlocked"));
        return true;
    }

    // Note: "test reset baselines" command removed with simplified transient detection

    // === MUSIC MODE STATUS ===
    if (strcmp(cmd, "music") == 0) {
        if (music_) {
            Serial.println(F("=== Music Mode Status ==="));
            Serial.print(F("Active: "));
            Serial.println(music_->active ? F("YES") : F("NO"));
            Serial.print(F("BPM: "));
            Serial.println(music_->bpm);
            Serial.print(F("Phase: "));
            Serial.println(music_->phase);
            Serial.print(F("Beat #: "));
            Serial.println(music_->beatNumber);
            Serial.print(F("Confidence: "));
            Serial.println(music_->getConfidence());
        } else {
            Serial.println(F("Music mode not available"));
        }
        return true;
    }

    // === CONFIGURATION COMMANDS ===
    if (strcmp(cmd, "save") == 0) {
        if (configStorage_ && fireGenerator_ && mic_) {
            configStorage_->saveConfiguration(fireGenerator_->getParams(), *mic_);
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "load") == 0) {
        if (configStorage_ && fireGenerator_ && mic_) {
            configStorage_->loadConfiguration(fireGenerator_->getParamsMutable(), *mic_);
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
    if (mic_) {
        mic_->peakTau = Defaults::PeakTau;              // 2s peak adaptation
        mic_->releaseTau = Defaults::ReleaseTau;        // 5s peak release
        mic_->hwTarget = 0.35f;                         // Target raw input level (±0.01 dead zone)
        mic_->transientThreshold = 3.0f;                // 3x louder than recent average
        mic_->attackMultiplier = 1.3f;                  // 30% sudden rise required
        mic_->averageTau = 0.8f;                        // Recent average tracking time
        mic_->cooldownMs = 80;                          // 80ms cooldown between hits
    }
}

void SerialConsole::streamTick() {
    if (!streamEnabled_) return;

    uint32_t now = millis();

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

        // Music mode telemetry (always include when music_ is available)
        // Format: "m":{"a":1,"bpm":125.3,"ph":0.45,"conf":0.82,"q":1,"h":0,"w":0}
        // a = active, bpm = tempo, ph = phase, conf = confidence
        // q/h/w = quarter/half/whole note events (1 = event this frame)
        if (music_) {
            Serial.print(F(",\"m\":{\"a\":"));
            Serial.print(music_->isActive() ? 1 : 0);
            Serial.print(F(",\"bpm\":"));
            Serial.print(music_->getBPM(), 1);
            Serial.print(F(",\"ph\":"));
            Serial.print(music_->getPhase(), 2);
            Serial.print(F(",\"conf\":"));
            Serial.print(music_->getConfidence(), 2);
            Serial.print(F(",\"q\":"));
            Serial.print(music_->quarterNote ? 1 : 0);
            Serial.print(F(",\"h\":"));
            Serial.print(music_->halfNote ? 1 : 0);
            Serial.print(F(",\"w\":"));
            Serial.print(music_->wholeNote ? 1 : 0);
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
