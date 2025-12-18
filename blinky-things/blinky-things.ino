/**
 * Blinky Time - LED Fire Effect Controller
 *
 * A sophisticated fire effect system for wearable LED installations.
 * Supports multiple device configurations with realistic fire simulation,
 * audio reactivity, battery management, and motion sensing.
 *
 * Hardware: nRF52840 XIAO Sense with WS2812B LED strips
 * Author: Blinky Time Project Contributors
 * License: Creative Commons Attribution-ShareAlike 4.0 International
 * Repository: https://github.com/Jdubz/blinky_time
 *
 * Device Types:
 * - Hat: 89 LEDs in string configuration
 * - Tube Light: 60 LEDs in 4x15 zigzag matrix
 * - Bucket Totem: 128 LEDs in 16x8 matrix
 */

// CRITICAL: Adafruit_NeoPixel must be included FIRST to avoid pinDefinitions.h
// redefinition conflicts between PDM library and NeoPixel library
#include <Adafruit_NeoPixel.h>
#include "BlinkyArchitecture.h"     // Includes all architecture components and config
#include "BlinkyImplementations.h"  // Includes all .cpp implementations for Arduino IDE
#include "types/Version.h"           // Version information from repository

// Device Configuration Selection
// Define DEVICE_TYPE to select active configuration:
// 1 = Hat (89 LEDs, STRING_FIRE mode)
// 2 = Tube Light (4x15 matrix, MATRIX_FIRE mode)
// 3 = Bucket Totem (16x8 matrix, MATRIX_FIRE mode)
#ifndef DEVICE_TYPE
#define DEVICE_TYPE 1  // Hat (89 LEDs, STRING_FIRE mode)
#endif

#if DEVICE_TYPE == 1
#include "devices/HatConfig.h"
const DeviceConfig& config = HAT_CONFIG;
#elif DEVICE_TYPE == 2
#include "devices/TubeLightConfig.h"
const DeviceConfig& config = TUBE_LIGHT_CONFIG;
#elif DEVICE_TYPE == 3
#include "devices/BucketTotemConfig.h"
const DeviceConfig& config = BUCKET_TOTEM_CONFIG;
#else
#error "Invalid DEVICE_TYPE. Use 1=Hat, 2=TubeLight, 3=BucketTotem"
#endif
LEDMapper ledMapper;

Adafruit_NeoPixel leds(config.matrix.width * config.matrix.height, config.matrix.ledPin, config.matrix.ledType);

// New Generator-Effect-Render Architecture
// === ARCHITECTURE STATUS ===
// ✅ Core System: Inputs→Generator→Effect(optional)→Render pipeline operational
// ✅ Fire: Realistic fire simulation (red/orange/yellow) for all layout types
// ✅ Water: Flowing water effects (blue/cyan) for all layout types
// ✅ Lightning: Electric bolt effects (yellow/white) for all layout types
// ✅ Effects: HueRotation for color cycling, NoOp for pass-through
// ✅ Hardware: AdaptiveMic ready for audio input
// ✅ Compilation: Ready for all device types (Hat, Tube Light, Bucket Totem)
Generator* currentGenerator = nullptr;
Effect* currentEffect = nullptr;
EffectRenderer* renderer = nullptr;
PixelMatrix* pixelMatrix = nullptr;

AdaptiveMic mic;
BatteryMonitor battery;
ConfigStorage configStorage;    // Persistent settings storage

uint32_t lastMs = 0;
bool prevChargingState = false;
bool micDebugEnabled = false;
uint32_t lastMicDebug = 0;
uint16_t micDebugInterval = 100;  // ms between debug outputs (default 10Hz)

// Live-tunable fire parameters
FireParams fireParams;

void updateFireParams() {
  Fire* f = static_cast<Fire*>(currentGenerator);
  if (f) {
    f->setParams(fireParams);
  }
}

// Helper function to clear all LEDs
void clearAllLEDs() {
  for (int i = 0; i < ledMapper.getTotalPixels(); i++) {
    leds.setPixelColor(i, Constants::LED_OFF);
  }
}

// Helper functions for new Generator-Effect-Renderer architecture
void updateFireEffect(float energy, float hit) {
  static uint32_t lastDebug = 0;
  if (millis() - lastDebug > 5000) {  // Debug every 5 seconds
    lastDebug = millis();
    Serial.print(F("Fire update - energy: "));
    Serial.print(energy);
    Serial.print(F(", hit: "));
    Serial.println(hit);
  }

  // Update generator with audio energy and impact
  if (currentGenerator) {
    // Cast to Fire to access update method and setAudioInput
    Fire* fireGen = static_cast<Fire*>(currentGenerator);
    if (fireGen) {
      fireGen->setAudioInput(energy, hit);
      fireGen->update();
    }
  }
}

void showFireEffect() {
  // Generate -> Effect -> Render -> Display pipeline
  if (currentGenerator && currentEffect && renderer && pixelMatrix) {
    // Get audio input for generation
    float energy = mic.getLevel();
    float hit = mic.getTransient();

    // Update generator with audio input (handled by updateFireEffect)
    updateFireEffect(energy, hit);

    // Generate effects and render
    currentGenerator->generate(*pixelMatrix, energy, hit);
    currentEffect->apply(pixelMatrix);
    renderer->render(*pixelMatrix);
    leds.show();
  }
}

void renderFireEffect() {
  // In the new architecture, rendering is handled by showFireEffect()
  // This function can be used for additional processing if needed
  showFireEffect();
}

// Simple serial command handler
void handleSerialCommands() {
  if (Serial.available()) {
    static char buf[64];
    size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
    buf[len] = '\0';
    // Trim CR/LF
    while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) {
      buf[--len] = '\0';
    }

    if (strcmp(buf, "help") == 0) {
      Serial.println(F("=== COMMANDS ==="));
      Serial.println(F("show         - Show all settings"));
      Serial.println(F("save         - Save settings to flash"));
      Serial.println(F("mic fast     - Fast mic stream (30Hz)"));
      Serial.println(F("mic slow     - Slow mic stream (5Hz)"));
      Serial.println(F("mic off      - Stop mic stream"));
      Serial.println(F("fire stats   - Fire parameters"));
      Serial.println(F(""));
      Serial.println(F("=== FIRE TUNING ==="));
      Serial.println(F("set cooling <0-255>"));
      Serial.println(F("set sparkchance <0.0-1.0>"));
      Serial.println(F("set sparkmin <0-255>"));
      Serial.println(F("set sparkmax <0-255>"));
      Serial.println(F("set audioboost <0.0-2.0>"));
      Serial.println(F("set heatboost <0-255>"));
      Serial.println(F("set coolbias <-128 to 0>"));
      Serial.println(F("set spread <1-20>"));
      Serial.println(F("set decay <0.0-1.0>"));
      Serial.println(F("set brightness <0-255>"));
      Serial.println(F(""));
      Serial.println(F("=== MIC/AGC TUNING ==="));
      Serial.println(F("set agstrength <0.01-1.0>  - AGC speed (lower=slower)"));
      Serial.println(F("set agtarget <0.1-0.9>    - Target level"));
      Serial.println(F("set gate <0.0-0.2>        - Noise gate"));
      Serial.println(F(""));
      Serial.println(F("=== TRANSIENT TUNING ==="));
      Serial.println(F("set tfactor <1.1-5.0>     - Hit threshold (higher=less sensitive)"));
      Serial.println(F("set tcooldown <50-500>    - Min ms between hits"));
      Serial.println(F("set loudfloor <0.05-0.5>  - Min level for hit"));
      Serial.println(F("set burst <1-30>          - Sparks on burst (punch)"));
      Serial.println(F("set suppress <50-1000>    - Suppression time after burst (ms)"));
    }
    else if (strcmp(buf, "show") == 0 || strcmp(buf, "mic stats") == 0) {
      Serial.println(F("=== MIC STATUS ==="));
      Serial.print(F("Level: ")); Serial.println(mic.getLevel(), 3);
      Serial.print(F("Transient: ")); Serial.println(mic.getTransient(), 3);
      Serial.print(F("Envelope: ")); Serial.println(mic.getEnv(), 3);
      Serial.print(F("EnvMean: ")); Serial.println(mic.getEnvMean(), 3);
      Serial.print(F("PDM Alive: ")); Serial.println(mic.pdmAlive ? F("YES") : F("NO"));
      Serial.println(F("=== AGC ==="));
      Serial.print(F("GlobalGain: ")); Serial.println(mic.getGlobalGain(), 3);
      Serial.print(F("HW Gain: ")); Serial.println(mic.getHwGain());
      Serial.print(F("agStrength: ")); Serial.println(mic.agStrength, 3);
      Serial.print(F("agTarget: ")); Serial.println(mic.agTarget, 2);
      Serial.print(F("noiseGate: ")); Serial.println(mic.noiseGate, 3);
    }
    else if (strcmp(buf, "mic debug") == 0) {
      micDebugEnabled = !micDebugEnabled;
      Serial.print(F("Mic debug: ")); Serial.print(micDebugEnabled ? F("ON") : F("OFF"));
      Serial.print(F(" (")); Serial.print(1000/micDebugInterval); Serial.println(F("Hz)"));
    }
    else if (strcmp(buf, "mic fast") == 0) {
      micDebugEnabled = true;
      micDebugInterval = 33;  // ~30Hz
      Serial.println(F("Mic debug: FAST (30Hz)"));
    }
    else if (strcmp(buf, "mic slow") == 0) {
      micDebugEnabled = true;
      micDebugInterval = 200;  // 5Hz
      Serial.println(F("Mic debug: SLOW (5Hz)"));
    }
    else if (strcmp(buf, "mic off") == 0) {
      micDebugEnabled = false;
      Serial.println(F("Mic debug: OFF"));
    }
    else if (strcmp(buf, "fire stats") == 0) {
      Serial.println(F("=== FIRE STATUS ==="));
      Fire* f = static_cast<Fire*>(currentGenerator);
      if (f) {
        Serial.print(F("Layout: "));
        Serial.println(config.matrix.layoutType == LINEAR_LAYOUT ? F("LINEAR") : F("MATRIX"));
        Serial.print(F("LEDs: ")); Serial.println(config.matrix.width * config.matrix.height);
        Serial.print(F("Brightness: ")); Serial.println(leds.getBrightness());
      }
      Serial.println(F("Fire Params (live):"));
      Serial.print(F("  cooling: ")); Serial.println(fireParams.baseCooling);
      Serial.print(F("  sparkChance: ")); Serial.println(fireParams.sparkChance, 3);
      Serial.print(F("  sparkMin: ")); Serial.println(fireParams.sparkHeatMin);
      Serial.print(F("  sparkMax: ")); Serial.println(fireParams.sparkHeatMax);
      Serial.print(F("  audioBoost: ")); Serial.println(fireParams.audioSparkBoost, 3);
      Serial.print(F("  heatBoost: ")); Serial.println(fireParams.audioHeatBoostMax);
      Serial.print(F("  coolBias: ")); Serial.println(fireParams.coolingAudioBias);
      Serial.print(F("  spread: ")); Serial.println(fireParams.spreadDistance);
      Serial.print(F("  decay: ")); Serial.println(fireParams.heatDecay, 2);
      Serial.print(F("  burst: ")); Serial.println(fireParams.burstSparks);
      Serial.print(F("  suppress: ")); Serial.println(fireParams.suppressionMs);
    }
    // Fire parameter tuning commands
    else if (strncmp(buf, "set cooling ", 12) == 0) {
      int val = atoi(buf + 12);
      fireParams.baseCooling = constrain(val, 0, 255);
      updateFireParams();
      configStorage.markDirty();
      Serial.print(F("cooling=")); Serial.println(fireParams.baseCooling);
    }
    else if (strncmp(buf, "set sparkchance ", 16) == 0) {
      float val = atof(buf + 16);
      fireParams.sparkChance = constrain(val, 0.0f, 1.0f);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("sparkChance=")); Serial.println(fireParams.sparkChance, 3);
    }
    else if (strncmp(buf, "set sparkmin ", 13) == 0) {
      int val = atoi(buf + 13);
      fireParams.sparkHeatMin = constrain(val, 0, 255);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("sparkMin=")); Serial.println(fireParams.sparkHeatMin);
    }
    else if (strncmp(buf, "set sparkmax ", 13) == 0) {
      int val = atoi(buf + 13);
      fireParams.sparkHeatMax = constrain(val, 0, 255);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("sparkMax=")); Serial.println(fireParams.sparkHeatMax);
    }
    else if (strncmp(buf, "set audioboost ", 15) == 0) {
      float val = atof(buf + 15);
      fireParams.audioSparkBoost = constrain(val, 0.0f, 2.0f);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("audioBoost=")); Serial.println(fireParams.audioSparkBoost, 3);
    }
    else if (strncmp(buf, "set heatboost ", 14) == 0) {
      int val = atoi(buf + 14);
      fireParams.audioHeatBoostMax = constrain(val, 0, 255);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("heatBoost=")); Serial.println(fireParams.audioHeatBoostMax);
    }
    else if (strncmp(buf, "set coolbias ", 13) == 0) {
      int val = atoi(buf + 13);
      fireParams.coolingAudioBias = constrain(val, -128, 0);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("coolBias=")); Serial.println(fireParams.coolingAudioBias);
    }
    else if (strncmp(buf, "set spread ", 11) == 0) {
      int val = atoi(buf + 11);
      fireParams.spreadDistance = constrain(val, 1, 20);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("spread=")); Serial.println(fireParams.spreadDistance);
    }
    else if (strncmp(buf, "set decay ", 10) == 0) {
      float val = atof(buf + 10);
      fireParams.heatDecay = constrain(val, 0.0f, 1.0f);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("decay=")); Serial.println(fireParams.heatDecay, 2);
    }
    else if (strncmp(buf, "set brightness ", 15) == 0) {
      int val = atoi(buf + 15);
      leds.setBrightness(constrain(val, 0, 255));
      configStorage.markDirty();
      Serial.print(F("brightness=")); Serial.println(leds.getBrightness());
    }
    // AGC tuning commands
    else if (strncmp(buf, "set agstrength ", 15) == 0) {
      float val = atof(buf + 15);
      mic.agStrength = constrain(val, 0.01f, 1.0f);
      configStorage.markDirty();
      Serial.print(F("agStrength=")); Serial.println(mic.agStrength, 3);
    }
    else if (strncmp(buf, "set agtarget ", 13) == 0) {
      float val = atof(buf + 13);
      mic.agTarget = constrain(val, 0.1f, 0.9f);
      configStorage.markDirty();
      Serial.print(F("agTarget=")); Serial.println(mic.agTarget, 2);
    }
    else if (strncmp(buf, "set gate ", 9) == 0) {
      float val = atof(buf + 9);
      mic.noiseGate = constrain(val, 0.0f, 0.2f);
      configStorage.markDirty();
      Serial.print(F("noiseGate=")); Serial.println(mic.noiseGate, 3);
    }
    else if (strncmp(buf, "set gain ", 9) == 0) {
      float val = atof(buf + 9);
      mic.globalGain = constrain(val, 0.5f, 15.0f);
      configStorage.markDirty();
      Serial.print(F("globalGain=")); Serial.println(mic.globalGain, 2);
    }
    else if (strncmp(buf, "set agmin ", 10) == 0) {
      float val = atof(buf + 10);
      mic.agMin = constrain(val, 0.1f, 5.0f);
      configStorage.markDirty();
      Serial.print(F("agMin=")); Serial.println(mic.agMin, 2);
    }
    else if (strncmp(buf, "set agmax ", 10) == 0) {
      float val = atof(buf + 10);
      mic.agMax = constrain(val, 1.0f, 20.0f);
      configStorage.markDirty();
      Serial.print(F("agMax=")); Serial.println(mic.agMax, 2);
    }
    // Transient detection tuning
    else if (strncmp(buf, "set tfactor ", 12) == 0) {
      float val = atof(buf + 12);
      mic.transientFactor = constrain(val, 1.1f, 5.0f);
      configStorage.markDirty();
      Serial.print(F("transientFactor=")); Serial.println(mic.transientFactor, 2);
    }
    else if (strncmp(buf, "set tcooldown ", 14) == 0) {
      int val = atoi(buf + 14);
      mic.transientCooldownMs = constrain(val, 50, 500);
      configStorage.markDirty();
      Serial.print(F("transientCooldownMs=")); Serial.println(mic.transientCooldownMs);
    }
    else if (strncmp(buf, "set loudfloor ", 14) == 0) {
      float val = atof(buf + 14);
      mic.loudFloor = constrain(val, 0.05f, 0.5f);
      configStorage.markDirty();
      Serial.print(F("loudFloor=")); Serial.println(mic.loudFloor, 2);
    }
    // Hit spark tuning
    else if (strncmp(buf, "set hitbase ", 12) == 0) {
      int val = atoi(buf + 12);
      fireParams.hitSparkBase = constrain(val, 0, 10);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("hitSparkBase=")); Serial.println(fireParams.hitSparkBase);
    }
    else if (strncmp(buf, "set hitmult ", 12) == 0) {
      int val = atoi(buf + 12);
      fireParams.hitSparkMult = constrain(val, 0, 20);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("hitSparkMult=")); Serial.println(fireParams.hitSparkMult);
    }
    else if (strncmp(buf, "set burst ", 10) == 0) {
      int val = atoi(buf + 10);
      fireParams.burstSparks = constrain(val, 1, 30);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("burstSparks=")); Serial.println(fireParams.burstSparks);
    }
    else if (strncmp(buf, "set suppress ", 13) == 0) {
      int val = atoi(buf + 13);
      fireParams.suppressionMs = constrain(val, 50, 1000);
      updateFireParams(); configStorage.markDirty();
      Serial.print(F("suppressionMs=")); Serial.println(fireParams.suppressionMs);
    }
    else if (strcmp(buf, "save") == 0) {
      Serial.println(F("Saving settings to flash..."));
      configStorage.saveConfiguration(fireParams, mic);
    }
    else if (strcmp(buf, "reset") == 0) {
      Serial.println(F("Factory reset..."));
      configStorage.factoryReset();
      configStorage.loadConfiguration(fireParams, mic);
      updateFireParams();
      Serial.println(F("Settings reset to defaults"));
    }
    else if (len > 0) {
      Serial.println(F("Unknown command. Type 'help'"));
    }
  }

  // Periodic mic debug output with activity monitor
  if (micDebugEnabled && millis() - lastMicDebug > micDebugInterval) {
    lastMicDebug = millis();

    // Calculate total LED activity (sum of brightness)
    uint32_t totalBrightness = 0;
    for (int i = 0; i < leds.numPixels(); i++) {
      uint32_t c = leds.getPixelColor(i);
      totalBrightness += ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
    }
    uint8_t activityPct = (uint8_t)(totalBrightness * 100 / (leds.numPixels() * 765));  // 765 = 255*3

    // Calculate expected spark count using same formula as Fire::generateSparks
    float energy = mic.getLevel();
    float hit = mic.getTransient();
    int sparks = 1;
    if (energy > 0.15f) sparks = 1 + (int)((energy - 0.15f) * 6);
    if (hit > 0.1f) sparks += fireParams.hitSparkBase + (int)(hit * fireParams.hitSparkMult);

    // Transient detection threshold (same calc as AdaptiveMic)
    float thresh = max(mic.loudFloor, mic.slowAvg * mic.transientFactor);

    Serial.print(F("lvl="));  Serial.print(energy, 2);
    Serial.print(F(" avg=")); Serial.print(mic.slowAvg, 2);
    Serial.print(F(" thr=")); Serial.print(thresh, 2);
    Serial.print(F(" hit=")); Serial.print(hit, 2);
    Serial.print(F(" spk=")); Serial.print(sparks);
    Serial.print(F(" act=")); Serial.print(activityPct);
    Serial.print(F("%"));
    Serial.println();
  }
}

void setup() {
  Serial.begin(config.serial.baudRate);
  while (!Serial && millis() < config.serial.initTimeoutMs) {}

  // Display version and device information
  Serial.println(F("=== BLINKY TIME STARTUP ==="));
  Serial.println(F(BLINKY_FULL_VERSION));
  Serial.print(F("Build: ")); Serial.print(F(BLINKY_BUILD_DATE));
  Serial.print(F(" ")); Serial.println(F(BLINKY_BUILD_TIME));
  Serial.println();

  // Display active device configuration
  Serial.print(F("Starting device: "));
  Serial.println(config.deviceName);
  Serial.print(F("Device Type: "));
#if DEVICE_TYPE == 1
  Serial.println(F("Hat (Type 1)"));
#elif DEVICE_TYPE == 2
  Serial.println(F("Tube Light (Type 2)"));
#elif DEVICE_TYPE == 3
  Serial.println(F("Bucket Totem (Type 3)"));
#endif

  // Validate critical configuration parameters
  if (config.matrix.width <= 0 || config.matrix.height <= 0) {
    Serial.println(F("ERROR: Invalid matrix dimensions"));
    while(1); // Halt execution
  }
  if (config.matrix.brightness > 255) {
    Serial.println(F("WARNING: Brightness clamped to 255"));
  }

  leds.begin();
  leds.setBrightness(min(config.matrix.brightness, 255));
  leds.show();

  // Basic LED test - light up first few LEDs to verify hardware
  Serial.print(F("LED Test: Lighting first 3 LEDs at brightness "));
  Serial.println(config.matrix.brightness);
  leds.setPixelColor(0, leds.Color(255, 0, 0));  // Should show RED
  leds.setPixelColor(1, leds.Color(0, 255, 0));  // Should show GREEN
  leds.setPixelColor(2, leds.Color(0, 0, 255));  // Should show BLUE
  leds.show();
  delay(3000);  // Hold for 3 seconds to verify colors are correct

  // Clear test LEDs
  leds.setPixelColor(0, 0);
  leds.setPixelColor(1, 0);
  leds.setPixelColor(2, 0);
  leds.show();

  if (!ledMapper.begin(config)) {
    Serial.println(F("ERROR: LED mapper initialization failed"));
    while(1); // Halt execution
  }

  // Initialize new Generator-Effect-Renderer architecture
  Serial.print(F("Config fire type: "));
  Serial.println(config.matrix.fireType == STRING_FIRE ? F("STRING_FIRE") : F("MATRIX_FIRE"));
  Serial.print(F("Matrix dimensions: "));
  Serial.print(config.matrix.width);
  Serial.print(F(" x "));
  Serial.print(config.matrix.height);
  Serial.print(F(" = "));
  Serial.print(config.matrix.width * config.matrix.height);
  Serial.println(F(" LEDs"));

  // Create PixelMatrix for the visual pipeline
  pixelMatrix = new PixelMatrix(config.matrix.width, config.matrix.height);
  if (!pixelMatrix) {
    Serial.println(F("ERROR: PixelMatrix allocation failed"));
    while(1); // Halt execution
  }

  // Initialize appropriate generator based on layout type
  Serial.print(F("Initializing fire generator for layout type: "));
  switch (config.matrix.layoutType) {
    case MATRIX_LAYOUT:
      Serial.println(F("MATRIX"));
      break;
    case LINEAR_LAYOUT:
      Serial.println(F("LINEAR"));
      break;
    case RANDOM_LAYOUT:
      Serial.println(F("RANDOM"));
      break;
    default:
      Serial.println(F("UNKNOWN"));
      break;
  }

  // Create fire generator instance
  Fire* fireGen = new Fire();
  currentGenerator = fireGen;

  if (!currentGenerator) {
    Serial.println(F("ERROR: Generator allocation failed"));
    while(1); // Halt execution
  }

  // Initialize the generator with device configuration
  if (!fireGen->begin(config)) {
    Serial.println(F("ERROR: Generator initialization failed"));
    while(1); // Halt execution
  }

  // Initialize live-tunable fire params from config
  fireParams.baseCooling = config.fireDefaults.baseCooling;
  fireParams.sparkHeatMin = config.fireDefaults.sparkHeatMin;
  fireParams.sparkHeatMax = config.fireDefaults.sparkHeatMax;
  fireParams.sparkChance = config.fireDefaults.sparkChance;
  fireParams.audioSparkBoost = config.fireDefaults.audioSparkBoost;
  fireParams.audioHeatBoostMax = config.fireDefaults.audioHeatBoostMax;
  fireParams.coolingAudioBias = config.fireDefaults.coolingAudioBias;
  fireParams.transientHeatMax = config.fireDefaults.transientHeatMax;
  // Layout-specific params set by Fire::begin()
  fireParams.spreadDistance = 3;
  fireParams.heatDecay = 0.60f;

  // Initialize effect (pass-through for pure fire colors)
  currentEffect = new NoOpEffect();
  if (!currentEffect) {
    Serial.println(F("ERROR: Effect allocation failed"));
    while(1); // Halt execution
  }
  currentEffect->begin(config.matrix.width, config.matrix.height);

  // Initialize renderer
  renderer = new EffectRenderer(leds, ledMapper);
  if (!renderer) {
    Serial.println(F("ERROR: Renderer allocation failed"));
    while(1); // Halt execution
  }

  Serial.println(F("New architecture initialized successfully"));

  bool micOk = mic.begin(config.microphone.sampleRate, config.microphone.bufferSize);
  if (!micOk) {
    Serial.println(F("ERROR: Microphone failed to start"));
  } else {
    Serial.println(F("Microphone initialized"));
  }

  // Initialize configuration storage and load saved settings
  configStorage.begin();
  if (configStorage.isValid()) {
    configStorage.loadConfiguration(fireParams, mic);
    updateFireParams();
    Serial.println(F("Loaded saved configuration from flash"));
  } else {
    Serial.println(F("Using default configuration"));
  }

  // Initialize battery monitor
  if (!battery.begin()) {
    Serial.println(F("WARNING: Battery monitor failed to start"));
  } else {
    battery.setFastCharge(config.charging.fastChargeEnabled);
    Serial.println(F("Battery monitor initialized"));
  }

  Serial.println(F("Setup complete!"));
}

void loop() {
  uint32_t now = millis();
  float dt = (lastMs == 0) ? Constants::DEFAULT_FRAME_TIME : (now - lastMs) * 0.001f;
  dt = constrain(dt, Constants::MIN_FRAME_TIME, Constants::MAX_FRAME_TIME); // Clamp dt to reasonable range
  lastMs = now;

  mic.update(dt);

  float energy = mic.getLevel();
  float hit = mic.getTransient();

  // Track charging state changes
  bool currentChargingState = battery.isCharging();
  if (currentChargingState != prevChargingState) {
    if (currentChargingState) {
      Serial.println(F("Charging started"));
    } else {
      Serial.println(F("Charging stopped"));
    }
    prevChargingState = currentChargingState;
  }

  // Simplified rendering for new architecture - just fire effect for now
  showFireEffect();

  // Handle serial commands for debugging
  handleSerialCommands();

  // Auto-save dirty settings to flash (debounced)
  configStorage.saveIfDirty(fireParams, mic);

  // Battery monitoring - periodic voltage check
  static uint32_t lastBatteryCheck = 0;
  if (millis() - lastBatteryCheck > Constants::BATTERY_CHECK_INTERVAL_MS) {
    lastBatteryCheck = millis();
    float voltage = battery.getVoltage();
    if (voltage > 0 && voltage < config.charging.criticalBatteryThreshold) {
      Serial.print(F("CRITICAL BATTERY: "));
      Serial.print(voltage);
      Serial.println(F("V"));
    } else if (voltage > 0 && voltage < config.charging.lowBatteryThreshold) {
      Serial.print(F("Low battery: "));
      Serial.print(voltage);
      Serial.println(F("V"));
    }
  }
}
