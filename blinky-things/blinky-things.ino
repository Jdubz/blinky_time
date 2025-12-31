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

// NOTE: Adafruit_NeoPixel must be first. PDM.h is included separately in
// Nrf52PdmMic.cpp to avoid pinDefinitions.h redefinition (Seeeduino mbed platform bug)
#include <Adafruit_NeoPixel.h>
#include "BlinkyArchitecture.h"     // Includes all architecture components and config
#include "BlinkyImplementations.h"  // Includes all .cpp implementations for Arduino IDE
#include "types/Version.h"           // Version information from repository
#include "tests/SafeMode.h"          // Crash recovery system
#include "hal/DefaultHal.h"          // HAL singleton instances
#include "hal/hardware/NeoPixelLedStrip.h"  // LED strip wrapper

// Device Configuration Selection
// Define DEVICE_TYPE to select active configuration:
// 1 = Hat (89 LEDs, STRING_FIRE mode)
// 2 = Tube Light (4x15 matrix, MATRIX_FIRE mode)
// 3 = Bucket Totem (16x8 matrix, MATRIX_FIRE mode)
#ifndef DEVICE_TYPE
#define DEVICE_TYPE 3  // Bucket Totem (16x8 matrix)
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

// Hardware abstraction layer for testability
// Use pointers to avoid static initialization order fiasco
// Adafruit_NeoPixel allocates memory in constructor - unsafe at global scope
Adafruit_NeoPixel* neoPixelStrip = nullptr;
NeoPixelLedStrip* leds = nullptr;

// New Generator-Effect-Render Architecture
// === ARCHITECTURE STATUS ===
// ✅ Core System: Inputs→Generator→Effect(optional)→Render pipeline operational
// ✅ Fire: Realistic fire simulation (red/orange/yellow) for all layout types
// ✅ Water: Flowing water effects (blue/cyan) for all layout types
// ✅ Lightning: Electric bolt effects (yellow/white) for all layout types
// ✅ Effects: HueRotation for color cycling, NoOp for pass-through
// ✅ Hardware: AdaptiveMic ready for audio input
// ✅ Compilation: Ready for all device types (Hat, Tube Light, Bucket Totem)
// ✅ RenderPipeline: Unified generator/effect switching via serial console
RenderPipeline* pipeline = nullptr;

// HAL-enabled components - use pointers to avoid static initialization order fiasco
// These are initialized in setup() AFTER Arduino runtime is ready
BatteryMonitor* battery = nullptr;
IMUHelper imu;                     // IMU sensor interface; auto-initializes, uses stub mode if LSM6DS3 not installed
ConfigStorage configStorage;       // Persistent settings storage
SerialConsole* console = nullptr;  // Serial command interface
AudioController* audioCtrl = nullptr;  // Unified audio controller (owns mic, rhythm tracking)

uint32_t lastMs = 0;
bool prevChargingState = false;

// Live-tunable fire parameters (still needed for ConfigStorage compatibility)
FireParams fireParams;

void updateFireParams() {
  if (!pipeline) return;
  Fire* f = pipeline->getFireGenerator();
  if (f) {
    f->setParams(fireParams);
  }
}

// Helper function for new Generator-Effect-Renderer architecture
void renderFrame() {
  // Pipeline handles: Generate -> Effect -> Render
  if (pipeline && pipeline->isValid() && audioCtrl) {
    const AudioControl& audio = audioCtrl->getControl();
    pipeline->render(audio);
    leds->show();
  }
}

/**
 * Cleanup all dynamically allocated resources.
 * Called on allocation failure to prevent memory leaks.
 * Also useful for future sleep/wake or restart scenarios.
 */
void cleanup() {
  delete console;    console = nullptr;
  delete audioCtrl;  audioCtrl = nullptr;
  delete pipeline;   pipeline = nullptr;
  delete battery;    battery = nullptr;
  delete leds;       leds = nullptr;
  delete neoPixelStrip; neoPixelStrip = nullptr;
}

/**
 * Halt execution with error message after cleanup.
 * Use instead of bare while(1) loops to prevent memory leaks.
 */
void haltWithError(const __FlashStringHelper* msg) {
  Serial.println(msg);
  cleanup();
  while(1) { delay(10000); }
}

void setup() {
  // CRITICAL: Check for crash loop FIRST - before any other initialization
  // This allows recovery if the app is crashing on boot
  SafeMode::check();

  Serial.begin(config.serial.baudRate);
  while (!Serial && millis() < config.serial.initTimeoutMs) {}

  // Log boot count (from SafeMode crash detection)
  Serial.print(F("[BOOT] Count: "));
  Serial.print(SafeMode::getCrashCount());
  Serial.print(F("/"));
  Serial.println(SafeMode::CRASH_THRESHOLD);

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
    haltWithError(F("ERROR: Invalid matrix dimensions"));
  }
  if (config.matrix.brightness > 255) {
    Serial.println(F("WARNING: Brightness clamped to 255"));
  }

  // Initialize LED strip (must be done in setup, not global scope)
  neoPixelStrip = new(std::nothrow) Adafruit_NeoPixel(
      config.matrix.width * config.matrix.height,
      config.matrix.ledPin,
      config.matrix.ledType);
  if (!neoPixelStrip) {
    haltWithError(F("ERROR: NeoPixel allocation failed"));
  }
  leds = new(std::nothrow) NeoPixelLedStrip(*neoPixelStrip);
  if (!leds || !leds->isValid()) {
    haltWithError(F("ERROR: LED strip wrapper allocation failed"));
  }

  leds->begin();
  leds->setBrightness(min(config.matrix.brightness, 255));
  leds->show();

  // Basic LED test - light up first few LEDs to verify hardware
  Serial.print(F("LED Test: Lighting first 3 LEDs at brightness "));
  Serial.println(config.matrix.brightness);
  leds->setPixelColor(0, leds->Color(255, 0, 0));  // Should show RED
  leds->setPixelColor(1, leds->Color(0, 255, 0));  // Should show GREEN
  leds->setPixelColor(2, leds->Color(0, 0, 255));  // Should show BLUE
  leds->show();
  delay(3000);  // Hold for 3 seconds to verify colors are correct

  // Clear test LEDs
  leds->setPixelColor(0, 0);
  leds->setPixelColor(1, 0);
  leds->setPixelColor(2, 0);
  leds->show();

  if (!ledMapper.begin(config)) {
    haltWithError(F("ERROR: LED mapper initialization failed"));
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

  // Initialize appropriate generator based on layout type
  Serial.print(F("Initializing generators for layout type: "));
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

  // Create unified render pipeline (manages all generators and effects)
  pipeline = new(std::nothrow) RenderPipeline();
  if (!pipeline) {
    haltWithError(F("ERROR: RenderPipeline allocation failed"));
  }

  if (!pipeline->begin(config, *leds, ledMapper)) {
    haltWithError(F("ERROR: RenderPipeline initialization failed"));
  }

  // Initialize live-tunable fire params from config
  fireParams.baseCooling = config.fireDefaults.baseCooling;
  fireParams.sparkHeatMin = config.fireDefaults.sparkHeatMin;
  fireParams.sparkHeatMax = config.fireDefaults.sparkHeatMax;
  fireParams.sparkChance = config.fireDefaults.sparkChance;
  fireParams.audioSparkBoost = config.fireDefaults.audioSparkBoost;
  fireParams.coolingAudioBias = config.fireDefaults.coolingAudioBias;
  // Layout-specific params set by Fire::begin()
  fireParams.spreadDistance = 3;
  fireParams.heatDecay = 0.60f;

  Serial.println(F("RenderPipeline initialized with Fire, Water, Lightning generators"));
  Serial.println(F("Available effects: None, HueRotation"));

  // Initialize HAL-enabled components (must be done in setup(), not at global scope)
  battery = new(std::nothrow) BatteryMonitor(DefaultHal::gpio(), DefaultHal::adc(), DefaultHal::time());
  if (!battery) {
    haltWithError(F("ERROR: Battery monitor allocation failed"));
  }

  // Initialize unified AudioController (owns mic + rhythm tracking)
  audioCtrl = new(std::nothrow) AudioController(DefaultHal::pdm(), DefaultHal::time());
  if (!audioCtrl) {
    haltWithError(F("ERROR: AudioController allocation failed"));
  }

  if (!audioCtrl->begin(config.microphone.sampleRate)) {
    Serial.println(F("ERROR: AudioController failed to start"));
  } else {
    Serial.println(F("AudioController initialized (mic + rhythm tracking)"));
  }

  // Initialize configuration storage and load saved settings
  configStorage.begin();
  if (configStorage.isValid()) {
    // Load into local fireParams, then apply to pipeline's fire generator
    configStorage.loadConfiguration(fireParams, audioCtrl->getMicForTuning(), audioCtrl);
    updateFireParams();  // Apply to pipeline's fire generator
    Serial.println(F("Loaded saved configuration from flash"));
  } else {
    Serial.println(F("Using default configuration"));
  }

  // Initialize battery monitor
  if (!battery->begin()) {
    Serial.println(F("WARNING: Battery monitor failed to start"));
  } else {
    battery->setFastCharge(config.charging.fastChargeEnabled);
    Serial.println(F("Battery monitor initialized"));
  }

  // Initialize serial console for interactive settings management
  // Uses pipeline for generator/effect switching
  console = new(std::nothrow) SerialConsole(pipeline, &audioCtrl->getMicForTuning());
  if (!console) {
    haltWithError(F("ERROR: SerialConsole allocation failed"));
  }
  console->setConfigStorage(&configStorage);
  console->setBatteryMonitor(battery);
  console->setAudioController(audioCtrl);
  console->begin();
  Serial.println(F("Serial console initialized"));
  Serial.println(F("Commands: 'gen list', 'gen <fire|water|lightning>', 'effect list', 'effect <none|hue>'"));

  // FIX: Reset frame timing to prevent stale state from previous boot
  lastMs = 0;

  Serial.println(F("Setup complete!"));

  // Mark boot as stable - we made it through setup without crashing
  // This resets the crash counter so future boots start fresh
  SafeMode::markStable();
}

void loop() {
  uint32_t now = millis();
  float dt = (lastMs == 0) ? Constants::DEFAULT_FRAME_TIME : (now - lastMs) * 0.001f;

  // FIX: Add diagnostics when frame time exceeds maximum (indicates performance issues)
  if (dt > Constants::MAX_FRAME_TIME) {
    Serial.print(F("WARNING: Frame time exceeded: "));
    Serial.print((now - lastMs));
    Serial.println(F("ms - loop() running too slowly!"));
  }

  dt = constrain(dt, Constants::MIN_FRAME_TIME, Constants::MAX_FRAME_TIME); // Clamp dt to reasonable range
  lastMs = now;

  // Update unified audio controller (handles mic, rhythm tracking, and synthesis)
  if (audioCtrl) {
    const AudioControl& audio = audioCtrl->update(dt);

    // Optional: Send transient detection events for test analysis
    if (audio.pulse > 0.0f) {
      uint8_t mode = audioCtrl->getDetectionMode();
      Serial.print(F("{\"type\":\"TRANSIENT\",\"ts\":"));
      Serial.print(now);
      Serial.print(F(",\"strength\":"));
      Serial.print(audio.pulse, 2);
      Serial.print(F(",\"mode\":"));
      Serial.print(mode);
      Serial.print(F(",\"level\":"));
      Serial.print(audio.energy, 2);
      Serial.print(F(",\"phase\":"));
      Serial.print(audio.phase, 2);
      Serial.print(F(",\"rhythm\":"));
      Serial.print(audio.rhythmStrength, 2);
      Serial.println(F("}"));
    }
  }

  // Track charging state changes
  bool currentChargingState = battery ? battery->isCharging() : false;
  if (currentChargingState != prevChargingState) {
    if (currentChargingState) {
      Serial.println(F("Charging started"));
    } else {
      Serial.println(F("Charging stopped"));
    }
    prevChargingState = currentChargingState;
  }

  // Render current generator through pipeline
  renderFrame();

  // Handle serial commands via SerialConsole
  if (console) {
    console->update();
  }

  // Rhythm analyzer debug output (periodic, every 2 seconds when pattern detected)
  static uint32_t lastRhythmDebug = 0;
  if (audioCtrl && millis() - lastRhythmDebug > 2000) {
    const AudioControl& audio = audioCtrl->getControl();
    if (audio.rhythmStrength > 0.5f) {
      lastRhythmDebug = millis();
      Serial.print(F("{\"type\":\"RHYTHM\",\"bpm\":"));
      Serial.print(audioCtrl->getCurrentBpm(), 1);
      Serial.print(F(",\"strength\":"));
      Serial.print(audio.rhythmStrength, 2);
      Serial.println(F("}"));
    }
  }

  // Auto-save dirty settings to flash (debounced)
  if (audioCtrl) configStorage.saveIfDirty(fireParams, audioCtrl->getMicForTuning(), audioCtrl);

  // Battery monitoring - periodic voltage check
  static uint32_t lastBatteryCheck = 0;
  if (battery && millis() - lastBatteryCheck > Constants::BATTERY_CHECK_INTERVAL_MS) {
    lastBatteryCheck = millis();
    float voltage = battery->getVoltage();
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
