#include "SerialConsole.h"
#include "TotemDefaults.h"
#include "AdaptiveMic.h"

extern AdaptiveMic mic;

SerialConsole::SerialConsole(FireEffect &f, Adafruit_NeoPixel &l)
    : fire(f), leds(l) {}

void SerialConsole::begin() {
    Serial.begin(115200);

    // Don’t block forever on nRF52840
    unsigned long start = millis();
    while (!Serial && (millis() - start < 2000)) { delay(10); }

    Serial.println(F("Serial console ready. Type 'help' for commands."));
}

void SerialConsole::update() {
    if (Serial.available()) {
        static char buf[96];
        size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
        buf[len] = '\0';

        // Trim CR/LF
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
            buf[--len] = '\0';
        }

        // Debug echo
        Serial.print(F("Received: '"));
        Serial.print(buf);
        Serial.println(F("'"));

        handleCommand(buf);
    }
}

void SerialConsole::handleCommand(const char *cmd) {
    int tempInt;
    float tempFloat;

    if (strcmp(cmd, "help") == 0) {
        Serial.println(F("Commands: show, defaults"));
        Serial.println(F(" Fire: set cooling <0-255>, set sparkchance <0-1>, "
                         "set sparkheatmin <0-255>, set sparkheatmax <0-255>, "
                         "set audiosparkboost <0-1>, set audioheatboost <0-255>, "
                         "set coolingaudiobias <-128..127>, set bottomrows <1-8>"));
        Serial.println(F(" Mic : set gate <0-1>, set gamma <0.1-3>, set gain <0-5>, "
                         "set attack <s>, set release <s>"));
        Serial.println(F(" vu on/off"));
    }
    else if (strcmp(cmd, "show") == 0) {
        printAll();
    }
    else if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
    }
    // --- Fire parameters ---
    else if (sscanf(cmd, "set cooling %d", &tempInt) == 1) { fire.params.baseCooling = (uint8_t)tempInt; }
    else if (sscanf(cmd, "set sparkchance %f", &tempFloat) == 1) { fire.params.sparkChance = tempFloat; }
    else if (sscanf(cmd, "set sparkheatmin %d", &tempInt) == 1) { fire.params.sparkHeatMin = (uint8_t)tempInt; }
    else if (sscanf(cmd, "set sparkheatmax %d", &tempInt) == 1) { fire.params.sparkHeatMax = (uint8_t)tempInt; }
    else if (sscanf(cmd, "set audiosparkboost %f", &tempFloat) == 1) { fire.params.audioSparkBoost = tempFloat; }
    else if (sscanf(cmd, "set audioheatboost %d", &tempInt) == 1) { fire.params.audioHeatBoostMax = (uint8_t)tempInt; }
    else if (sscanf(cmd, "set coolingaudiobias %d", &tempInt) == 1) { fire.params.coolingAudioBias = (int8_t)tempInt; }
    else if (sscanf(cmd, "set bottomrows %d", &tempInt) == 1) { fire.params.bottomRowsForSparks = (uint8_t)tempInt; }
    // --- AdaptiveMic parameters ---
    else if (sscanf(cmd, "set gate %f", &tempFloat) == 1) { mic.noiseGate = tempFloat; }
    else if (sscanf(cmd, "set gamma %f", &tempFloat) == 1) { mic.gamma = tempFloat; }
    else if (sscanf(cmd, "set gain %f", &tempFloat) == 1) { mic.globalGain = tempFloat; }
    else if (sscanf(cmd, "set attack %f", &tempFloat) == 1) { mic.attackTau = tempFloat; }
    else if (sscanf(cmd, "set release %f", &tempFloat) == 1) { mic.releaseTau = tempFloat; }
    // --- VU meter ---
    else if (strcmp(cmd, "vu on") == 0) {
        fire.params.vuTopRowEnabled = true;
    }
    else if (strcmp(cmd, "vu off") == 0) {
        fire.params.vuTopRowEnabled = false;
    }
    else {
        Serial.println(F("Unknown command"));
    }
}

void SerialConsole::restoreDefaults() {
    fire.restoreDefaults();
    mic.noiseGate   = Defaults::NoiseGate;
    mic.gamma       = Defaults::Gamma;
    mic.globalGain  = Defaults::GlobalGain;
    mic.attackTau   = Defaults::AttackTau;
    mic.releaseTau  = Defaults::ReleaseTau;
    Serial.println(F("Defaults restored."));
}

void SerialConsole::printAll() {
    // Fire
    Serial.print(F("Cooling: ")); Serial.println(fire.params.baseCooling);
    Serial.print(F("SparkChance: ")); Serial.println(fire.params.sparkChance);
    Serial.print(F("SparkHeatMin: ")); Serial.println(fire.params.sparkHeatMin);
    Serial.print(F("SparkHeatMax: ")); Serial.println(fire.params.sparkHeatMax);
    Serial.print(F("AudioSparkBoost: ")); Serial.println(fire.params.audioSparkBoost);
    Serial.print(F("AudioHeatBoostMax: ")); Serial.println(fire.params.audioHeatBoostMax);
    Serial.print(F("CoolingAudioBias: ")); Serial.println(fire.params.coolingAudioBias);
    Serial.print(F("BottomRowsForSparks: ")); Serial.println(fire.params.bottomRowsForSparks);
    Serial.print(F("VU meter: "));
    Serial.println(fire.params.vuTopRowEnabled ? F("on") : F("off"));

    // Mic
    Serial.print(F("NoiseGate: ")); Serial.println(mic.noiseGate);
    Serial.print(F("Gamma: ")); Serial.println(mic.gamma);
    Serial.print(F("GlobalGain: ")); Serial.println(mic.globalGain);
    Serial.print(F("AttackTau: ")); Serial.println(mic.attackTau);
    Serial.print(F("ReleaseTau: ")); Serial.println(mic.releaseTau);
}

void SerialConsole::drawTopRowVU() {
    if (!fire.params.vuTopRowEnabled) return;

    float level = mic.getLevel();
    int lit = (int)(level * 16.0f + 0.5f);
    if (lit > 16) lit = 16;

    int y = 0; // top row
    for (int x = 0; x < 16; ++x) {
        uint32_t color = (x < lit) ? leds.Color(255, 0, 0) : 0;
        leds.setPixelColor(fire.xyToIndex(x, y), color);
    }
}
