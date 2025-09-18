#include "SerialConsole.h"
#include "TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"

extern AdaptiveMic mic;
extern BatteryMonitor battery;
extern IMUHelper imu;

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

    // Periodic debug outputs
    micDebugTick();
    debugTick();
}

void SerialConsole::handleCommand(const char *cmd) {
    int tempInt = 0;
    float tempFloat = 0.0f;
    float tempFloat2 = 0.0f;

    if (strcmp(cmd, "help") == 0) {
        Serial.println(F("=== FIRE TOTEM DEBUG CONSOLE ==="));
        Serial.println(F("General: show, defaults, save, load"));
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
        Serial.println(F("MOTION EFFECTS:"));
        Serial.println(F("  set windspeed <0-10>       - Wind lean speed"));
        Serial.println(F("  set windscale <0-1>        - Wind intensity scale"));
        Serial.println(F("  motion on/off              - Enable/disable motion"));
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
        Serial.println(F("  fire disable/enable        - Disable fire for IMU viz"));
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
    else if (sscanf(cmd, "set transientheatmax %d", &tempInt) == 1) {
        fire.params.transientHeatMax = constrain(tempInt, 0, 255);
        Serial.print(F("TransientHeatMax="));
        Serial.println(fire.params.transientHeatMax);
    }
 
    // --- AdaptiveMic parameters ---
    else if (sscanf(cmd, "set gate %f", &tempFloat) == 1) { mic.noiseGate = tempFloat; Serial.print(F("NoiseGate=")); Serial.println(mic.noiseGate, 3); }
    else if (sscanf(cmd, "set gain %f", &tempFloat) == 1) { mic.globalGain = tempFloat; Serial.print(F("GlobalGain=")); Serial.println(mic.globalGain, 3); }
    else if (sscanf(cmd, "set attack %f", &tempFloat) == 1) { mic.attackSeconds = tempFloat; Serial.print(F("attackSeconds=")); Serial.println(mic.attackSeconds, 3); }
    else if (sscanf(cmd, "set release %f", &tempFloat) == 1) { mic.releaseSeconds = tempFloat; Serial.print(F("releaseSeconds=")); Serial.println(mic.releaseSeconds, 3); }
    else if (sscanf(cmd, "set transientCooldown %d", &tempInt) == 1) {
        mic.transientCooldownMs = constrain(tempInt, 10, 1000); // 10ms–1s
        Serial.print(F("TransientCooldownMs="));
        Serial.println(mic.transientCooldownMs);
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

    // IMU stats
    else if (strcmp(cmd, "imu stats") == 0) {
      const MotionState& m = imu.motion();
      Serial.print(F("IMU up=("));  Serial.print(m.up.x,3);  Serial.print(',');
      Serial.print(m.up.y,3);       Serial.print(',');       Serial.print(m.up.z,3);
      Serial.print(F(")  wind=(")); Serial.print(m.wind.x,3);Serial.print(','); Serial.print(m.wind.y,3);
      Serial.print(F(")  stoke=")); Serial.println(m.stoke,3);
    }
    else if (sscanf(cmd, "imu tau %f", &tempFloat) == 1) { MotionConfig c=imu.getMotionConfig(); c.tauLP=tempFloat; imu.setMotionConfig(c); Serial.print(F("imu.tauLP=")); Serial.println(c.tauLP,3); }
    else if (sscanf(cmd, "imu windaccel %f", &tempFloat) == 1) { MotionConfig c=imu.getMotionConfig(); c.kAccel=tempFloat; imu.setMotionConfig(c); Serial.print(F("imu.kAccel=")); Serial.println(c.kAccel,3); }
    else if (sscanf(cmd, "imu windspin %f", &tempFloat) == 1) { MotionConfig c=imu.getMotionConfig(); c.kSpin=tempFloat; imu.setMotionConfig(c); Serial.print(F("imu.kSpin=")); Serial.println(c.kSpin,3); }
    else if (sscanf(cmd, "imu stoke %f", &tempFloat) == 1) { MotionConfig c=imu.getMotionConfig(); c.kStoke=tempFloat; imu.setMotionConfig(c); Serial.print(F("imu.kStoke=")); Serial.println(c.kStoke,3); }

    // --- New enhanced commands ---
    // LED brightness control
    else if (sscanf(cmd, "set brightness %d", &tempInt) == 1) {
        tempInt = constrain(tempInt, 0, 255);
        leds.setBrightness(tempInt);
        Serial.print(F("LED Brightness=")); Serial.println(tempInt);
    }

    // Motion control
    else if (sscanf(cmd, "set windspeed %f", &tempFloat) == 1) {
        windSpeed = constrain(tempFloat, 0.0f, 10.0f);
        Serial.print(F("WindSpeed=")); Serial.println(windSpeed, 2);
    }
    else if (sscanf(cmd, "set windscale %f", &tempFloat) == 1) {
        windScale = constrain(tempFloat, 0.0f, 1.0f);
        Serial.print(F("WindScale=")); Serial.println(windScale, 3);
    }
    else if (strcmp(cmd, "motion on") == 0) {
        motionEnabled = true; Serial.println(F("Motion=on"));
    }
    else if (strcmp(cmd, "motion off") == 0) {
        motionEnabled = false; Serial.println(F("Motion=off"));
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
        Serial.println(F("IMU Visualization=on (Matrix shows IMU orientation)"));
        Serial.println(F("Blue=Up vector, Green=Wind vector, Red=Stoke level"));
        Serial.println(F("Use 'fire disable' to turn off fire effect for clearer view"));
    }
    else if (strcmp(cmd, "imu viz off") == 0) {
        imuVizEnabled = false;
        Serial.println(F("IMU Visualization=off"));
    }
    else if (strcmp(cmd, "imu test") == 0) {
        Serial.println(F("IMU Test Mode: Tilt device and watch orientation indicators"));
        const MotionState& m = imu.motion();
        Serial.print(F("Current: Up=(")); Serial.print(m.up.x, 2);
        Serial.print(F(","));            Serial.print(m.up.y, 2);
        Serial.print(F(","));            Serial.print(m.up.z, 2);
        Serial.print(F(") Wind=(")); Serial.print(m.wind.x, 2);
        Serial.print(F(","));        Serial.print(m.wind.y, 2);
        Serial.print(F(") Stoke=")); Serial.println(m.stoke, 2);
    }
    else if (strcmp(cmd, "fire disable") == 0) {
        fireDisabled = true;
        Serial.println(F("Fire effect disabled (IMU viz clearer)"));
    }
    else if (strcmp(cmd, "fire enable") == 0) {
        fireDisabled = false;
        Serial.println(F("Fire effect enabled"));
    }

    // Save/load presets (placeholder for future)
    else if (strcmp(cmd, "save") == 0) {
        Serial.println(F("Save preset feature not yet implemented"));
    }
    else if (strcmp(cmd, "load") == 0) {
        Serial.println(F("Load preset feature not yet implemented"));
    }

    else {
        Serial.println(F("Unknown command. Type 'help' for commands."));
    }
}

void SerialConsole::restoreDefaults() {
    fire.restoreDefaults();
    mic.noiseGate   = Defaults::NoiseGate;
    mic.globalGain  = Defaults::GlobalGain;
    mic.attackSeconds   = Defaults::AttackSeconds;
    mic.releaseSeconds  = Defaults::ReleaseSeconds;
    fire.params.transientHeatMax = Defaults::TransientHeatMax;
    mic.transientCooldownMs = Defaults::TransientCooldownMs;

    Serial.println(F("Defaults restored."));
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

    // Motion
    Serial.println(F("MOTION:"));
    Serial.print(F("  Enabled: ")); Serial.println(motionEnabled ? F("YES") : F("NO"));
    Serial.print(F("  WindSpeed: ")); Serial.println(windSpeed, 2);
    Serial.print(F("  WindScale: ")); Serial.println(windScale, 3);

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
    Serial.print(F("  Fire: ")); Serial.println(fireDisabled ? F("DISABLED") : F("ENABLED"));
    Serial.print(F("  Rate: ")); Serial.print(debugPeriodMs); Serial.println(F("ms"));

    // Battery
    float vbat = battery.readVoltage();
    uint8_t soc = BatteryMonitor::voltageToPercent(vbat);
    bool charging = battery.isCharging();
    Serial.println(F("SYSTEM:"));
    Serial.print(F("  Battery: ")); Serial.print(vbat, 2); Serial.print(F("V ("));
    Serial.print(soc); Serial.print(F("%) ")); Serial.println(charging ? F("CHARGING") : F(""));
}

void SerialConsole::renderIMUVisualization() {
    if (!imuVizEnabled) return;

    // Clear the matrix
    for (int i = 0; i < 16 * 8; i++) {
        leds.setPixelColor(i, 0);
    }

    const MotionState& m = imu.motion();

    // Matrix dimensions: 16 wide x 8 tall (cylindrical)
    const int WIDTH = 16;
    const int HEIGHT = 8;
    const int centerX = WIDTH / 2;
    const int centerY = HEIGHT / 2;

    // Helper function to set pixel safely
    auto setPixel = [&](int x, int y, uint32_t color) {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
            // Convert to linear index (assuming row-major, top to bottom)
            int index = y * WIDTH + x;
            leds.setPixelColor(index, color);
        }
    };

    // 1. Show UP VECTOR in BLUE
    // Map up vector to matrix position (assumes up.z is vertical, up.x/y are horizontal)
    int upX = centerX + (int)(m.up.x * (WIDTH/2 - 1));
    int upY = centerY - (int)(m.up.z * (HEIGHT/2 - 1)); // Flip Z for screen coords
    upX = constrain(upX, 0, WIDTH-1);
    upY = constrain(upY, 0, HEIGHT-1);
    setPixel(upX, upY, leds.Color(0, 0, 255)); // Blue for UP

    // 2. Show WIND VECTOR in GREEN
    // Wind shows tilt direction - larger vectors = more tilt
    if (fabsf(m.wind.x) > 0.01f || fabsf(m.wind.y) > 0.01f) {
        int windX = centerX + (int)(m.wind.x * (WIDTH/2 - 1));
        int windY = centerY + (int)(m.wind.y * (HEIGHT/2 - 1));
        windX = constrain(windX, 0, WIDTH-1);
        windY = constrain(windY, 0, HEIGHT-1);
        setPixel(windX, windY, leds.Color(0, 255, 0)); // Green for WIND

        // Draw a line from center to wind position for better visibility
        int dx = windX - centerX;
        int dy = windY - centerY;
        int steps = max(abs(dx), abs(dy));
        if (steps > 1) {
            for (int i = 1; i < steps; i++) {
                int lineX = centerX + (dx * i) / steps;
                int lineY = centerY + (dy * i) / steps;
                setPixel(lineX, lineY, leds.Color(0, 128, 0)); // Dimmer green for line
            }
        }
    }

    // 3. Show STOKE LEVEL as RED intensity at bottom center
    if (m.stoke > 0.01f) {
        int stokeIntensity = (int)(m.stoke * 255);
        int stokeY = HEIGHT - 1; // Bottom row
        // Show stoke as a horizontal bar at bottom
        int stokeWidth = (int)(m.stoke * WIDTH);
        for (int x = 0; x < stokeWidth; x++) {
            setPixel(x, stokeY, leds.Color(stokeIntensity, 0, 0)); // Red for STOKE
        }
    }

    // 4. Show CENTER REFERENCE in WHITE (dim)
    setPixel(centerX, centerY, leds.Color(32, 32, 32)); // Dim white center

    // 5. Show ORIENTATION MARKERS
    // Top center = "UP" direction reference
    setPixel(centerX, 0, leds.Color(64, 64, 64)); // Dim white at top
    // Left/right markers for cylindrical orientation
    setPixel(0, centerY, leds.Color(64, 0, 64));      // Magenta left
    setPixel(WIDTH-1, centerY, leds.Color(0, 64, 64)); // Cyan right

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

        if (motionEnabled) {
            const MotionState& m = imu.motion();
            Serial.print(F(" wind=(")); Serial.print(m.wind.x * windScale, 2);
            Serial.print(F(","));      Serial.print(m.wind.y * windScale, 2);
            Serial.print(F(")"));
        }

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

    // Motion state
    if (motionEnabled) {
        const MotionState& m = imu.motion();
        Serial.print(F("Motion: Wind=(")); Serial.print(m.wind.x * windScale, 3);
        Serial.print(F(","));            Serial.print(m.wind.y * windScale, 3);
        Serial.print(F(")  Up=(")); Serial.print(m.up.x, 2);
        Serial.print(F(","));        Serial.print(m.up.y, 2);
        Serial.print(F(","));        Serial.print(m.up.z, 2);
        Serial.print(F(")  Scale=")); Serial.print(windScale, 3);
        Serial.print(F("  Speed=")); Serial.println(windSpeed, 2);
    } else {
        Serial.println(F("Motion: DISABLED"));
    }

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
