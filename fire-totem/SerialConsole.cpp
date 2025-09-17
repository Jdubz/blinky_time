#include "SerialConsole.h"
#include "TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"

extern AdaptiveMic mic;
extern BatteryMonitor battery;

SerialConsole::SerialConsole(FireEffect &f, Adafruit_NeoPixel &l)
    : fire(f), leds(l) {}

void SerialConsole::begin() {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 2000)) { delay(10); }
    Serial.println(F("Serial console ready. Type 'help' for commands."));
}

void SerialConsole::update() {
    // Handle incoming commands
    if (Serial.available()) {
        static char buf[128];
        size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
        buf[len] = '\0';
        // Trim CR/LF
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) {
            buf[--len] = '\0';
        }
        // Debug echo
        // Serial.print(F("Received: '")); Serial.print(buf); Serial.println(F("'"));
        handleCommand(buf);
    }

    // Periodic mic debug output
    micDebugTick();
}

void SerialConsole::handleCommand(const char *cmd) {
    int tempInt = 0;
    float tempFloat = 0.0f;
    float tempFloat2 = 0.0f;

    if (strcmp(cmd, "help") == 0) {
        Serial.println(F("Commands: show, defaults"));
        Serial.println(F(" Fire: set cooling <0-255>, set sparkchance <0-1>, "
                         "set sparkheatmin <0-255>, set sparkheatmax <0-255>, "
                         "set audiosparkboost <0-1>, set audioheatboost <0-255>, "
                         "set coolingaudiobias <-128..127>, set bottomrows <1-8>"));
        Serial.println(F(" Mic : set gate <0-1>, set gamma <0.1-3>, set gain <0-5>, "
                         "set attack <s>, set release <s>"));
        Serial.println(F(" VU  : vu on/off"));
        Serial.println(F(" Mic Debug:"));
        Serial.println(F("   mic debug on/off"));
        Serial.println(F("   mic debug rate <ms>"));
        Serial.println(F("   mic debug csv on/off  (CSV: #ms,rawAbs,level,envAR,floor,peak,gain)"));
        Serial.println(F("   mic stats   (one snapshot)"));
    }
    else if (strcmp(cmd, "show") == 0) {
        printAll();
    }
    else if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
    }
    // --- Fire parameters ---
    else if (sscanf(cmd, "set cooling %d", &tempInt) == 1) { fire.params.baseCooling = (uint8_t)tempInt; Serial.print(F("Cooling=")); Serial.println(fire.params.baseCooling); }
    else if (sscanf(cmd, "set sparkchance %f", &tempFloat) == 1) { fire.params.sparkChance = tempFloat; Serial.print(F("SparkChance=")); Serial.println(fire.params.sparkChance, 3); }
    else if (sscanf(cmd, "set sparkheatmin %d", &tempInt) == 1) { fire.params.sparkHeatMin = (uint8_t)tempInt; Serial.print(F("SparkHeatMin=")); Serial.println(fire.params.sparkHeatMin); }
    else if (sscanf(cmd, "set sparkheatmax %d", &tempInt) == 1) { fire.params.sparkHeatMax = (uint8_t)tempInt; Serial.print(F("SparkHeatMax=")); Serial.println(fire.params.sparkHeatMax); }
    else if (sscanf(cmd, "set audiosparkboost %f", &tempFloat) == 1) { fire.params.audioSparkBoost = tempFloat; Serial.print(F("AudioSparkBoost=")); Serial.println(fire.params.audioSparkBoost, 3); }
    else if (sscanf(cmd, "set audioheatboost %d", &tempInt) == 1) { fire.params.audioHeatBoostMax = (uint8_t)tempInt; Serial.print(F("AudioHeatBoostMax=")); Serial.println(fire.params.audioHeatBoostMax); }
    else if (sscanf(cmd, "set coolingaudiobias %d", &tempInt) == 1) { fire.params.coolingAudioBias = (int8_t)tempInt; Serial.print(F("CoolingAudioBias=")); Serial.println(fire.params.coolingAudioBias); }
    else if (sscanf(cmd, "set bottomrows %d", &tempInt) == 1) { fire.params.bottomRowsForSparks = (uint8_t)tempInt; Serial.print(F("BottomRowsForSparks=")); Serial.println(fire.params.bottomRowsForSparks); }
    // --- AdaptiveMic parameters ---
    else if (sscanf(cmd, "set gate %f", &tempFloat) == 1) { mic.noiseGate = tempFloat; Serial.print(F("NoiseGate=")); Serial.println(mic.noiseGate, 3); }
    else if (sscanf(cmd, "set gamma %f", &tempFloat) == 1) { mic.gamma = tempFloat; Serial.print(F("Gamma=")); Serial.println(mic.gamma, 3); }
    else if (sscanf(cmd, "set gain %f", &tempFloat) == 1) { mic.globalGain = tempFloat; Serial.print(F("GlobalGain=")); Serial.println(mic.globalGain, 3); }
    else if (sscanf(cmd, "set attack %f", &tempFloat) == 1) { mic.attackTau = tempFloat; Serial.print(F("AttackTau=")); Serial.println(mic.attackTau, 3); }
    else if (sscanf(cmd, "set release %f", &tempFloat) == 1) { mic.releaseTau = tempFloat; Serial.print(F("ReleaseTau=")); Serial.println(mic.releaseTau, 3); }
    // --- Mic debug tool ---
    else if (strcmp(cmd, "mic debug on") == 0) {
        micDebugEnabled = true; Serial.println(F("MicDebug=on"));
    }
    else if (strcmp(cmd, "mic debug off") == 0) {
        micDebugEnabled = false; Serial.println(F("MicDebug=off"));
    }
    else if (sscanf(cmd, "mic debug rate %d", &tempInt) == 1) {
        if (tempInt < 20) tempInt = 20;
        micDebugPeriodMs = (unsigned long)tempInt;
        Serial.print(F("MicDebugRate(ms)=")); Serial.println(micDebugPeriodMs);
    }
    else if (strcmp(cmd, "mic debug csv on") == 0) {
        micDebugCsv = true; Serial.println(F("MicDebugCSV=on"));
        Serial.println(F("#ms,level,envAR,floor,peak,avgAbs,gain"));
    }
    else if (strcmp(cmd, "mic debug csv off") == 0) {
        micDebugCsv = false; Serial.println(F("MicDebugCSV=off"));
    }
    else if (strcmp(cmd, "mic stats") == 0) {
        micDebugPrintLine();
    }

    // --- Auto-gain controls ---
    else if (strcmp(cmd, "ag on") == 0) {
        mic.autoGainEnabled = true; Serial.println(F("AutoGain=on"));
    }
    else if (strcmp(cmd, "ag off") == 0) {
        mic.autoGainEnabled = false; Serial.println(F("AutoGain=off"));
    }
    else if (sscanf(cmd, "ag target %f", &tempFloat) == 1) {
        mic.agTarget = constrain(tempFloat, 0.1f, 0.95f);
        Serial.print(F("AutoGainTarget=")); Serial.println(mic.agTarget, 3);
    }
    else if (sscanf(cmd, "ag strength %f", &tempFloat) == 1) {
        mic.agStrength = max(0.0f, tempFloat);
        Serial.print(F("AutoGainStrength=")); Serial.println(mic.agStrength, 3);
    }
    else if (sscanf(cmd, "ag limits %f %f", &tempFloat, &tempFloat2) == 2) {
        mic.agMin = tempFloat; mic.agMax = tempFloat2;
        if (mic.agMin > mic.agMax) { float t=mic.agMin; mic.agMin=mic.agMax; mic.agMax=t; }
        Serial.print(F("AutoGainLimits=["));
        Serial.print(mic.agMin,2); Serial.print(','); Serial.print(mic.agMax,2); Serial.println(']');
    }
    else if (strcmp(cmd, "ag stats") == 0) {
        Serial.print(F("AutoGain: enabled=")); Serial.print(mic.autoGainEnabled ? F("yes") : F("no"));
        Serial.print(F(" target="));   Serial.print(mic.agTarget,3);
        Serial.print(F(" strength=")); Serial.print(mic.agStrength,3);
        Serial.print(F(" limits=["));  Serial.print(mic.agMin,2); Serial.print(','); Serial.print(mic.agMax,2); Serial.println(']');
        Serial.print(F(" globalGain=")); Serial.println(mic.globalGain,3);
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
    Serial.print(F("SparkChance: ")); Serial.println(fire.params.sparkChance, 3);
    Serial.print(F("SparkHeatMin: ")); Serial.println(fire.params.sparkHeatMin);
    Serial.print(F("SparkHeatMax: ")); Serial.println(fire.params.sparkHeatMax);
    Serial.print(F("AudioSparkBoost: ")); Serial.println(fire.params.audioSparkBoost, 3);
    Serial.print(F("AudioHeatBoostMax: ")); Serial.println(fire.params.audioHeatBoostMax);
    Serial.print(F("CoolingAudioBias: ")); Serial.println(fire.params.coolingAudioBias);
    Serial.print(F("BottomRowsForSparks: ")); Serial.println(fire.params.bottomRowsForSparks);
    // Mic
    Serial.print(F("NoiseGate: ")); Serial.println(mic.noiseGate, 3);
    Serial.print(F("Gamma: ")); Serial.println(mic.gamma, 3);
    Serial.print(F("GlobalGain: ")); Serial.println(mic.globalGain, 3);
    Serial.print(F("AttackTau: ")); Serial.println(mic.attackTau, 3);
    Serial.print(F("ReleaseTau: ")); Serial.println(mic.releaseTau, 3);
     // Battery (one-shot read; no background polling)
    float vbat = battery.readVoltage();                       // volts
    uint8_t soc = BatteryMonitor::voltageToPercent(vbat);     // % estimate
    bool charging = battery.isCharging();                     // CHG pin if wired

    Serial.print(F("Battery: "));
    Serial.print(vbat, 3); Serial.print(F(" V  ("));
    Serial.print(soc); Serial.print(F("%)  Charging: "));
    Serial.println(charging ? F("yes") : F("no"));
}

// ---- Mic debug tool ----
void SerialConsole::micDebugTick() {
    if (!micDebugEnabled) return;
    unsigned long now = millis();
    if (now - micDebugLastMs >= micDebugPeriodMs) {
        micDebugLastMs = now;
        micDebugPrintLine();
    }
}

void SerialConsole::micDebugPrintLine() {
    unsigned long ms = millis();
    float raw   = mic.getAvgAbs();  // << raw, pre-compensation
    float level = mic.getLevel();   // normalized+gated+gain+gamma
    float env   = mic.getEnvAR();
    float floor = mic.getFloor();
    float peak  = mic.getPeak();
    int   gain  = mic.getGain();

    if (micDebugCsv) {
        // CSV header (printed when enabling CSV): #ms,rawAbs,level,envAR,floor,peak,gain
        Serial.print(ms);   Serial.print(',');
        Serial.print(raw, 1);   Serial.print(',');  // RAW
        Serial.print(level, 4); Serial.print(',');
        Serial.print(env,   1); Serial.print(',');
        Serial.print(floor, 1); Serial.print(',');
        Serial.print(peak,  1); Serial.print(',');
        Serial.println(gain);
    } else {
        Serial.print(F("[MIC] t=")); Serial.print(ms);
        Serial.print(F("ms  RAW="));  Serial.print(raw, 1);    // RAW shown first
        Serial.print(F("  lvl="));    Serial.print(level, 4);
        Serial.print(F("  env="));    Serial.print(env, 1);
        Serial.print(F("  floor="));  Serial.print(floor, 1);
        Serial.print(F("  peak="));   Serial.print(peak, 1);
        Serial.print(F("  gain="));   Serial.println(gain);
    }
}
