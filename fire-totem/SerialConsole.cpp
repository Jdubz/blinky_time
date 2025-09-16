#include "SerialConsole.h"
#include "TotemDefaults.h"
#include "AdaptiveMic.h"

extern AdaptiveMic mic;
extern Adafruit_NeoPixel leds;

SerialConsole::SerialConsole(FireEffect &f) : fire(f) {}

void SerialConsole::begin() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    Serial.println(F("Serial console ready. Type 'help' for commands."));
}

void SerialConsole::update() {
    if (Serial.available()) {
        static char buf[64];
        size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
        buf[len] = '\0';
        handleCommand(buf);
    }
}

void SerialConsole::handleCommand(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        Serial.println(F("Commands: show, defaults"));
        Serial.println(F(" Fire params: set cooling X, set sparkchance X, "
                         "set sparkheatmin X, set sparkheatmax X, "
                         "set audiosparkboost X, set audioheatboost X, "
                         "set coolingaudiobias X, set bottomrows X"));
        Serial.println(F(" Mic params: set gate X, set gamma X, set gain X, "
                         "set attack X, set release X"));
        Serial.println(F(" vu on/off"));
    }
    else if (strcmp(cmd, "show") == 0) {
        printAll();
    }
    else if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
    }
    // --- Fire parameters ---
    else if (sscanf(cmd, "set cooling %hhu", &fire.params.baseCooling) == 1) {}
    else if (sscanf(cmd, "set sparkchance %f", &fire.params.sparkChance) == 1) {}
    else if (sscanf(cmd, "set sparkheatmin %hhu", &fire.params.sparkHeatMin) == 1) {}
    else if (sscanf(cmd, "set sparkheatmax %hhu", &fire.params.sparkHeatMax) == 1) {}
    else if (sscanf(cmd, "set audiosparkboost %f", &fire.params.audioSparkBoost) == 1) {}
    else if (sscanf(cmd, "set audioheatboost %hhu", &fire.params.audioHeatBoostMax) == 1) {}
    else if (sscanf(cmd, "set coolingaudiobias %hhd", &fire.params.coolingAudioBias) == 1) {}
    else if (sscanf(cmd, "set bottomrows %hhu", &fire.params.bottomRowsForSparks) == 1) {}
    // --- AdaptiveMic parameters ---
    else if (sscanf(cmd, "set gate %f", &mic.noiseGate) == 1) {}
    else if (sscanf(cmd, "set gamma %f", &mic.gamma) == 1) {}
    else if (sscanf(cmd, "set gain %f", &mic.globalGain) == 1) {}
    else if (sscanf(cmd, "set attack %f", &mic.attackTau) == 1) {}
    else if (sscanf(cmd, "set release %f", &mic.releaseTau) == 1) {}
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

    float level = mic.getLevel(); // normalized 0–1
    int lit = (int)(level * 16.0f + 0.5f);
    if (lit > 16) lit = 16;

    for (int x = 0; x < 16; x++) {
        uint32_t color = (x < lit) ? leds.Color(255, 0, 0) : 0;
        leds.setPixelColor(x, 0, color);
    }
}
