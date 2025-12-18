#include "SerialConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../tests/GeneratorTestRunner.h"
#include "../types/Version.h"

extern BatteryMonitor battery;
extern IMUHelper imu;
extern const DeviceConfig& config;
extern LEDMapper ledMapper;

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

SerialConsole::SerialConsole(Fire* fireGen, AdaptiveMic* mic, Adafruit_NeoPixel& l)
    : fireGenerator_(fireGen), mic_(mic), leds_(l),
      configStorage_(nullptr), testRunner_(nullptr) {
    instance_ = this;
}

void SerialConsole::begin() {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 2000)) { delay(10); }

    settings_.begin();
    registerSettings();

    Serial.println(F("Serial console ready. Type 'help' for commands."));
}

// Static callback - forwards to instance method
void SerialConsole::onFireParamChanged() {
    if (instance_ && instance_->fireGenerator_) {
        // Get fire params from the generator and update
        // The settings are directly modifying the generator's params
    }
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
    if (mic_) {
        settings_.registerFloat("gate", &mic_->noiseGate, "audio",
            "Noise gate threshold", 0.0f, 1.0f);
        settings_.registerFloat("gain", &mic_->globalGain, "audio",
            "Global gain multiplier", 0.1f, 10.0f);
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

    // === DEBUG SETTINGS ===
    settings_.registerBool("micdebug", &micDebugEnabled_, "debug",
        "Mic debug output", nullptr, false);
    settings_.registerBool("debug", &debugEnabled_, "debug",
        "General debug output", nullptr, false);
    settings_.registerBool("imudebug", &imuDebugEnabled_, "debug",
        "IMU debug output", nullptr, false);
    settings_.registerUint16("debugrate", &debugPeriodMs_, "debug",
        "Debug output rate (ms)", 100, 5000, nullptr, false);

    // === VISUALIZATION SETTINGS ===
    settings_.registerBool("imuviz", &imuVizEnabled, "viz",
        "IMU visualization", nullptr, false);
    settings_.registerBool("batteryviz", &batteryVizEnabled, "viz",
        "Battery visualization", nullptr, false);
    settings_.registerBool("testpattern", &testPatternEnabled, "viz",
        "Test pattern", nullptr, false);
    settings_.registerBool("topviz", &heatVizEnabled, "viz",
        "Top column visualization", nullptr, false);
    settings_.registerBool("firedisabled", &fireDisabled, "viz",
        "Fire effect disabled", nullptr, false);
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
        if (len > 0) {
            handleCommand(buf);
        }
    }

    // Periodic debug outputs
    micDebugTick();
    debugTick();
    imuDebugTick();
}

void SerialConsole::handleCommand(const char* cmd) {
    // First try settings registry
    if (settings_.handleCommand(cmd)) {
        return;
    }

    // Then try special commands
    if (handleSpecialCommand(cmd)) {
        return;
    }

    Serial.println(F("Unknown command. Type 'help' for available commands."));
}

bool SerialConsole::handleSpecialCommand(const char* cmd) {
    // === GENERAL COMMANDS ===
    if (strcmp(cmd, "help") == 0) {
        printHelp();
        return true;
    }

    if (strcmp(cmd, "version") == 0) {
        printVersionInfo();
        return true;
    }

    if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
        return true;
    }

    // === CONFIGURATION COMMANDS ===
    if (strcmp(cmd, "save") == 0) {
        if (configStorage_ && fireGenerator_ && mic_) {
            configStorage_->saveConfiguration(fireGenerator_->getParams(), *mic_);
            Serial.println(F("Configuration saved to flash"));
        } else {
            Serial.println(F("ERROR: Storage not available"));
        }
        return true;
    }

    if (strcmp(cmd, "load") == 0) {
        if (configStorage_ && fireGenerator_ && mic_) {
            configStorage_->loadConfiguration(fireGenerator_->getParams(), *mic_);
            Serial.println(F("Configuration loaded from flash"));
        } else {
            Serial.println(F("ERROR: Storage not available"));
        }
        return true;
    }

    if (strcmp(cmd, "reset") == 0 || strcmp(cmd, "factory") == 0) {
        if (configStorage_) {
            configStorage_->factoryReset();
            restoreDefaults();
            Serial.println(F("Factory reset complete"));
        } else {
            Serial.println(F("ERROR: Storage not available"));
        }
        return true;
    }

    // === STATS COMMANDS ===
    if (strcmp(cmd, "mic stats") == 0) {
        printMicStats();
        return true;
    }

    if (strcmp(cmd, "fire stats") == 0) {
        printFireStats();
        return true;
    }

    if (strcmp(cmd, "battery stats") == 0) {
        printBatteryStats();
        return true;
    }

    if (strcmp(cmd, "imu stats") == 0) {
        printRawIMUData();
        return true;
    }

    // === SHORTCUT COMMANDS ===
    // Auto-gain shortcuts
    if (strcmp(cmd, "ag on") == 0) {
        if (mic_) { mic_->agEnabled = true; Serial.println(F("AutoGain=on")); }
        return true;
    }
    if (strcmp(cmd, "ag off") == 0) {
        if (mic_) { mic_->agEnabled = false; Serial.println(F("AutoGain=off")); }
        return true;
    }
    if (strcmp(cmd, "ag stats") == 0) {
        if (mic_) {
            Serial.print(F("AutoGain: enabled=")); Serial.print(mic_->agEnabled ? F("yes") : F("no"));
            Serial.print(F(" target=")); Serial.print(mic_->agTarget, 3);
            Serial.print(F(" strength=")); Serial.print(mic_->agStrength, 3);
            Serial.print(F(" gain=")); Serial.print(mic_->globalGain, 3);
            Serial.print(F(" limits=[")); Serial.print(mic_->agMin, 2);
            Serial.print(','); Serial.print(mic_->agMax, 2); Serial.println(']');
        }
        return true;
    }

    // Visualization shortcuts
    if (strcmp(cmd, "imu viz on") == 0) { imuVizEnabled = true; Serial.println(F("IMU Viz=on")); return true; }
    if (strcmp(cmd, "imu viz off") == 0) { imuVizEnabled = false; Serial.println(F("IMU Viz=off")); return true; }
    if (strcmp(cmd, "battery viz on") == 0) { batteryVizEnabled = true; Serial.println(F("Battery Viz=on")); return true; }
    if (strcmp(cmd, "battery viz off") == 0) { batteryVizEnabled = false; Serial.println(F("Battery Viz=off")); return true; }
    if (strcmp(cmd, "test pattern on") == 0) { testPatternEnabled = true; Serial.println(F("Test Pattern=on")); return true; }
    if (strcmp(cmd, "test pattern off") == 0) { testPatternEnabled = false; Serial.println(F("Test Pattern=off")); return true; }
    if (strcmp(cmd, "top viz on") == 0) { heatVizEnabled = true; Serial.println(F("Top Viz=on")); return true; }
    if (strcmp(cmd, "top viz off") == 0) { heatVizEnabled = false; Serial.println(F("Top Viz=off")); return true; }
    if (strcmp(cmd, "fire disable") == 0) { fireDisabled = true; Serial.println(F("Fire disabled")); return true; }
    if (strcmp(cmd, "fire enable") == 0) { fireDisabled = false; Serial.println(F("Fire enabled")); return true; }

    // Debug shortcuts
    if (strcmp(cmd, "mic debug on") == 0) { micDebugEnabled_ = true; Serial.println(F("Mic Debug=on")); return true; }
    if (strcmp(cmd, "mic debug off") == 0) { micDebugEnabled_ = false; Serial.println(F("Mic Debug=off")); return true; }
    if (strcmp(cmd, "debug on") == 0) { debugEnabled_ = true; Serial.println(F("Debug=on")); return true; }
    if (strcmp(cmd, "debug off") == 0) { debugEnabled_ = false; Serial.println(F("Debug=off")); return true; }
    if (strcmp(cmd, "imu debug on") == 0) { imuDebugEnabled_ = true; Serial.println(F("IMU Debug=on")); return true; }
    if (strcmp(cmd, "imu debug off") == 0) { imuDebugEnabled_ = false; Serial.println(F("IMU Debug=off")); return true; }

    // Test runner commands
    if (strncmp(cmd, "test ", 5) == 0 || strncmp(cmd, "gen ", 4) == 0) {
        if (testRunner_) {
            testRunner_->handleCommand(cmd);
        } else {
            Serial.println(F("Test runner not available"));
        }
        return true;
    }

    return false;
}

void SerialConsole::printHelp() {
    Serial.println(F("=== SERIAL CONSOLE HELP ==="));
    Serial.println();
    Serial.println(F("SETTINGS:"));
    Serial.println(F("  set <name> <value>  - Set a parameter"));
    Serial.println(F("  get <name>          - Get current value"));
    Serial.println(F("  show                - Show all settings"));
    Serial.println(F("  show <category>     - Show category (fire/audio/agc/debug/viz)"));
    Serial.println(F("  settings            - Show all with descriptions"));
    Serial.println(F("  categories          - List available categories"));
    Serial.println();
    Serial.println(F("GENERAL:"));
    Serial.println(F("  help                - Show this help"));
    Serial.println(F("  version             - Show version info"));
    Serial.println(F("  defaults            - Restore default values"));
    Serial.println();
    Serial.println(F("CONFIGURATION:"));
    Serial.println(F("  save                - Save to flash"));
    Serial.println(F("  load                - Load from flash"));
    Serial.println(F("  reset               - Factory reset"));
    Serial.println();
    Serial.println(F("STATS:"));
    Serial.println(F("  mic stats           - Audio statistics"));
    Serial.println(F("  fire stats          - Fire engine stats"));
    Serial.println(F("  battery stats       - Battery status"));
    Serial.println(F("  imu stats           - IMU orientation"));
    Serial.println();
    Serial.println(F("SHORTCUTS:"));
    Serial.println(F("  ag on/off           - Toggle auto-gain"));
    Serial.println(F("  ag stats            - Auto-gain status"));
    Serial.println(F("  mic debug on/off    - Mic debug stream"));
    Serial.println(F("  debug on/off        - General debug stream"));
    Serial.println();
    Serial.println(F("VISUALIZATION:"));
    Serial.println(F("  imu viz on/off      - IMU orientation display"));
    Serial.println(F("  battery viz on/off  - Battery level display"));
    Serial.println(F("  test pattern on/off - Test pattern"));
    Serial.println(F("  fire disable/enable - Toggle fire effect"));
}

void SerialConsole::printVersionInfo() {
    Serial.println(F("=== VERSION INFO ==="));
    Serial.println(F(BLINKY_FULL_VERSION));
    Serial.print(F("Device: ")); Serial.println(config.deviceName);
    Serial.print(F("Matrix: ")); Serial.print(config.matrix.width);
    Serial.print(F("x")); Serial.println(config.matrix.height);
    Serial.print(F("LEDs: ")); Serial.println(config.matrix.width * config.matrix.height);
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

    Serial.print(F("Defaults restored for: "));
    Serial.println(config.deviceName);
}

void SerialConsole::printMicStats() {
    if (!mic_) {
        Serial.println(F("Mic not available"));
        return;
    }
    Serial.println(F("=== MIC STATS ==="));
    Serial.print(F("Level: ")); Serial.println(mic_->getLevel(), 3);
    Serial.print(F("Transient: ")); Serial.println(mic_->getTransient(), 3);
    Serial.print(F("Envelope: ")); Serial.println(mic_->getEnv(), 3);
    Serial.print(F("EnvMean: ")); Serial.println(mic_->getEnvMean(), 3);
    Serial.print(F("PreGate: ")); Serial.println(mic_->getLevelPreGate(), 3);
    Serial.print(F("PostAGC: ")); Serial.println(mic_->getLevelPostAGC(), 3);
    Serial.print(F("GlobalGain: ")); Serial.println(mic_->getGlobalGain(), 3);
    Serial.print(F("HW Gain: ")); Serial.println(mic_->getHwGain());
    Serial.print(F("ISR Count: ")); Serial.println(mic_->getIsrCount());
}

void SerialConsole::printFireStats() {
    Serial.println(F("=== FIRE STATS ==="));
    if (!fireGenerator_) {
        Serial.println(F("Fire generator not available"));
        return;
    }
    const FireParams& p = fireGenerator_->getParams();
    Serial.print(F("Cooling: ")); Serial.println(p.baseCooling);
    Serial.print(F("SparkChance: ")); Serial.println(p.sparkChance, 3);
    Serial.print(F("SparkHeat: ")); Serial.print(p.sparkHeatMin);
    Serial.print(F("-")); Serial.println(p.sparkHeatMax);
    Serial.print(F("AudioBoost: spark=")); Serial.print(p.audioSparkBoost, 3);
    Serial.print(F(" heat=")); Serial.println(p.audioHeatBoostMax);
    Serial.print(F("BurstSparks: ")); Serial.print(p.burstSparks);
    Serial.print(F(" suppression=")); Serial.print(p.suppressionMs);
    Serial.println(F("ms"));
    if (mic_) {
        Serial.print(F("Current: energy=")); Serial.print(mic_->getLevel(), 3);
        Serial.print(F(" hit=")); Serial.println(mic_->getTransient(), 3);
    }
}

void SerialConsole::printBatteryStats() {
    Serial.println(F("=== BATTERY STATS ==="));
    Serial.print(F("Voltage: ")); Serial.print(battery.getVoltage(), 2); Serial.println(F("V"));
    Serial.print(F("Raw: ")); Serial.println(battery.readRaw());
    Serial.print(F("Percent: ")); Serial.print(battery.getPercent()); Serial.println(F("%"));
    Serial.print(F("Charging: ")); Serial.println(battery.isCharging() ? F("YES") : F("NO"));
    Serial.print(F("Config: ")); Serial.print(config.charging.minVoltage, 1);
    Serial.print(F("V - ")); Serial.print(config.charging.maxVoltage, 1); Serial.println(F("V"));
}

void SerialConsole::printRawIMUData() {
    if (!imu.isReady()) {
        Serial.println(F("IMU not ready"));
        return;
    }
    if (!imu.updateIMUData()) {
        Serial.println(F("Failed to read IMU"));
        return;
    }
    const IMUData& data = imu.getRawIMUData();
    Serial.println(F("=== IMU DATA ==="));
    Serial.print(F("Accel: (")); Serial.print(data.accel.x, 3);
    Serial.print(F(",")); Serial.print(data.accel.y, 3);
    Serial.print(F(",")); Serial.print(data.accel.z, 3); Serial.println(F(")"));
    Serial.print(F("Up: (")); Serial.print(data.up.x, 3);
    Serial.print(F(",")); Serial.print(data.up.y, 3);
    Serial.print(F(",")); Serial.print(data.up.z, 3); Serial.println(F(")"));
    Serial.print(F("Tilt: ")); Serial.print(data.tiltAngle, 1); Serial.println(F("°"));
}

// === DEBUG TICK FUNCTIONS ===

void SerialConsole::micDebugTick() {
    if (!micDebugEnabled_) return;
    uint32_t now = millis();
    if (now - micDebugLastMs_ >= micDebugPeriodMs_) {
        micDebugLastMs_ = now;
        micDebugPrintLine();
    }
}

void SerialConsole::micDebugPrintLine() {
    if (!mic_) return;
    Serial.print(F("MIC: lvl=")); Serial.print(mic_->getLevel(), 3);
    Serial.print(F(" trans=")); Serial.print(mic_->getTransient(), 3);
    Serial.print(F(" env=")); Serial.print(mic_->getEnv(), 3);
    Serial.print(F(" gain=")); Serial.print(mic_->getGlobalGain(), 3);
    Serial.println();
}

void SerialConsole::debugTick() {
    if (!debugEnabled_) return;
    uint32_t now = millis();
    if (now - debugLastMs_ >= debugPeriodMs_) {
        debugLastMs_ = now;
        if (mic_ && fireGenerator_) {
            Serial.print(F("DBG: lvl=")); Serial.print(mic_->getLevel(), 2);
            Serial.print(F(" hit=")); Serial.print(mic_->getTransient(), 2);
            Serial.print(F(" cool=")); Serial.print(fireGenerator_->getParams().baseCooling);
            Serial.print(F(" spark=")); Serial.print(fireGenerator_->getParams().sparkChance, 2);
            Serial.println();
        }
    }
}

void SerialConsole::imuDebugTick() {
    if (!imuDebugEnabled_) return;
    uint32_t now = millis();
    if (now - imuDebugLastMs_ >= imuDebugPeriodMs_) {
        imuDebugLastMs_ = now;
        printRawIMUData();
    }
}

// === VISUALIZATION RENDERING ===

void SerialConsole::renderIMUVisualization() {
    if (!imuVizEnabled) return;
    if (!imu.isReady() || !imu.updateIMUData()) return;

    for (int i = 0; i < ledMapper.getTotalPixels(); i++) {
        leds_.setPixelColor(i, 0);
    }

    const IMUData& data = imu.getRawIMUData();
    const int WIDTH = ledMapper.getWidth();
    const int HEIGHT = ledMapper.getHeight();

    auto setPixel = [&](int x, int y, uint32_t color) {
        int index = ledMapper.getIndex(x, y);
        if (index >= 0) leds_.setPixelColor(index, color);
    };

    // Corner references
    setPixel(0, 0, leds_.Color(16, 16, 16));
    setPixel(WIDTH-1, 0, leds_.Color(16, 16, 16));
    setPixel(0, HEIGHT-1, leds_.Color(16, 16, 16));
    setPixel(WIDTH-1, HEIGHT-1, leds_.Color(16, 16, 16));

    // Up direction
    int upX = (int)((data.up.x + 1.0f) * (WIDTH - 1) / 2.0f);
    int upY = (int)((data.up.y + 1.0f) * (HEIGHT - 1) / 2.0f);
    upX = constrain(upX, 0, WIDTH-1);
    upY = constrain(upY, 0, HEIGHT-1);
    setPixel(upX, upY, leds_.Color(255, 255, 255));

    leds_.show();
}

void SerialConsole::renderTopVisualization() {
    if (!heatVizEnabled) return;
    if (!imu.isReady() || !imu.updateIMUData()) return;

    const int WIDTH = ledMapper.getWidth();
    const int HEIGHT = ledMapper.getHeight();
    const IMUData& data = imu.getRawIMUData();

    float circumfMag = sqrt(data.up.y * data.up.y + data.up.z * data.up.z);

    if (circumfMag < 0.3f) {
        for (int x = 0; x < WIDTH; x++) {
            leds_.setPixelColor(x, leds_.Color(100, 0, 0));
        }
    } else {
        float angle = atan2(data.up.z, data.up.y) + PI / 2.0f;
        float normAngle = (angle + PI) / (2.0f * PI);
        int topCol = (int)(normAngle * WIDTH + 0.5f) % WIDTH;
        for (int y = 0; y < HEIGHT; y++) {
            int index = y * WIDTH + topCol;
            leds_.setPixelColor(index, leds_.Color(255, 0, 0));
        }
    }
    leds_.show();
}

void SerialConsole::renderBatteryVisualization() {
    if (!batteryVizEnabled) return;

    int width = config.matrix.width;
    int height = config.matrix.height;

    for (int i = 0; i < leds_.numPixels(); i++) {
        leds_.setPixelColor(i, 0);
    }

    float voltage = battery.getVoltage();
    if (voltage <= 0) {
        for (int x = 0; x < width; x++) {
            int idx = xyToPixelIndex(x, height - 1);
            leds_.setPixelColor(idx, leds_.Color(50, 0, 0));
        }
        leds_.show();
        return;
    }

    float chargeLevel = constrain(
        (voltage - config.charging.minVoltage) /
        (config.charging.maxVoltage - config.charging.minVoltage),
        0.0f, 1.0f);
    int numPixels = (int)(chargeLevel * width);
    bool charging = battery.isCharging();

    for (int x = 0; x < width; x++) {
        int idx = xyToPixelIndex(x, height - 1);
        if (x < numPixels) {
            uint32_t color;
            if (charging) color = leds_.Color(0, 50, 255);
            else if (chargeLevel > 0.6f) color = leds_.Color(0, 255, 0);
            else if (chargeLevel > 0.3f) color = leds_.Color(255, 255, 0);
            else color = leds_.Color(255, 0, 0);
            leds_.setPixelColor(idx, color);
        } else {
            leds_.setPixelColor(idx, leds_.Color(5, 5, 5));
        }
    }
    leds_.show();
}

void SerialConsole::renderTestPattern() {
    if (!testPatternEnabled) return;

    int width = config.matrix.width;
    int height = config.matrix.height;

    for (int i = 0; i < leds_.numPixels(); i++) {
        leds_.setPixelColor(i, 0);
    }

    static uint32_t lastUpdate = 0;
    static int offset = 0;
    if (millis() - lastUpdate > 500) {
        lastUpdate = millis();
        offset = (offset + 1) % (height + 3);
    }

    for (int y = 0; y < height; y++) {
        uint32_t color;
        switch ((y + offset) % 3) {
            case 0: color = leds_.Color(255, 0, 0); break;
            case 1: color = leds_.Color(0, 255, 0); break;
            default: color = leds_.Color(0, 0, 255); break;
        }
        for (int x = 0; x < width; x++) {
            int idx = xyToPixelIndex(x, y);
            if (idx < leds_.numPixels()) {
                leds_.setPixelColor(idx, color);
            }
        }
    }
    leds_.show();
}

int SerialConsole::xyToPixelIndex(int x, int y) {
    int width = config.matrix.width;
    int height = config.matrix.height;
    x = (x % width + width) % width;
    y = (y % height + height) % height;

    if (config.matrix.orientation == VERTICAL && width == 4 && height == 15) {
        if (x % 2 == 0) return x * height + y;
        else return x * height + (height - 1 - y);
    }
    return y * width + x;
}
