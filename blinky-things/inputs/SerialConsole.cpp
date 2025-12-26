#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../types/Version.h"

extern const DeviceConfig& config;

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

SerialConsole::SerialConsole(Fire* fireGen, AdaptiveMic* mic)
    : fireGenerator_(fireGen), mic_(mic), battery_(nullptr), configStorage_(nullptr) {
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
        settings_.registerUint8("audioheatboost", &fp->audioHeatBoostMax, "fire",
            "Max audio heat boost", 0, 255);
        settings_.registerInt8("coolingaudiobias", &fp->coolingAudioBias, "fire",
            "Audio cooling bias", -128, 127);
        settings_.registerUint8("bottomrows", &fp->bottomRowsForSparks, "fire",
            "Spark injection rows", 1, 8);
        settings_.registerUint8("transientheatmax", &fp->transientHeatMax, "fire",
            "Transient heat boost", 0, 255);
        settings_.registerUint8("burstsparks", &fp->burstSparks, "fire",
            "Sparks per burst", 1, 20);
        settings_.registerUint16("suppressionms", &fp->suppressionMs, "fire",
            "Burst suppression time", 50, 1000);
        settings_.registerFloat("heatdecay", &fp->heatDecay, "fire",
            "Heat decay rate", 0.5f, 0.99f);
        settings_.registerUint8("emberheatmax", &fp->emberHeatMax, "fire",
            "Max ember heat", 0, 50);
    }

    // === AUDIO SETTINGS ===
    if (mic_) {
        settings_.registerFloat("gate", &mic_->noiseGate, "audio",
            "Noise gate threshold", 0.0f, 1.0f);

        // Window/Range normalization settings
        // Peak tracks actual signal (no target - follows loudness naturally)
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
        settings_.registerFloat("hwtargetlow", &mic_->hwTargetLow, "agc",
            "HW target low (raw)", 0.05f, 0.5f);
        settings_.registerFloat("hwtargethigh", &mic_->hwTargetHigh, "agc",
            "HW target high (raw)", 0.1f, 0.9f);
    }

    // === FREQUENCY-SPECIFIC DETECTION (always enabled) ===
    if (mic_) {
        settings_.registerFloat("kickthresh", &mic_->kickThreshold, "freq",
            "Kick detection threshold", 1.0f, 5.0f);
        settings_.registerFloat("snarethresh", &mic_->snareThreshold, "freq",
            "Snare detection threshold", 1.0f, 5.0f);
        settings_.registerFloat("hihatthresh", &mic_->hihatThreshold, "freq",
            "Hi-hat detection threshold", 1.0f, 5.0f);
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

    // Restore mic defaults (window/range normalization)
    if (mic_) {
        mic_->noiseGate = Defaults::NoiseGate;
        mic_->peakTau = Defaults::PeakTau;        // 2s peak adaptation
        mic_->releaseTau = Defaults::ReleaseTau;  // 5s peak release
        // Note: Timing constants (transient cooldown, hw calib, hw tracking) are now compile-time constants
    }
}

void SerialConsole::streamTick() {
    if (!streamEnabled_) return;

    uint32_t now = millis();

    // Audio streaming at ~20Hz
    if (mic_ && (now - streamLastMs_ >= STREAM_PERIOD_MS)) {
        streamLastMs_ = now;

        // Output compact JSON for web app (abbreviated field names for serial bandwidth)
        // Format: {"a":{"l":0.45,"t":0.85,"pk":0.32,"vl":0.04,"raw":0.12,"h":32,"alive":1,"k":0,"sn":1,"hh":0,"ks":0.0,"ss":0.82,"hs":0.0,"z":0.15}}
        //
        // Field Mapping (abbreviated → full name : range):
        // l     → level            : 0-1 (post-range-mapping output, noise-gated)
        // t     → transient        : 0-1 (max percussion strength: kick/snare/hihat, normalized)
        // pk    → peak             : 0-1 (current tracked peak for window normalization, raw range)
        // vl    → valley           : 0-1 (current tracked valley for window normalization, raw range)
        // raw   → raw ADC level    : 0-1 (what HW gain targets, pre-normalization)
        // h     → hardware gain    : 0-80 (PDM gain setting)
        // alive → PDM alive status : 0 or 1 (microphone health: 0=dead, 1=working)
        // k     → kick impulse     : 0 or 1 (boolean flag: kick detected this frame)
        // sn    → snare impulse    : 0 or 1 (boolean flag: snare detected this frame)
        // hh    → hihat impulse    : 0 or 1 (boolean flag: hihat detected this frame)
        // ks    → kick strength    : 0-1 (normalized: 0 at threshold, 1.0 at 3x threshold)
        // ss    → snare strength   : 0-1 (normalized: 0 at threshold, 1.0 at 3x threshold)
        // hs    → hihat strength   : 0-1 (normalized: 0 at threshold, 1.0 at 3x threshold)
        // z     → zero-crossing    : 0-1 (zero-crossing rate, for frequency classification)
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
        Serial.print(F(",\"k\":"));
        Serial.print(mic_->getKickImpulse() ? 1 : 0);
        Serial.print(F(",\"sn\":"));
        Serial.print(mic_->getSnareImpulse() ? 1 : 0);
        Serial.print(F(",\"hh\":"));
        Serial.print(mic_->getHihatImpulse() ? 1 : 0);
        Serial.print(F(",\"ks\":"));
        Serial.print(mic_->getKickStrength(), 2);
        Serial.print(F(",\"ss\":"));
        Serial.print(mic_->getSnareStrength(), 2);
        Serial.print(F(",\"hs\":"));
        Serial.print(mic_->getHihatStrength(), 2);
        Serial.print(F(",\"z\":"));
        Serial.print(mic_->zeroCrossingRate, 2);
        Serial.println(F("}}"));
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
