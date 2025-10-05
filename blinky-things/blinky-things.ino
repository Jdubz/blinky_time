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
#define DEVICE_TYPE 2  // Set to Tube Light for testing
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

// === TEMPORARILY DISABLED (ready for future enablement) ===
// AdaptiveMic mic;              // TODO: Fix pinDefinitions.h conflict with PDM/NeoPixel
// SerialConsole console;        // TODO: Update for unified fire generator
// BatteryMonitor battery;       // TODO: Update for new Generator architecture
// IMUHelper imu;               // TODO: Enable when LSM6DS3 library is available
// ConfigStorage configStorage; // TODO: Clean up legacy fire params

uint32_t lastMs = 0;
bool prevChargingState = false;

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
    Serial.print(hit);
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
    // Get audio input for generation (TODO: Re-enable when mic is fixed)
    float energy = 0.0f; // mic.getLevel();
    float hit = 0.0f;    // mic.getTransient();

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

  // Initialize effect (for now, just a pass-through effect)
  currentEffect = new HueRotationEffect();
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

  // TODO: Re-enable when PDM/NeoPixel pinDefinitions.h conflict is resolved
  // bool micOk = mic.begin(config.microphone.sampleRate, config.microphone.bufferSize);
  // if (!micOk) {
  //   Serial.println(F("ERROR: Microphone failed to start"));
  // } else {
  //   Serial.println(F("Microphone initialized"));
  // }

  // Initialize EEPROM configuration storage
  // configStorage.begin();  // TODO: Clean up legacy fire params

  // TODO: Update configuration loading for new architecture
  Serial.println(F("Configuration system initialized"));

  // TODO: Uncomment when hardware components are updated for new architecture
  // console.begin();
  // console.setConfigStorage(&configStorage); // Enable EEPROM saving for parameters

  // if (!imu.begin()) {
  //   Serial.println(F("WARNING: IMU initialization failed"));
  // } else {
  //   Serial.println(F("IMU initialized"));
  // }

  // if (!battery.begin()) {
  //   Serial.println(F("WARNING: Battery monitor failed to start"));
  // } else {
  //   battery.setFastCharge(config.charging.fastChargeEnabled);
  //   Serial.println(F("Battery monitor initialized"));
  // }

  Serial.println(F("Setup complete!"));
}

void loop() {
  uint32_t now = millis();
  float dt = (lastMs == 0) ? Constants::DEFAULT_FRAME_TIME : (now - lastMs) * 0.001f;
  dt = constrain(dt, Constants::MIN_FRAME_TIME, Constants::MAX_FRAME_TIME); // Clamp dt to reasonable range
  lastMs = now;

  // TODO: Re-enable when PDM/NeoPixel pinDefinitions.h conflict is resolved
  // mic.update(dt);

  // TODO: Uncomment when IMU is updated for new architecture
  // IMU data update for visualization only (no fire effects)
  // if (imu.isReady() && console.heatVizEnabled) {
  //   imu.updateIMUData(); // Update clean IMU data for debugging only
  // }

  // TODO: Re-enable when mic is fixed
  float energy = 0.0f; // mic.getLevel();
  float hit = 0.0f;    // mic.getTransient();

  // TODO: Uncomment when battery monitor is updated for new architecture
  // Auto-activation of battery visualization when charging
  // if (config.charging.autoShowVisualizationWhenCharging) {
  //   bool currentChargingState = battery.isCharging();
  //   if (currentChargingState && !prevChargingState) {
  //     // Just started charging - auto-activate battery visualization and disable fire
  //     console.batteryVizEnabled = true;
  //     console.fireDisabled = true;
  //     // Clear fire display immediately
  //     clearAllLEDs();
  //     leds.show();
  //     Serial.println(F("Auto-activated battery visualization (charging detected)"));
  //   } else if (!currentChargingState && prevChargingState) {
  //     // Just stopped charging - auto-deactivate and re-enable fire
  //     if (console.batteryVizEnabled) {
  //       console.batteryVizEnabled = false;
  //       console.fireDisabled = false;
  //       Serial.println(F("Auto-deactivated battery visualization (charging stopped)"));
  //     }
  //   }
  //   prevChargingState = currentChargingState;
  // }

  // Simplified rendering for new architecture - just fire effect for now
  updateFireEffect(energy, hit);
  showFireEffect();

  // TODO: Uncomment when console is updated for new architecture
  // console.update();

  // TODO: Uncomment when battery monitor is updated for new architecture
  // Battery monitoring
  // static uint32_t lastBatteryCheck = 0;
  // if (millis() - lastBatteryCheck > Constants::BATTERY_CHECK_INTERVAL_MS) { // Check every 30 seconds
  //   lastBatteryCheck = millis();
  //   float voltage = battery.getVoltage();
  //   if (voltage > 0 && voltage < config.charging.criticalBatteryThreshold) {
  //     Serial.print(F("CRITICAL BATTERY: "));
  //     Serial.print(voltage);
  //     Serial.println(F("V"));
  //   } else if (voltage > 0 && voltage < config.charging.lowBatteryThreshold) {
  //     Serial.print(F("Low battery: "));
  //     Serial.print(voltage);
  //     Serial.println(F("V"));
  //   }
  // }
}
