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
        fp = &fireGenerator_->getParams();
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
    // Note: globalGain is NOT registered here - it's auto-managed by AGC
    // and displayed via streaming JSON as {"a":{"g":...}}
    if (mic_) {
        settings_.registerFloat("gate", &mic_->noiseGate, "audio",
            "Noise gate threshold", 0.0f, 1.0f);
        settings_.registerFloat("attack", &mic_->attackSeconds, "audio",
            "Attack time (seconds)", 0.001f, 1.0f);
        settings_.registerFloat("release", &mic_->releaseSeconds, "audio",
            "Release time (seconds)", 0.01f, 2.0f);
        settings_.registerUint32("transientcooldown", &mic_->transientCooldownMs, "audio",
            "Transient cooldown (ms)", 10, 1000);
        settings_.registerFloat("transientfactor", &mic_->transientFactor, "audio",
            "Transient sensitivity", 1.0f, 5.0f);
    }

    // === AUTO-GAIN SETTINGS ===
    if (mic_) {
        settings_.registerBool("agenabled", &mic_->agEnabled, "agc",
            "Auto-gain enabled");
        settings_.registerFloat("agtarget", &mic_->agTarget, "agc",
            "Target level", 0.1f, 0.95f);
        settings_.registerFloat("agstrength", &mic_->agStrength, "agc",
            "Adjustment speed", 0.01f, 1.0f);
        settings_.registerFloat("agmin", &mic_->agMin, "agc",
            "Minimum gain", 0.1f, 5.0f);
        settings_.registerFloat("agmax", &mic_->agMax, "agc",
            "Maximum gain", 1.0f, 20.0f);
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

    if (strcmp(cmd, "battery test") == 0 || strcmp(cmd, "batt test") == 0) {
        if (battery_) {
            battery_->testDividerEnable();
        } else {
            Serial.println(F("Battery monitor not available"));
        }
        return true;
    }

    if (strcmp(cmd, "battery scan") == 0 || strcmp(cmd, "batt scan") == 0) {
        if (battery_) {
            battery_->scanForEnablePin();
        } else {
            Serial.println(F("Battery monitor not available"));
        }
        return true;
    }

    if (strcmp(cmd, "battery raw") == 0 || strcmp(cmd, "batt raw") == 0) {
        if (battery_) {
            // Get config
            const auto& cfg = battery_->getConfig();

            // Collect 5 samples
            uint16_t samples[5];
            for (int i = 0; i < 5; i++) {
                samples[i] = battery_->readRaw();
                if (i < 4) delay(10);
            }

            // Use last sample for calculations
            uint16_t raw = samples[4];
            const float maxCount = (cfg.adcBits == 12) ? 4095.0f :
                                   ((cfg.adcBits == 10) ? 1023.0f :
                                   (float)((1UL<<cfg.adcBits)-1));
            float v_adc = (raw * cfg.vrefVolts) / maxCount;
            float v_batt_calc = v_adc / cfg.dividerRatio;
            float v_batt_actual = battery_->getVoltage();

            // Send as JSON
            Serial.print(F("{\"batteryRaw\":{"));

            // Pins
            Serial.print(F("\"pins\":{\"vbat\":"));
            Serial.print(cfg.pinVBAT);
            Serial.print(F(",\"enable\":"));
            Serial.print(cfg.pinVBATEnable);
            Serial.print(F("},"));

            // Samples
            Serial.print(F("\"samples\":["));
            for (int i = 0; i < 5; i++) {
                if (i > 0) Serial.print(F(","));
                Serial.print(samples[i]);
            }
            Serial.print(F("],"));

            // ADC config
            Serial.print(F("\"adc\":{\"bits\":"));
            Serial.print(cfg.adcBits);
            Serial.print(F(",\"maxCount\":"));
            Serial.print((uint16_t)maxCount);
            Serial.print(F(",\"vref\":"));
            Serial.print(cfg.vrefVolts, 3);
            Serial.print(F("},"));

            // Readings
            Serial.print(F("\"reading\":{\"raw\":"));
            Serial.print(raw);
            Serial.print(F(",\"vAdc\":"));
            Serial.print(v_adc, 4);
            Serial.print(F(",\"dividerRatio\":"));
            Serial.print(cfg.dividerRatio, 4);
            Serial.print(F(",\"vBattCalc\":"));
            Serial.print(v_batt_calc, 4);
            Serial.print(F(",\"vBattActual\":"));
            Serial.print(v_batt_actual, 4);
            Serial.print(F("},"));

            // Platform info
            Serial.print(F("\"platform\":{\"p0_31\":"));
            #if defined(P0_31)
            Serial.print(F("true,\"p0_31_value\":"));
            Serial.print(P0_31);
            #else
            Serial.print(F("false"));
            #endif
            Serial.print(F(",\"analogReadRes\":"));
            #if defined(analogReadResolution)
            Serial.print(F("true"));
            #else
            Serial.print(F("false"));
            #endif
            Serial.print(F(",\"ar_internal2v4\":"));
            #if defined(AR_INTERNAL2V4)
            Serial.print(F("true"));
            #else
            Serial.print(F("false"));
            #endif
            Serial.print(F("}"));

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
            configStorage_->loadConfiguration(fireGenerator_->getParams(), *mic_);
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

    // Restore mic defaults
    if (mic_) {
        mic_->noiseGate = Defaults::NoiseGate;
        mic_->globalGain = Defaults::GlobalGain;
        mic_->attackSeconds = Defaults::AttackSeconds;
        mic_->releaseSeconds = Defaults::ReleaseSeconds;
        mic_->transientCooldownMs = Defaults::TransientCooldownMs;
        mic_->agEnabled = true;
        mic_->agTarget = Defaults::AutoGainTarget;
        mic_->agStrength = Defaults::AutoGainStrength;
        mic_->agMin = Defaults::AutoGainMin;
        mic_->agMax = Defaults::AutoGainMax;
    }
}

void SerialConsole::streamTick() {
    if (!streamEnabled_) return;

    uint32_t now = millis();

    // Audio streaming at ~20Hz
    if (mic_ && (now - streamLastMs_ >= STREAM_PERIOD_MS)) {
        streamLastMs_ = now;

        // Output compact JSON for web app
        // Format: {"a":{"l":0.45,"t":0.85,"e":0.32,"g":3.5}}
        Serial.print(F("{\"a\":{\"l\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"t\":"));
        Serial.print(mic_->getTransient(), 2);
        Serial.print(F(",\"e\":"));
        Serial.print(mic_->getEnv(), 2);
        Serial.print(F(",\"g\":"));
        Serial.print(mic_->getGlobalGain(), 1);
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
