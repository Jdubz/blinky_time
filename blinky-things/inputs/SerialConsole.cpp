#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../tests/GeneratorTestRunner.h"
#include "../types/Version.h"

extern AdaptiveMic mic;
extern BatteryMonitor battery;
extern IMUHelper imu;
extern const DeviceConfig& config;

SerialConsole::SerialConsole(UnifiedFireGenerator* fireGen, Adafruit_NeoPixel &l)
    : fireGenerator_(fireGen), leds(l), configStorage_(nullptr), testRunner_(nullptr) {}

void SerialConsole::begin() {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 2000)) { delay(10); }
    Serial.println(F("Serial console ready. Type 'help' for commands."));

    // Initialize test runner lazily when first needed
    if (!testRunner_) {
        testRunner_ = new GeneratorTestRunner();
    }
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

    // Periodic debug outputs
    micDebugTick();
    debugTick();
    imuDebugTick();
}

void SerialConsole::handleCommand(const char *cmd) {
    int tempInt = 0;
    float tempFloat = 0.0f;
    float tempFloat2 = 0.0f;

    if (strcmp(cmd, "help") == 0) {
        Serial.println(F("=== FIRE TOTEM DEBUG CONSOLE ==="));
        Serial.println(F("General: show, version, defaults"));
        Serial.println(F(""));
        Serial.println(F("FIRE ENGINE:"));
        Serial.println(F("  set cooling <0-255>        - Base cooling rate"));
        Serial.println(F("  set sparkchance <0-1>      - Probability of sparks"));
        Serial.println(F("  set sparkheatmin <0-255>   - Min spark heat"));
        Serial.println(F("  set sparkheatmax <0-255>   - Max spark heat"));
        Serial.println(F("  set audiosparkboost <0-1>  - Audio influence on sparks"));
        Serial.println(F("  set audioheatboost <0-255> - Max audio heat boost"));
        Serial.println(F("  set coolingaudiobias <-128..127> - Audio cooling bias"));
        Serial.println(F("  set bottomrows <1-8>       - Spark injection rows"));
        Serial.println(F("  set transientheatmax <0-255> - Transient heat boost"));
        Serial.println(F("  set brightness <0-255>     - LED brightness"));
        Serial.println(F(""));
        Serial.println(F("AUDIO ENGINE:"));
        Serial.println(F("  set gate <0-1>             - Noise gate level"));
        Serial.println(F("  set gain <0-5>             - Global gain"));
        Serial.println(F("  set attack <s>             - Attack time"));
        Serial.println(F("  set release <s>            - Release time"));
        Serial.println(F("  set transientcooldown <ms> - Transient detection cooldown"));
        Serial.println(F(""));
        Serial.println(F("AUTO-GAIN:"));
        Serial.println(F("  ag on/off                  - Enable/disable auto-gain"));
        Serial.println(F("  ag target <0-1>            - Target level"));
        Serial.println(F("  ag strength <0-1>          - Adjustment speed"));
        Serial.println(F("  ag limits <min> <max>      - Gain limits"));
        Serial.println(F("  ag stats                   - Show auto-gain info"));
        Serial.println(F(""));
        Serial.println(F("DEBUGGING:"));
        Serial.println(F("  debug on/off               - Real-time parameter display"));
        Serial.println(F("  debug rate <ms>            - Debug update rate"));
        Serial.println(F("  mic debug on/off           - Mic-specific debug"));
        Serial.println(F("  mic stats                  - One-time mic snapshot"));
        Serial.println(F("  imu stats                  - IMU state"));
        Serial.println(F("  fire stats                 - Fire engine state"));
        Serial.println(F(""));
        Serial.println(F("IMU VISUALIZATION:"));
        Serial.println(F("  imu viz on/off             - Show IMU orientation on matrix"));
        Serial.println(F("  imu test                   - Test IMU mapping"));
        Serial.println(F("  imu calibrate              - Help calibrate IMU orientation"));
        Serial.println(F(""));
        Serial.println(F("BATTERY VISUALIZATION:"));
        Serial.println(F("  battery viz on/off         - Show battery charge level on top row"));
        Serial.println(F("  battery debug              - Show detailed battery readings"));
        Serial.println(F(""));
        Serial.println(F("TEST PATTERN:"));
        Serial.println(F("  test pattern on/off        - Show RGB cycling rows for layout verification"));
        Serial.println(F(""));
        Serial.println(F("IMU ADVANCED:"));
        Serial.println(F("  imu test gravity           - Test gravity-based flame rising"));
        Serial.println(F("  imu mapping                - Guide for understanding IMU coordinates"));
        Serial.println(F("  imu raw                    - Show raw IMU sensor data"));
        Serial.println(F("  imu debug on/off           - Real-time IMU data stream"));
        Serial.println(F("  fire disable/enable        - Disable fire for IMU viz"));
        Serial.println(F(""));
        Serial.println(F("CYLINDER ORIENTATION:"));
        Serial.println(F("  top viz on/off             - Show physical top column indicator"));
        Serial.println(F("  top test                   - Test cylinder orientation detection"));
        Serial.println(F(""));
        Serial.println(F("CONFIGURATION STORAGE:"));
        Serial.println(F("  config save                - Save all settings to EEPROM"));
        Serial.println(F("  config load                - Load all settings from EEPROM"));
        Serial.println(F("  config status              - Show EEPROM configuration status"));
        Serial.println(F("  config factory             - Reset to factory defaults"));
        Serial.println(F("  config device <1-3>        - Set device type (1=Hat, 2=Tube, 3=Bucket)"));
        Serial.println(F(""));
        Serial.println(F("GENERATOR TESTING:"));
        Serial.println(F("  generators                 - Run all generator tests"));
        Serial.println(F("  gen fire                   - Test fire generator"));
        Serial.println(F("  fire all                   - All fire-specific tests"));
        Serial.println(F("  fire color                 - Test fire color generation"));
        Serial.println(F("  gen help                   - Show all generator test commands"));
        Serial.println(F(""));
        Serial.println(F("VERSION UTILITIES:"));
        Serial.println(F("  version check <M.m.p>      - Check if current version >= specified"));
        Serial.println(F("  version compare <M.m.p>    - Compare current version to specified"));
    }
    else if (strcmp(cmd, "show") == 0) {
        printAll();
    }
    else if (strcmp(cmd, "version") == 0) {
        Serial.println(F("=== VERSION INFORMATION ==="));
        Serial.println(F(BLINKY_FULL_VERSION));
        Serial.print(F("Semantic Version: ")); Serial.println(F(BLINKY_VERSION_STRING));
        Serial.print(F("Version Number: ")); Serial.println(BLINKY_VERSION_NUMBER);
        Serial.print(F("Components: "));
        Serial.print(BLINKY_VERSION_MAJOR); Serial.print(F("."));
        Serial.print(BLINKY_VERSION_MINOR); Serial.print(F("."));
        Serial.println(BLINKY_VERSION_PATCH);
        Serial.println();
        Serial.print(F("Build: ")); Serial.println(F(BLINKY_BUILD_TIMESTAMP));
        Serial.print(F("Git Branch: ")); Serial.println(F(BLINKY_GIT_BRANCH));
        Serial.print(F("Git Commit: ")); Serial.println(F(BLINKY_GIT_COMMIT));
        Serial.println();
        Serial.print(F("Device Type: ")); Serial.println(DEVICE_TYPE);
        Serial.print(F("Device Name: ")); Serial.println(config.deviceName);
        Serial.println(F("Hardware: XIAO nRF52840 Sense"));
        Serial.println();
        Serial.println(F("Version Checks:"));
        Serial.print(F("  >= 1.0.0: ")); Serial.println(BLINKY_VERSION_CHECK(1, 0, 0) ? F("YES") : F("NO"));
        Serial.print(F("  >= 1.1.0: ")); Serial.println(BLINKY_VERSION_CHECK(1, 1, 0) ? F("YES") : F("NO"));
        Serial.print(F("  >= 2.0.0: ")); Serial.println(BLINKY_VERSION_CHECK(2, 0, 0) ? F("YES") : F("NO"));
    }
    else if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
    }
    // --- Fire parameters ---
    else if (sscanf(cmd, "set cooling %d", &tempInt) == 1) {
        if (stringFire_) {
            stringFire_->params.baseCooling = (uint8_t)tempInt;
            Serial.print(F("Cooling=")); Serial.println(stringFire_->params.baseCooling);
        } else {
            fire.params.baseCooling = (uint8_t)tempInt;
            Serial.print(F("Cooling=")); Serial.println(fire.params.baseCooling);
        }
        saveFireParameterToEEPROM("baseCooling");
    }
    else if (sscanf(cmd, "set sparkchance %f", &tempFloat) == 1) {
        if (stringFire_) {
            stringFire_->params.sparkChance = tempFloat;
            Serial.print(F("SparkChance=")); Serial.println(stringFire_->params.sparkChance, 3);
        } else {
            fire.params.sparkChance = tempFloat;
            Serial.print(F("SparkChance=")); Serial.println(fire.params.sparkChance, 3);
        }
        saveFireParameterToEEPROM("sparkChance");
    }
    else if (sscanf(cmd, "set sparkheatmin %d", &tempInt) == 1) {
        if (stringFire_) {
            stringFire_->params.sparkHeatMin = (uint8_t)tempInt;
            Serial.print(F("SparkHeatMin=")); Serial.println(stringFire_->params.sparkHeatMin);
        } else {
            fire.params.sparkHeatMin = (uint8_t)tempInt;
            Serial.print(F("SparkHeatMin=")); Serial.println(fire.params.sparkHeatMin);
        }
        saveFireParameterToEEPROM("sparkHeatMin");
    }
    else if (sscanf(cmd, "set sparkheatmax %d", &tempInt) == 1) {
        if (stringFire_) {
            stringFire_->params.sparkHeatMax = (uint8_t)tempInt;
            Serial.print(F("SparkHeatMax=")); Serial.println(stringFire_->params.sparkHeatMax);
        } else {
            fire.params.sparkHeatMax = (uint8_t)tempInt;
            Serial.print(F("SparkHeatMax=")); Serial.println(fire.params.sparkHeatMax);
        }
        saveFireParameterToEEPROM("sparkHeatMax");
    }
    else if (sscanf(cmd, "set audiosparkboost %f", &tempFloat) == 1) {
        if (stringFire_) {
            stringFire_->params.audioSparkBoost = tempFloat;
            Serial.print(F("AudioSparkBoost=")); Serial.println(stringFire_->params.audioSparkBoost, 3);
        } else {
            fire.params.audioSparkBoost = tempFloat;
            Serial.print(F("AudioSparkBoost=")); Serial.println(fire.params.audioSparkBoost, 3);
        }
        saveFireParameterToEEPROM("audioSparkBoost");
    }
    else if (sscanf(cmd, "set audioheatboost %d", &tempInt) == 1) {
        if (stringFire_) {
            stringFire_->params.audioHeatBoostMax = (uint8_t)tempInt;
            Serial.print(F("AudioHeatBoostMax=")); Serial.println(stringFire_->params.audioHeatBoostMax);
        } else {
            fire.params.audioHeatBoostMax = (uint8_t)tempInt;
            Serial.print(F("AudioHeatBoostMax=")); Serial.println(fire.params.audioHeatBoostMax);
        }
        saveFireParameterToEEPROM("audioHeatBoostMax");
    }
    else if (sscanf(cmd, "set coolingaudiobias %d", &tempInt) == 1) {
        if (stringFire_) {
            stringFire_->params.coolingAudioBias = (int8_t)tempInt;
            Serial.print(F("CoolingAudioBias=")); Serial.println(stringFire_->params.coolingAudioBias);
        } else {
            fire.params.coolingAudioBias = (int8_t)tempInt;
            Serial.print(F("CoolingAudioBias=")); Serial.println(fire.params.coolingAudioBias);
        }
        saveFireParameterToEEPROM("coolingAudioBias");
    }
    else if (sscanf(cmd, "set bottomrows %d", &tempInt) == 1) {
        if (stringFire_) {
            Serial.println(F("BottomRowsForSparks not applicable to StringFire (uses sparkPositions instead)"));
        } else {
            fire.params.bottomRowsForSparks = (uint8_t)tempInt;
            Serial.print(F("BottomRowsForSparks=")); Serial.println(fire.params.bottomRowsForSparks);
        }
        saveFireParameterToEEPROM("bottomRowsForSparks");
    }
    else if (sscanf(cmd, "set transientheatmax %d", &tempInt) == 1) {
        if (stringFire_) {
            stringFire_->params.transientHeatMax = constrain(tempInt, 0, 255);
            Serial.print(F("TransientHeatMax="));
            Serial.println(stringFire_->params.transientHeatMax);
        } else {
            fire.params.transientHeatMax = constrain(tempInt, 0, 255);
            Serial.print(F("TransientHeatMax="));
            Serial.println(fire.params.transientHeatMax);
        }
        saveFireParameterToEEPROM("transientHeatMax");
    }

    // --- AdaptiveMic parameters ---
    else if (sscanf(cmd, "set gate %f", &tempFloat) == 1) {
        mic.noiseGate = tempFloat;
        Serial.print(F("NoiseGate=")); Serial.println(mic.noiseGate, 3);
        if (configStorage_) configStorage_->saveMicParam("noiseGate", mic);
    }
    else if (sscanf(cmd, "set gain %f", &tempFloat) == 1) {
        mic.globalGain = tempFloat;
        Serial.print(F("GlobalGain=")); Serial.println(mic.globalGain, 3);
        if (configStorage_) configStorage_->saveMicParam("globalGain", mic);
    }
    else if (sscanf(cmd, "set attack %f", &tempFloat) == 1) {
        mic.attackSeconds = tempFloat;
        Serial.print(F("attackSeconds=")); Serial.println(mic.attackSeconds, 3);
        if (configStorage_) configStorage_->saveMicParam("attackSeconds", mic);
    }
    else if (sscanf(cmd, "set release %f", &tempFloat) == 1) {
        mic.releaseSeconds = tempFloat;
        Serial.print(F("releaseSeconds=")); Serial.println(mic.releaseSeconds, 3);
        if (configStorage_) configStorage_->saveMicParam("releaseSeconds", mic);
    }
    else if (sscanf(cmd, "set transientCooldown %d", &tempInt) == 1) {
        mic.transientCooldownMs = constrain(tempInt, 10, 1000); // 10ms–1s
        Serial.print(F("TransientCooldownMs="));
        Serial.println(mic.transientCooldownMs);
        if (configStorage_) configStorage_->saveMicParam("transientCooldownMs", mic);
    }
    // --- Mic debug tool ---
    else if (strcmp(cmd, "mic debug on") == 0) {
        micDebugEnabled = true; Serial.println(F("MicDebug=on"));
    }
    else if (strcmp(cmd, "mic debug off") == 0) {
        micDebugEnabled = false; Serial.println(F("MicDebug=off"));
    }
    else if (strcmp(cmd, "mic stats") == 0) {
        micDebugPrintLine();
    }

    // --- Auto-gain controls ---
    else if (strcmp(cmd, "ag on") == 0) {
        mic.agEnabled = true; Serial.println(F("AutoGain=on"));
    }
    else if (strcmp(cmd, "ag off") == 0) {
        mic.agEnabled = false; Serial.println(F("AutoGain=off"));
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
        Serial.print(F("AutoGain: enabled=")); Serial.print(mic.agEnabled ? F("yes") : F("no"));
        Serial.print(F(" target="));   Serial.print(mic.agTarget,3);
        Serial.print(F(" strength=")); Serial.print(mic.agStrength,3);
        Serial.print(F(" limits=["));  Serial.print(mic.agMin,2); Serial.print(','); Serial.print(mic.agMax,2); Serial.println(']');
        Serial.print(F(" globalGain=")); Serial.println(mic.globalGain,3);
    }

    // IMU stats - using clean IMU data interface
    else if (strcmp(cmd, "imu stats") == 0) {
      if (imu.isReady() && imu.updateIMUData()) {
        const IMUData& data = imu.getRawIMUData();
        Serial.print(F("IMU up=("));  Serial.print(data.up.x,3);  Serial.print(',');
        Serial.print(data.up.y,3);   Serial.print(',');         Serial.print(data.up.z,3);
        Serial.print(F(")  tilt=")); Serial.print(data.tiltAngle,1); Serial.print(F("°"));
        Serial.print(F("  motion=")); Serial.print(data.motionMagnitude,2);
        Serial.print(data.isMoving ? F(" MOVING") : F(" STILL"));
        Serial.println();
      } else {
        Serial.println(F("IMU not ready"));
      }
    }
    // Legacy IMU config commands removed - use 'imu raw' and 'imu debug' for monitoring

    // --- New enhanced commands ---
    // LED brightness control
    else if (sscanf(cmd, "set brightness %d", &tempInt) == 1) {
        tempInt = constrain(tempInt, 0, 255);
        leds.setBrightness(tempInt);
        Serial.print(F("LED Brightness=")); Serial.println(tempInt);
    }

    // General debug control
    else if (strcmp(cmd, "debug on") == 0) {
        debugEnabled = true; Serial.println(F("Debug=on"));
    }
    else if (strcmp(cmd, "debug off") == 0) {
        debugEnabled = false; Serial.println(F("Debug=off"));
    }
    else if (sscanf(cmd, "debug rate %d", &tempInt) == 1) {
        debugPeriodMs = constrain(tempInt, 100, 5000);
        Serial.print(F("DebugRate=")); Serial.print(debugPeriodMs); Serial.println(F("ms"));
    }

    // Fire stats
    else if (strcmp(cmd, "fire stats") == 0) {
        printFireStats();
    }

    // IMU visualization commands
    else if (strcmp(cmd, "imu viz on") == 0) {
        imuVizEnabled = true;
        Serial.println(F("IMU Orientation Test=on"));
        Serial.println(F("WHITE dot = UP direction (follows gravity)"));
        Serial.println(F("RED edge = Tilted RIGHT, GREEN edge = Tilted LEFT"));
        Serial.println(F("BLUE edge = Tilted FORWARD, YELLOW edge = Tilted BACK"));
        Serial.println(F("DIM corners = Matrix reference points"));
        Serial.println(F("Use 'fire disable' for clearer view"));
    }
    else if (strcmp(cmd, "imu viz off") == 0) {
        imuVizEnabled = false;
        Serial.println(F("IMU Visualization=off"));
    }

    // Battery visualization commands
    else if (strcmp(cmd, "battery viz on") == 0) {
        batteryVizEnabled = true;
        Serial.println(F("Battery Visualization=on"));
        Serial.println(F("Shows battery charge level as horizontal bar on top row"));
        Serial.println(F("GREEN=full, YELLOW=medium, RED=low, BLUE=charging"));
    }
    else if (strcmp(cmd, "battery viz off") == 0) {
        batteryVizEnabled = false;
        Serial.println(F("Battery Visualization=off"));
    }

    // Test pattern commands
    else if (strcmp(cmd, "test pattern on") == 0) {
        testPatternEnabled = true;
        Serial.println(F("Test Pattern=on"));
        Serial.println(F("Shows cycling RGB rows moving upward for layout verification"));
    }
    else if (strcmp(cmd, "test pattern off") == 0) {
        testPatternEnabled = false;
        Serial.println(F("Test Pattern=off"));
    }
    else if (strcmp(cmd, "battery debug") == 0) {
        Serial.println(F("=== BATTERY DEBUG ==="));
        uint16_t raw = battery.readRaw();
        float voltage = battery.readVoltage();
        uint8_t percent = battery.getPercent();
        bool charging = battery.isCharging();

        Serial.print(F("Raw ADC: ")); Serial.println(raw);
        Serial.print(F("Calculated Voltage: ")); Serial.print(voltage, 3); Serial.println(F("V"));
        Serial.print(F("Percentage: ")); Serial.print(percent); Serial.println(F("%"));
        Serial.print(F("Charging: ")); Serial.println(charging ? F("YES") : F("NO"));
        Serial.print(F("Config min/max: ")); Serial.print(config.charging.minVoltage, 1);
        Serial.print(F("V / ")); Serial.print(config.charging.maxVoltage, 1); Serial.println(F("V"));
    }
    else if (strcmp(cmd, "imu test") == 0) {
        Serial.println(F("IMU Test Mode: Tilt device and watch orientation indicators"));
        const MotionState& m = imu.motion();
        Serial.print(F("Current: Up=(")); Serial.print(m.up.x, 2);
        Serial.print(F(","));            Serial.print(m.up.y, 2);
        Serial.print(F(","));            Serial.print(m.up.z, 2);
        Serial.println(F(")"));
    }
    else if (strcmp(cmd, "imu calibrate") == 0) {
        Serial.println(F("=== IMU CALIBRATION ANALYSIS ==="));
        const MotionState& m = imu.motion();
        Serial.print(F("Current up vector: (")); Serial.print(m.up.x, 3);
        Serial.print(F(","));                    Serial.print(m.up.y, 3);
        Serial.print(F(","));                    Serial.print(m.up.z, 3);
        Serial.println(F(")"));

        Serial.println(F("Expected when upright: (0,0,1)"));
        if (m.up.z < -0.8f) Serial.println(F("ISSUE: Z-axis inverted! Try 'imu invert z'"));
        if (fabsf(m.up.x) > 0.2f) Serial.println(F("ISSUE: X-axis tilted"));
        if (fabsf(m.up.y) > 0.2f) Serial.println(F("ISSUE: Y-axis tilted"));

        Serial.println(F(""));
        Serial.println(F("RECOMMENDED FIXES:"));
        Serial.println(F("1. imu invert z           - Fix upside-down mounting"));
    }
    // IMU configuration commands
    else if (strcmp(cmd, "imu invert z") == 0) {
        Serial.println(F("Z-axis inversion not yet implemented"));
        Serial.println(F("This requires code changes to flip Z readings"));
    }
    else if (strcmp(cmd, "imu test gravity") == 0) {
        Serial.println(F("=== GRAVITY-BASED FLAME RISING TEST ==="));
        Serial.println(F("Tilt controller and observe flame behavior:"));
        Serial.println(F(""));
        Serial.println(F("Expected behavior:"));
        Serial.println(F("- UPRIGHT -> Flames rise straight up"));
        Serial.println(F("- TILT LEFT -> Flames lean LEFT (gravity pulls right)"));
        Serial.println(F("- TILT RIGHT -> Flames lean RIGHT (gravity pulls left)"));
        Serial.println(F("- TILT FORWARD -> Flames lean back"));
        Serial.println(F("- TILT BACK -> Flames lean forward"));
        Serial.println(F(""));
        Serial.println(F("Fire uses simplified gravity-based physics only"));
        Serial.println(F("Use 'debug on' to monitor gravity values"));
        const MotionState& m = imu.motion();
        Serial.print(F("Current gravity: (")); Serial.print(-m.up.x, 2);
        Serial.print(F(","));                  Serial.print(-m.up.y, 2);
        Serial.print(F(","));                  Serial.print(-m.up.z, 2);
        Serial.println(F(") - should be ~(0,0,1) when upright"));
    }
    else if (strcmp(cmd, "fire disable") == 0) {
        fireDisabled = true;
        Serial.println(F("Fire effect disabled (IMU viz clearer)"));
    }
    else if (strcmp(cmd, "fire enable") == 0) {
        fireDisabled = false;
        Serial.println(F("Fire effect enabled"));
    }
    // Cylinder orientation visualization commands
    else if (strcmp(cmd, "top viz on") == 0) {
        heatVizEnabled = true;
        Serial.println(F("Cylinder Top Visualization=on"));
        Serial.println(F("GREEN line = Physical top of cylinder"));
        Serial.println(F("Shows which column is physically highest when tilted"));
    }
    else if (strcmp(cmd, "top viz off") == 0) {
        heatVizEnabled = false;
        Serial.println(F("Cylinder Top Visualization=off"));
    }
    else if (strcmp(cmd, "top test") == 0) {
        renderTopVisualization();
        Serial.println(F("Top test - check which column shows GREEN line"));

        // Debug IMU readings for top visualization using clean data
        if (imu.isReady() && imu.updateIMUData()) {
            const IMUData& data = imu.getRawIMUData();
            float circumferenceMagnitude = sqrt(data.up.y * data.up.y + data.up.z * data.up.z);
            float angle = atan2(data.up.z, data.up.y);
            angle += PI / 2.0f; // Add 90-degree correction
            float normalizedAngle = (angle + PI) / (2.0f * PI);
            int topColumn = (int)(normalizedAngle * ledMapper.getWidth() + 0.5f) % ledMapper.getWidth();

            Serial.print(F("RAW accel=(")); Serial.print(data.accel.x, 3); Serial.print(F(","));
            Serial.print(data.accel.y, 3); Serial.print(F(","));  Serial.print(data.accel.z, 3); Serial.print(F(")"));
            Serial.print(F(" UP=(")); Serial.print(data.up.x, 3); Serial.print(F(","));
            Serial.print(data.up.y, 3); Serial.print(F(","));  Serial.print(data.up.z, 3); Serial.print(F(")"));
            Serial.print(F(" upX=")); Serial.print(data.up.x, 3);
            Serial.print(F(" circumfMag=")); Serial.print(circumferenceMagnitude, 3);
            Serial.print(F(" angle=")); Serial.print(angle, 3);
            Serial.print(F(" -> column=")); Serial.println(topColumn);

            if (circumferenceMagnitude < 0.3f) {
                Serial.println(F("Status: Cylinder upright (X-axis dominates) - showing full green top row"));
            } else {
                Serial.print(F("Status: Cylinder tilted - top at column ")); Serial.println(topColumn);
            }
        } else {
            Serial.println(F("IMU not ready or failed to update"));
        }
    }
    else if (strcmp(cmd, "imu mapping") == 0) {
        Serial.println(F("=== IMU COORDINATE MAPPING TEST ==="));
        Serial.println(F("Follow these steps to understand IMU orientation:"));
        Serial.println(F(""));
        Serial.println(F("1. Hold cylinder UPRIGHT, run 'imu stats'"));
        Serial.println(F("2. Tilt cylinder LEFT, run 'imu stats'"));
        Serial.println(F("3. Tilt cylinder RIGHT, run 'imu stats'"));
        Serial.println(F("4. Tilt cylinder FORWARD, run 'imu stats'"));
        Serial.println(F("5. Tilt cylinder BACKWARD, run 'imu stats'"));
        Serial.println(F("6. Lay cylinder on SIDE, run 'imu stats'"));
        Serial.println(F(""));
        Serial.println(F("This will show which IMU axis corresponds to each physical direction"));
    }
    else if (strcmp(cmd, "imu raw") == 0) {
        printRawIMUData();
    }
    else if (strcmp(cmd, "imu debug on") == 0) {
        imuDebugEnabled = true;
        Serial.println(F("IMU Debug=on"));
        Serial.println(F("Real-time IMU sensor data streaming"));
    }
    else if (strcmp(cmd, "imu debug off") == 0) {
        imuDebugEnabled = false;
        Serial.println(F("IMU Debug=off"));
    }

    // --- Configuration storage commands ---
    else if (strcmp(cmd, "config save") == 0) {
        if (configStorage_) {
            if (stringFire_) {
                configStorage_->saveConfiguration(stringFire_->params, mic);
            } else {
                configStorage_->saveConfiguration(fire.params, mic);
            }
            Serial.println(F("Configuration saved to EEPROM"));
        } else {
            Serial.println(F("ERROR: Configuration storage not available"));
        }
    }
    else if (strcmp(cmd, "config load") == 0) {
        if (configStorage_) {
            if (stringFire_) {
                configStorage_->loadConfiguration(stringFire_->params, mic);
            } else {
                configStorage_->loadConfiguration(fire.params, mic);
            }
            Serial.println(F("Configuration loaded from EEPROM"));
        } else {
            Serial.println(F("ERROR: Configuration storage not available"));
        }
    }
    else if (strcmp(cmd, "config status") == 0) {
        if (configStorage_) {
            configStorage_->printStatus();
        } else {
            Serial.println(F("ERROR: Configuration storage not available"));
        }
    }
    else if (strcmp(cmd, "config factory") == 0) {
        if (configStorage_) {
            configStorage_->factoryReset();
            // Reload the defaults into current parameters
            if (stringFire_) {
                stringFire_->restoreDefaults();
            } else {
                fire.restoreDefaults();
            }
            Serial.println(F("Factory reset complete - defaults restored"));
        } else {
            Serial.println(F("ERROR: Configuration storage not available"));
        }
    }
    else if (sscanf(cmd, "config device %d", &tempInt) == 1) {
        if (tempInt >= 1 && tempInt <= 3) {
            if (configStorage_) {
                configStorage_->setDeviceType((uint8_t)tempInt);
                Serial.print(F("Device type set to: "));
                Serial.println(tempInt);
                Serial.println(F("NOTE: Recompile and reflash with DEVICE_TYPE="));
                Serial.print(tempInt);
                Serial.println(F(" to use the new configuration"));
            } else {
                Serial.println(F("ERROR: Configuration storage not available"));
            }
        } else {
            Serial.println(F("ERROR: Device type must be 1, 2, or 3"));
        }
    }

    // --- Version utility commands ---
    else if (strncmp(cmd, "version check ", 14) == 0) {
        int major, minor, patch;
        if (sscanf(cmd + 14, "%d.%d.%d", &major, &minor, &patch) == 3) {
            bool result = BLINKY_VERSION_CHECK(major, minor, patch);
            Serial.print(F("Version check ")); Serial.print(BLINKY_VERSION_STRING);
            Serial.print(F(" >= ")); Serial.print(major); Serial.print(F("."));
            Serial.print(minor); Serial.print(F(".")); Serial.print(patch);
            Serial.print(F(" = ")); Serial.println(result ? F("TRUE") : F("FALSE"));
        } else {
            Serial.println(F("Usage: version check <major>.<minor>.<patch>"));
        }
    }
    else if (strncmp(cmd, "version compare ", 16) == 0) {
        int major, minor, patch;
        if (sscanf(cmd + 16, "%d.%d.%d", &major, &minor, &patch) == 3) {
            uint32_t otherVersion = major * 10000 + minor * 100 + patch;
            uint32_t currentVersion = BLINKY_VERSION_NUMBER;

            Serial.print(F("Current: ")); Serial.print(BLINKY_VERSION_STRING);
            Serial.print(F(" (")); Serial.print(currentVersion); Serial.println(F(")"));
            Serial.print(F("Compare: ")); Serial.print(major); Serial.print(F("."));
            Serial.print(minor); Serial.print(F(".")); Serial.print(patch);
            Serial.print(F(" (")); Serial.print(otherVersion); Serial.println(F(")"));

            if (currentVersion > otherVersion) {
                Serial.println(F("Result: NEWER"));
            } else if (currentVersion < otherVersion) {
                Serial.println(F("Result: OLDER"));
            } else {
                Serial.println(F("Result: EQUAL"));
            }
        } else {
            Serial.println(F("Usage: version compare <major>.<minor>.<patch>"));
        }
    }

    // --- Generator testing commands ---
    else if (strncmp(cmd, "test ", 5) == 0 || strncmp(cmd, "gen ", 4) == 0 || strncmp(cmd, "fire ", 5) == 0) {
        if (testRunner_) {
            testRunner_->handleCommand(cmd);
        } else {
            Serial.println(F("ERROR: Generator test runner not available"));
        }
    }

    else {
        Serial.println(F("Unknown command. Type 'help' for commands."));
    }
}

void SerialConsole::restoreDefaults() {
    // First restore fire effect defaults (now uses config values)
    fire.restoreDefaults();

    // Then restore microphone defaults from global defaults (these aren't device-specific)
    mic.noiseGate           = Defaults::NoiseGate;
    mic.globalGain          = Defaults::GlobalGain;
    mic.attackSeconds       = Defaults::AttackSeconds;
    mic.releaseSeconds      = Defaults::ReleaseSeconds;
    mic.transientCooldownMs = Defaults::TransientCooldownMs;

    // Note: fire.params.transientHeatMax is now set by fire.restoreDefaults()
    Serial.print(F("Defaults restored for device: "));
    Serial.println(config.deviceName);
}

void SerialConsole::printAll() {
    Serial.println(F("=== CURRENT SETTINGS ==="));

    // Fire Engine
    Serial.println(F("FIRE ENGINE:"));
    Serial.print(F("  Cooling: ")); Serial.println(fire.params.baseCooling);
    Serial.print(F("  SparkChance: ")); Serial.println(fire.params.sparkChance, 3);
    Serial.print(F("  SparkHeat: ")); Serial.print(fire.params.sparkHeatMin);
    Serial.print(F(" - ")); Serial.println(fire.params.sparkHeatMax);
    Serial.print(F("  AudioSparkBoost: ")); Serial.println(fire.params.audioSparkBoost, 3);
    Serial.print(F("  AudioHeatBoostMax: ")); Serial.println(fire.params.audioHeatBoostMax);
    Serial.print(F("  CoolingAudioBias: ")); Serial.println(fire.params.coolingAudioBias);
    Serial.print(F("  BottomRowsForSparks: ")); Serial.println(fire.params.bottomRowsForSparks);
    Serial.print(F("  TransientHeatMax: ")); Serial.println(fire.params.transientHeatMax);
    Serial.print(F("  LED Brightness: ")); Serial.println(leds.getBrightness());


    // Audio Engine
    Serial.println(F("AUDIO:"));
    Serial.print(F("  NoiseGate: ")); Serial.println(mic.noiseGate, 3);
    Serial.print(F("  GlobalGain: ")); Serial.println(mic.globalGain, 3);
    Serial.print(F("  Attack: ")); Serial.print(mic.attackSeconds, 3); Serial.println(F("s"));
    Serial.print(F("  Release: ")); Serial.print(mic.releaseSeconds, 3); Serial.println(F("s"));
    Serial.print(F("  TransientCooldown: ")); Serial.print(mic.transientCooldownMs); Serial.println(F("ms"));
    Serial.print(F("  HW Gain: ")); Serial.println(mic.getHwGain());

    // Auto-gain
    Serial.println(F("AUTO-GAIN:"));
    Serial.print(F("  Enabled: ")); Serial.println(mic.agEnabled ? F("YES") : F("NO"));
    Serial.print(F("  Target: ")); Serial.println(mic.agTarget, 3);
    Serial.print(F("  Strength: ")); Serial.println(mic.agStrength, 3);
    Serial.print(F("  Limits: ")); Serial.print(mic.agMin, 2);
    Serial.print(F(" - ")); Serial.println(mic.agMax, 2);

    // Debug
    Serial.println(F("DEBUG:"));
    Serial.print(F("  General: ")); Serial.println(debugEnabled ? F("ON") : F("OFF"));
    Serial.print(F("  Mic: ")); Serial.println(micDebugEnabled ? F("ON") : F("OFF"));
    Serial.print(F("  IMU Viz: ")); Serial.println(imuVizEnabled ? F("ON") : F("OFF"));
    Serial.print(F("  Top Viz: ")); Serial.println(heatVizEnabled ? F("ON") : F("OFF"));
    Serial.print(F("  Fire: ")); Serial.println(fireDisabled ? F("DISABLED") : F("ENABLED"));
    Serial.print(F("  Rate: ")); Serial.print(debugPeriodMs); Serial.println(F("ms"));

    // Battery - detailed system data
    Serial.println(F("BATTERY SYSTEM:"));
    Serial.print(F("  Raw ADC: ")); Serial.println(battery.readRaw());
    Serial.print(F("  Voltage: ")); Serial.print(battery.readVoltage(), 3); Serial.println(F("V"));
    Serial.print(F("  Smoothed Voltage: ")); Serial.print(battery.getVoltage(), 3); Serial.println(F("V"));
    Serial.print(F("  Percentage: ")); Serial.print(battery.getPercent()); Serial.println(F("%"));
    Serial.print(F("  Charging: ")); Serial.println(battery.isCharging() ? F("YES") : F("NO"));
    Serial.print(F("  Fast Charge: ")); Serial.println(config.charging.fastChargeEnabled ? F("ENABLED") : F("DISABLED"));
    Serial.print(F("  Config Min/Max: ")); Serial.print(config.charging.minVoltage, 1);
    Serial.print(F("V / ")); Serial.print(config.charging.maxVoltage, 1); Serial.println(F("V"));
    Serial.print(F("  Low/Critical: ")); Serial.print(config.charging.lowBatteryThreshold, 1);
    Serial.print(F("V / ")); Serial.print(config.charging.criticalBatteryThreshold, 1); Serial.println(F("V"));
    Serial.print(F("  Auto Viz When Charging: ")); Serial.println(config.charging.autoShowVisualizationWhenCharging ? F("YES") : F("NO"));
}

void SerialConsole::renderIMUVisualization() {
    if (!imuVizEnabled) return;

    // Clear the matrix
    for (int i = 0; i < ledMapper.getTotalPixels(); i++) {
        leds.setPixelColor(i, 0);
    }

    const MotionState& m = imu.motion();

    // Matrix dimensions from mapper
    const int WIDTH = ledMapper.getWidth();
    const int HEIGHT = ledMapper.getHeight();

    // Helper function to set pixel safely
    auto setPixel = [&](int x, int y, uint32_t color) {
        int index = ledMapper.getIndex(x, y);
        if (index >= 0) {
            leds.setPixelColor(index, color);
        }
    };

    // SIMPLE ORIENTATION TEST
    // Show which way the controller thinks is "UP" using gravity

    // 1. ALWAYS show corner reference points in DIM WHITE
    setPixel(0, 0, leds.Color(16, 16, 16));           // Top-left
    setPixel(WIDTH-1, 0, leds.Color(16, 16, 16));     // Top-right
    setPixel(0, HEIGHT-1, leds.Color(16, 16, 16));    // Bottom-left
    setPixel(WIDTH-1, HEIGHT-1, leds.Color(16, 16, 16)); // Bottom-right

    // 2. Show UP direction with a BRIGHT WHITE dot
    // The dot will move to show which edge/corner is "up" according to IMU

    // Map gravity vector (up.x, up.y, up.z) to matrix position
    // up.z = vertical (controller face up/down)
    // up.x = horizontal tilt left/right
    // up.y = horizontal tilt forward/back

    // Convert normalized up vector (-1 to 1) to matrix coordinates
    int upX = (int)((m.up.x + 1.0f) * (WIDTH - 1) / 2.0f);   // -1..1 -> 0..15
    int upY = (int)((m.up.y + 1.0f) * (HEIGHT - 1) / 2.0f);  // -1..1 -> 0..7

    upX = constrain(upX, 0, WIDTH-1);
    upY = constrain(upY, 0, HEIGHT-1);

    setPixel(upX, upY, leds.Color(255, 255, 255)); // BRIGHT WHITE = UP

    // 3. Show tilt magnitude with colored intensity at edges
    // Red intensity on edges shows how much the controller is tilted

    float tiltMagnitude = sqrt(m.up.x * m.up.x + m.up.y * m.up.y); // 0 = upright, 1 = flat
    int tiltIntensity = (int)(tiltMagnitude * 128); // 0-128 intensity

    if (tiltIntensity > 10) {
        // Show tilt direction with colored edges
        if (m.up.x > 0.3f) { // Tilted right
            for (int y = 1; y < HEIGHT-1; y++) {
                setPixel(WIDTH-1, y, leds.Color(tiltIntensity, 0, 0)); // Red right edge
            }
        }
        if (m.up.x < -0.3f) { // Tilted left
            for (int y = 1; y < HEIGHT-1; y++) {
                setPixel(0, y, leds.Color(0, tiltIntensity, 0)); // Green left edge
            }
        }
        if (m.up.y > 0.3f) { // Tilted forward
            for (int x = 1; x < WIDTH-1; x++) {
                setPixel(x, HEIGHT-1, leds.Color(0, 0, tiltIntensity)); // Blue bottom edge
            }
        }
        if (m.up.y < -0.3f) { // Tilted back
            for (int x = 1; x < WIDTH-1; x++) {
                setPixel(x, 0, leds.Color(tiltIntensity, tiltIntensity, 0)); // Yellow top edge
            }
        }
    }

    leds.show();
}

int SerialConsole::xyToPixelIndex(int x, int y) {
    // Get current device config for consistent mapping
    extern const DeviceConfig& config;
    int width = config.matrix.width;
    int height = config.matrix.height;

    // Wrap coordinates
    x = (x % width + width) % width;
    y = (y % height + height) % height;

    // Handle different matrix orientations and wiring patterns
    if (config.matrix.orientation == VERTICAL && width == 4 && height == 15) {
        // Tube light: 4x15 zigzag pattern
        if (x % 2 == 0) {
            // Even columns (0,2): normal top-to-bottom
            return x * height + y;
        } else {
            // Odd columns (1,3): bottom-to-top (reversed)
            return x * height + (height - 1 - y);
        }
    } else {
        // Fire totem: standard row-major mapping
        return y * width + x;
    }
}

void SerialConsole::renderTopVisualization() {
    if (!heatVizEnabled) return;

    // Matrix dimensions from mapper
    const int WIDTH = ledMapper.getWidth();
    const int HEIGHT = ledMapper.getHeight();

    // Ensure IMU is updated for this visualization
    if (!imu.isReady()) return;

    // Update and get clean IMU data
    if (!imu.updateIMUData()) return;
    const IMUData& data = imu.getRawIMUData();

    // COORDINATE SYSTEM MAPPING:
    // Cylinder length axis = X-axis (UP.x indicates vertical orientation)
    // Cylinder circumference = Y-Z plane (UP.y, UP.z indicate which side is up)

    // Check if cylinder is upright (X-axis dominates)
    float circumferenceMagnitude = sqrt(data.up.y * data.up.y + data.up.z * data.up.z);

    if (circumferenceMagnitude < 0.3f) {
        // Cylinder is mostly upright (vertical) - show green across top row
        for (int x = 0; x < WIDTH; x++) {
            leds.setPixelColor(x, leds.Color(100, 0, 0)); // Green top row (GRB format)
        }
    } else {
        // Cylinder is tilted - determine which column is highest
        // Use Y-Z components (circumference plane) to find direction

        // Calculate angle around cylinder circumference using Y-Z plane
        float angle = atan2(data.up.z, data.up.y); // Angle in radians

        // Add 90-degree offset to correct the orientation (PI/2 radians = 90 degrees)
        angle += PI / 2.0f;

        // Convert angle to column position (0-15 around cylinder)
        // Add PI to shift from [-PI,PI] to [0,2PI], then normalize to [0,1]
        float normalizedAngle = (angle + PI) / (2.0f * PI);
        int topColumn = (int)(normalizedAngle * WIDTH + 0.5f) % WIDTH;

        // Show green vertical line on the physically highest column
        for (int y = 0; y < HEIGHT; y++) {
            int index = y * WIDTH + topColumn;
            leds.setPixelColor(index, leds.Color(255, 0, 0)); // Bright green column (GRB format)
        }
    }

    leds.show();
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
    Serial.print("envAR=");       Serial.print(mic.getEnv(), 3);
    Serial.print(" envMean=");    Serial.print(mic.getEnvMean(), 3);
    Serial.print(" pre=");        Serial.print(mic.getLevelPreGate(), 3);
    Serial.print(" post=");       Serial.print(mic.getLevelPostAGC(), 3);
    Serial.print(" gGain=");      Serial.print(mic.getGlobalGain(), 3);
    Serial.print(" hwGain=");     Serial.print(mic.getHwGain());
    Serial.print(" isrCnt=");     Serial.print(mic.getIsrCount());
    Serial.println();
}

// ---- General debug output ----
void SerialConsole::debugTick() {
    if (!debugEnabled) return;
    unsigned long now = millis();
    if (now - debugLastMs >= debugPeriodMs) {
        debugLastMs = now;

        // Compact real-time status line
        Serial.print(F("FIRE: level=")); Serial.print(mic.getLevel(), 2);
        Serial.print(F(" hit=")); Serial.print(mic.getTransient(), 2);
        Serial.print(F(" cool=")); Serial.print(fire.params.baseCooling);
        Serial.print(F(" spark=")); Serial.print(fire.params.sparkChance, 2);


        Serial.print(F(" bright=")); Serial.print(leds.getBrightness());
        Serial.println();
    }
}

void SerialConsole::printFireStats() {
    Serial.println(F("=== FIRE ENGINE STATUS ==="));

    // Core parameters
    Serial.print(F("Cooling: ")); Serial.print(fire.params.baseCooling);
    Serial.print(F("  SparkChance: ")); Serial.print(fire.params.sparkChance, 3);
    Serial.print(F("  SparkHeat: ")); Serial.print(fire.params.sparkHeatMin);
    Serial.print(F("-")); Serial.println(fire.params.sparkHeatMax);

    Serial.print(F("AudioSparkBoost: ")); Serial.print(fire.params.audioSparkBoost, 3);
    Serial.print(F("  AudioHeatBoost: ")); Serial.print(fire.params.audioHeatBoostMax);
    Serial.print(F("  CoolingBias: ")); Serial.println(fire.params.coolingAudioBias);

    // Audio levels
    Serial.print(F("Current Audio: Level=")); Serial.print(mic.getLevel(), 3);
    Serial.print(F("  Transient=")); Serial.print(mic.getTransient(), 3);
    Serial.print(F("  Gate=")); Serial.print(mic.noiseGate, 3);
    Serial.print(F("  Gain=")); Serial.println(mic.globalGain, 3);


    // Hardware
    Serial.print(F("LED Brightness: ")); Serial.print(leds.getBrightness());
    Serial.print(F("  HW Gain: ")); Serial.print(mic.getHwGain());
    Serial.print(F("  AutoGain: ")); Serial.println(mic.agEnabled ? F("ON") : F("OFF"));

    // Battery status
    float vbat = battery.readVoltage();
    uint8_t soc = BatteryMonitor::voltageToPercent(vbat);
    Serial.print(F("Battery: ")); Serial.print(vbat, 2); Serial.print(F("V ("));
    Serial.print(soc); Serial.print(F("%) "));
    Serial.println(battery.isCharging() ? F("CHARGING") : F(""));
}

// ---- IMU debug functions ----
void SerialConsole::imuDebugTick() {
    if (!imuDebugEnabled) return;
    unsigned long now = millis();
    if (now - imuDebugLastMs >= imuDebugPeriodMs) {
        imuDebugLastMs = now;
        printRawIMUData();
    }
}

void SerialConsole::printRawIMUData() {
    if (!imu.isReady()) {
        Serial.println(F("IMU not ready"));
        return;
    }

    // Update fresh IMU data
    if (!imu.updateIMUData()) {
        Serial.println(F("Failed to read IMU data"));
        return;
    }

    const IMUData& data = imu.getRawIMUData();

    // Print raw sensor data
    Serial.print(F("RAW: accel=("));
    Serial.print(data.accel.x, 3); Serial.print(F(","));
    Serial.print(data.accel.y, 3); Serial.print(F(","));
    Serial.print(data.accel.z, 3); Serial.print(F(")"));

    Serial.print(F(" gyro=("));
    Serial.print(data.gyro.x, 3); Serial.print(F(","));
    Serial.print(data.gyro.y, 3); Serial.print(F(","));
    Serial.print(data.gyro.z, 3); Serial.print(F(")"));

    Serial.print(F(" temp=")); Serial.print(data.temp, 1); Serial.print(F("C"));

    // Print processed orientation data
    Serial.print(F(" | UP=("));
    Serial.print(data.up.x, 3); Serial.print(F(","));
    Serial.print(data.up.y, 3); Serial.print(F(","));
    Serial.print(data.up.z, 3); Serial.print(F(")"));

    Serial.print(F(" tilt=")); Serial.print(data.tiltAngle, 1); Serial.print(F("°"));

    // Print motion data
    Serial.print(F(" motion=")); Serial.print(data.motionMagnitude, 2);
    Serial.print(data.isMoving ? F(" MOVING") : F(" STILL"));

    Serial.println();
}

void SerialConsole::renderBatteryVisualization() {
    // Clear the display first
    for (int i = 0; i < leds.numPixels(); i++) {
        leds.setPixelColor(i, 0);
    }

    // Get current device config
    extern const DeviceConfig& config;
    int width = config.matrix.width;
    int height = config.matrix.height;

    // Get battery voltage and normalize it
    float voltage = battery.getVoltage();
    if (voltage <= 0) {
        // No battery data available - show red error indication on top row
        for (int x = 0; x < width; x++) {
            int pixelIndex = xyToPixelIndex(x, height - 1); // Top row
            leds.setPixelColor(pixelIndex, leds.Color(50, 0, 0)); // Dim red
        }
        leds.show();
        return;
    }

    // Include config for voltage range
    float minV = config.charging.minVoltage;
    float maxV = config.charging.maxVoltage;

    // Normalize voltage to 0-1 range
    float chargeLevel = (voltage - minV) / (maxV - minV);
    chargeLevel = constrain(chargeLevel, 0.0f, 1.0f);

    // Calculate how many pixels to light up (width pixels total on top row)
    int numPixels = (int)(chargeLevel * width);

    // Determine if charging
    bool isCharging = battery.isCharging();

    // Draw battery visualization on top row only
    const int topRow = height - 1;
    for (int x = 0; x < width; x++) {
        int pixelIndex = xyToPixelIndex(x, topRow);

        if (x < numPixels) {
            // Lit portion of battery meter
            uint32_t color;
            if (isCharging) {
                // Blue when charging
                color = leds.Color(0, 50, 255);
            } else if (chargeLevel > 0.6f) {
                // Green when full
                color = leds.Color(0, 255, 0);
            } else if (chargeLevel > 0.3f) {
                // Yellow when medium
                color = leds.Color(255, 255, 0);
            } else {
                // Red when low
                color = leds.Color(255, 0, 0);
            }
            leds.setPixelColor(pixelIndex, color);
        } else {
            // Empty portion - dim outline
            leds.setPixelColor(pixelIndex, leds.Color(5, 5, 5)); // Very dim white outline
        }
    }

    leds.show();
}

void SerialConsole::renderTestPattern() {
    // Get current config for matrix dimensions
    extern const DeviceConfig& config;
    int width = config.matrix.width;
    int height = config.matrix.height;

    // Clear the display first
    for (int i = 0; i < leds.numPixels(); i++) {
        leds.setPixelColor(i, 0);
    }

    // Create cycling RGB pattern that moves upward
    static unsigned long lastUpdate = 0;
    static int offset = 0;

    if (millis() - lastUpdate > 500) { // Update every 500ms
        lastUpdate = millis();
        offset = (offset + 1) % (height + 3); // +3 for the three colors
    }

    // Draw the pattern
    for (int y = 0; y < height; y++) {
        uint32_t color = 0;
        int colorIndex = (y + offset) % 3;

        switch (colorIndex) {
            case 0: color = leds.Color(255, 0, 0); break; // Red
            case 1: color = leds.Color(0, 255, 0); break; // Green
            case 2: color = leds.Color(0, 0, 255); break; // Blue
        }

        // Light up entire row
        for (int x = 0; x < width; x++) {
            int pixelIndex = xyToPixelIndex(x, y);
            if (pixelIndex < leds.numPixels()) {
                leds.setPixelColor(pixelIndex, color);
            }
        }
    }

    leds.show();
}

// Helper function to save fire parameters based on active fire type
void SerialConsole::saveFireParameterToEEPROM(const char* paramName) {
    if (!configStorage_) return;

    if (stringFire_) {
        configStorage_->saveStringFireParam(paramName, stringFire_->params);
    } else {
        configStorage_->saveFireParam(paramName, fire.params);
    }
}
