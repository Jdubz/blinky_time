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
#include "render/RenderPipeline.h"  // Generator/Effect/Renderer management
#include "types/Version.h"           // Version information from repository
#include "tests/SafeMode.h"          // Crash recovery system
#include "hal/DefaultHal.h"          // HAL singleton instances
#include "hal/hardware/NeoPixelLedStrip.h"  // LED strip wrapper

// Device Configuration Selection
// Define DEVICE_TYPE to select active configuration:
// 1 = Hat (89 LEDs, LINEAR_LAYOUT)
// 2 = Tube Light (4x15 matrix, MATRIX_LAYOUT)
// 3 = Bucket Totem (16x8 matrix, MATRIX_LAYOUT)
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
RenderPipeline* pipeline = nullptr;  // Manages generators, effects, and rendering

// HAL-enabled components - use pointers to avoid static initialization order fiasco
// These are initialized in setup() AFTER Arduino runtime is ready
AudioController* audioController = nullptr;  // Unified audio analysis (owns AdaptiveMic internally)
BatteryMonitor* battery = nullptr;
IMUHelper imu;                     // IMU sensor interface; auto-initializes, uses stub mode if LSM6DS3 not installed
ConfigStorage configStorage;       // Persistent settings storage
SerialConsole* console = nullptr;  // Serial command interface

uint32_t lastMs = 0;
bool prevChargingState = false;

// Live-tunable fire parameters
FireParams fireParams;

void updateFireParams() {
  if (!pipeline) return;
  Fire* f = pipeline->getFireGenerator();
  if (f) {
    f->setParams(fireParams);
  }
}

// Helper function for Generator-Effect-Renderer pipeline
void renderFrame() {
  // Generate -> Effect -> Render -> Display pipeline
  if (pipeline && audioController) {
    const AudioControl& audio = audioController->getControl();
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
  delete pipeline;   pipeline = nullptr;  // RenderPipeline cleans up generators/effects/renderer
  delete battery;    battery = nullptr;
  delete audioController; audioController = nullptr;
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

  // Display version and device information (always show on boot)
  Serial.println(F("=== BLINKY TIME STARTUP ==="));
  Serial.println(F(BLINKY_FULL_VERSION));
  Serial.print(F("Device: "));
  Serial.println(config.deviceName);

  // Debug: detailed boot info
  if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
    Serial.print(F("[DEBUG] Boot count: "));
    Serial.print(SafeMode::getCrashCount());
    Serial.print(F("/"));
    Serial.println(SafeMode::CRASH_THRESHOLD);
    Serial.print(F("[DEBUG] Build: ")); Serial.print(F(BLINKY_BUILD_DATE));
    Serial.print(F(" ")); Serial.println(F(BLINKY_BUILD_TIME));
#if DEVICE_TYPE == 1
    Serial.println(F("[DEBUG] Device Type: Hat (Type 1)"));
#elif DEVICE_TYPE == 2
    Serial.println(F("[DEBUG] Device Type: Tube Light (Type 2)"));
#elif DEVICE_TYPE == 3
    Serial.println(F("[DEBUG] Device Type: Bucket Totem (Type 3)"));
#endif
  }

  // Validate critical configuration parameters
  if (config.matrix.width <= 0 || config.matrix.height <= 0) {
    haltWithError(F("ERROR: Invalid matrix dimensions"));
  }
  if (config.matrix.brightness > 255) {
    SerialConsole::logWarn(F("Brightness clamped to 255"));
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
  if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
    Serial.print(F("[DEBUG] LED Test at brightness "));
    Serial.println(config.matrix.brightness);
  }
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

  // Debug: detailed config info
  if (SerialConsole::getGlobalLogLevel() >= LogLevel::DEBUG) {
    Serial.print(F("[DEBUG] Layout: "));
    switch (config.matrix.layoutType) {
      case MATRIX_LAYOUT:  Serial.print(F("MATRIX")); break;
      case LINEAR_LAYOUT:  Serial.print(F("LINEAR")); break;
      case RANDOM_LAYOUT:  Serial.print(F("RANDOM")); break;
      default:             Serial.print(F("UNKNOWN")); break;
    }
    Serial.print(F(", Matrix: "));
    Serial.print(config.matrix.width);
    Serial.print(F("x"));
    Serial.print(config.matrix.height);
    Serial.print(F(" = "));
    Serial.print(config.matrix.width * config.matrix.height);
    Serial.println(F(" LEDs"));
  }

  // Initialize RenderPipeline (manages generators, effects, and rendering)
  pipeline = new(std::nothrow) RenderPipeline();
  if (!pipeline || !pipeline->begin(config, *leds, ledMapper)) {
    haltWithError(F("ERROR: RenderPipeline initialization failed"));
  }

  // Sync fireParams FROM the Fire generator (preserves layout-specific values set by Fire::begin)
  Fire* fireGen = pipeline->getFireGenerator();
  if (fireGen) {
    fireParams = fireGen->getParams();  // Copy all params including layout-specific ones
  }

  SerialConsole::logDebug(F("RenderPipeline initialized"));

  // Initialize HAL-enabled components (must be done in setup(), not at global scope)
  audioController = new(std::nothrow) AudioController(DefaultHal::pdm(), DefaultHal::time());
  battery = new(std::nothrow) BatteryMonitor(DefaultHal::gpio(), DefaultHal::adc(), DefaultHal::time());
  if (!audioController || !battery) {
    haltWithError(F("ERROR: HAL component allocation failed"));
  }

  // Initialize audio controller (owns microphone internally)
  bool audioOk = audioController->begin(config.microphone.sampleRate);
  if (!audioOk) {
    SerialConsole::logError(F("Audio controller failed to start"));
  } else {
    SerialConsole::logDebug(F("Audio controller initialized"));
  }

  // Initialize configuration storage and load saved settings
  configStorage.begin();
  if (configStorage.isValid()) {
    configStorage.loadConfiguration(fireParams, audioController->getMicForTuning(), audioController);
    updateFireParams();
    SerialConsole::logDebug(F("Loaded config from flash"));
  } else {
    SerialConsole::logDebug(F("Using default config"));
  }

  // Initialize battery monitor
  if (!battery->begin()) {
    SerialConsole::logWarn(F("Battery monitor failed to start"));
  } else {
    battery->setFastCharge(config.charging.fastChargeEnabled);
    SerialConsole::logDebug(F("Battery monitor initialized"));
  }

  // Note: Rhythm tracking is now handled internally by AudioController

  // Initialize serial console for interactive settings management
  // Uses RenderPipeline for generator/effect switching
  console = new(std::nothrow) SerialConsole(pipeline, &audioController->getMicForTuning());
  if (!console) {
    haltWithError(F("ERROR: SerialConsole allocation failed"));
  }
  console->setConfigStorage(&configStorage);
  console->setBatteryMonitor(battery);
  console->setAudioController(audioController);
  console->begin();
  SerialConsole::logDebug(F("Serial console initialized"));

  // FIX: Reset frame timing to prevent stale state from previous boot
  lastMs = 0;

  Serial.println(F("Ready."));

  // Mark boot as stable - we made it through setup without crashing
  // This resets the crash counter so future boots start fresh
  SafeMode::markStable();
}

void loop() {
  uint32_t now = millis();
  float dt = (lastMs == 0) ? Constants::DEFAULT_FRAME_TIME : (now - lastMs) * 0.001f;

  // FIX: Add diagnostics when frame time exceeds maximum (indicates performance issues)
  if (dt > Constants::MAX_FRAME_TIME) {
    if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
      Serial.print(F("[WARN] Frame time: "));
      Serial.print((now - lastMs));
      Serial.println(F("ms"));
    }
  }

  dt = constrain(dt, Constants::MIN_FRAME_TIME, Constants::MAX_FRAME_TIME); // Clamp dt to reasonable range
  lastMs = now;

  // Update unified audio controller (handles mic + rhythm tracking internally)
  if (audioController) {
    audioController->update(dt);
  }

  // Get pulse from audio control signal for transient logging
  const AudioControl& audio = audioController ? audioController->getControl() : AudioControl{};
  float pulse = audio.pulse;

  // Send transient detection events when debug channel is enabled
  // Use: "debug transient on" to enable, "debug transient off" to disable
  if (audioController && pulse > 0.0f &&
      SerialConsole::isDebugChannelEnabled(DebugChannel::TRANSIENT)) {
    // TRANSIENT message: simplified single-band detection
    Serial.print(F("{\"type\":\"TRANSIENT\",\"timestampMs\":"));
    Serial.print(now);
    Serial.print(F(",\"strength\":"));
    Serial.print(pulse, 2);
    Serial.println(F("}"));
  }

  // Track charging state changes
  bool currentChargingState = battery ? battery->isCharging() : false;
  if (currentChargingState != prevChargingState) {
    if (currentChargingState) {
      SerialConsole::logInfo(F("Charging started"));
    } else {
      SerialConsole::logInfo(F("Charging stopped"));
    }
    prevChargingState = currentChargingState;
  }

  // Render current generator through the effect pipeline
  renderFrame();

  // Handle serial commands via SerialConsole
  if (console) {
    console->update();
  }

  // Auto-save dirty settings to flash (debounced)
  if (audioController) configStorage.saveIfDirty(fireParams, audioController->getMicForTuning(), audioController);

  // Battery monitoring - periodic voltage check
  static uint32_t lastBatteryCheck = 0;
  if (battery && millis() - lastBatteryCheck > Constants::BATTERY_CHECK_INTERVAL_MS) {
    lastBatteryCheck = millis();
    float voltage = battery->getVoltage();
    if (voltage > 0 && voltage < config.charging.criticalBatteryThreshold) {
      if (SerialConsole::getGlobalLogLevel() >= LogLevel::ERROR) {
        Serial.print(F("[ERROR] CRITICAL BATTERY: "));
        Serial.print(voltage);
        Serial.println(F("V"));
      }
    } else if (voltage > 0 && voltage < config.charging.lowBatteryThreshold) {
      if (SerialConsole::getGlobalLogLevel() >= LogLevel::WARN) {
        Serial.print(F("[WARN] Low battery: "));
        Serial.print(voltage);
        Serial.println(F("V"));
      }
    }
  }
}
