#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"
#include "StringFireEffect.h"
#include "SerialConsole.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"
#include "TotemDefaults.h"
#include "Globals.h"
#include "Constants.h"

// Device Configuration Selection
// Define DEVICE_TYPE to select active configuration:
// 1 = Hat (89 LEDs, STRING_FIRE mode)
// 2 = Tube Light (4x15 matrix, MATRIX_FIRE mode)  
// 3 = Bucket Totem (16x8 matrix, MATRIX_FIRE mode)
#ifndef DEVICE_TYPE
#define DEVICE_TYPE 1  // Default to Hat
#endif

#if DEVICE_TYPE == 1
#include "configs/HatConfig.h"
const DeviceConfig& config = HAT_CONFIG;
#elif DEVICE_TYPE == 2
#include "configs/TubeLightConfig.h"  
const DeviceConfig& config = TUBE_LIGHT_CONFIG;
#elif DEVICE_TYPE == 3
#include "configs/BucketTotemConfig.h"
const DeviceConfig& config = BUCKET_TOTEM_CONFIG;
#else
#error "Invalid DEVICE_TYPE. Use 1=Hat, 2=TubeLight, 3=BucketTotem"
#endif
LEDMapper ledMapper;

Adafruit_NeoPixel leds(config.matrix.width * config.matrix.height, config.matrix.ledPin, config.matrix.ledType);

// Fire effect - will be initialized in setup() based on config
FireEffect fire(leds, 1, 1); // Temporary initialization, will be reconfigured
StringFireEffect* stringFire = nullptr; // Alternative fire effect for strings

AdaptiveMic mic;
SerialConsole console(fire, leds);
BatteryMonitor battery;
IMUHelper imu;

uint32_t lastMs = 0;
bool prevChargingState = false;

// Helper function to clear all LEDs
void clearAllLEDs() {
  for (int i = 0; i < ledMapper.getTotalPixels(); i++) {
    leds.setPixelColor(i, Constants::LED_OFF);
  }
}

// Helper functions for fire effects
void updateFireEffect(float energy, float hit) {
  static uint32_t lastDebug = 0;
  if (millis() - lastDebug > 5000) {  // Debug every 5 seconds
    lastDebug = millis();
    Serial.print(F("Fire update - energy: "));
    Serial.print(energy);
    Serial.print(F(", hit: "));
    Serial.print(hit);
    Serial.print(F(", using: "));
    Serial.println(config.matrix.fireType == STRING_FIRE ? F("STRING_FIRE") : F("MATRIX_FIRE"));
  }

  if (config.matrix.fireType == STRING_FIRE && stringFire) {
    stringFire->update(energy, hit);
  } else {
    fire.update(energy, hit);
  }
}

void showFireEffect() {
  if (config.matrix.fireType == STRING_FIRE && stringFire) {
    stringFire->show();
  } else {
    fire.show();
  }
}

void renderFireEffect() {
  if (config.matrix.fireType == STRING_FIRE && stringFire) {
    stringFire->render();
  } else {
    fire.render();
  }
}

void setup() {
  Serial.begin(config.serial.baudRate);
  while (!Serial && millis() < config.serial.initTimeoutMs) {}
  
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

  // Initialize fire effect based on config type
  Serial.print(F("Config fire type: "));
  Serial.println(config.matrix.fireType == STRING_FIRE ? F("STRING_FIRE") : F("MATRIX_FIRE"));
  Serial.print(F("Matrix dimensions: "));
  Serial.print(config.matrix.width);
  Serial.print(F(" x "));
  Serial.print(config.matrix.height);
  Serial.print(F(" = "));
  Serial.print(config.matrix.width * config.matrix.height);
  Serial.println(F(" LEDs"));

  if (config.matrix.fireType == STRING_FIRE) {
    Serial.println(F("Initializing STRING fire effect"));
    stringFire = new StringFireEffect(leds, config.matrix.width * config.matrix.height);
    if (stringFire) {
      stringFire->begin();
      stringFire->restoreDefaults(); // Apply device-specific config defaults
      Serial.println(F("String fire effect initialized with config defaults"));
    } else {
      Serial.println(F("ERROR: String fire effect allocation failed"));
      while(1); // Halt execution
    }
  } else {
    Serial.println(F("Initializing MATRIX fire effect"));
    // Reinitialize the matrix fire with proper dimensions
    fire.~FireEffect();
    new(&fire) FireEffect(leds, config.matrix.width, config.matrix.height);
    fire.begin();
    fire.restoreDefaults(); // Apply device-specific config defaults
    Serial.println(F("Matrix fire effect initialized with config defaults"));
  }

  bool micOk = mic.begin(config.microphone.sampleRate, config.microphone.bufferSize);
  if (!micOk) {
    Serial.println(F("ERROR: Microphone failed to start"));
  } else {
    Serial.println(F("Microphone initialized"));
  }

  console.begin();

  if (!imu.begin()) {
    Serial.println(F("WARNING: IMU initialization failed"));
  } else {
    Serial.println(F("IMU initialized"));
  }

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

  // IMU data update for visualization only (no fire effects)
  if (imu.isReady() && console.heatVizEnabled) {
    imu.updateIMUData(); // Update clean IMU data for debugging only
  }

  float energy = mic.getLevel();
  float hit = mic.getTransient();

  // Auto-activation of battery visualization when charging
  if (config.charging.autoShowVisualizationWhenCharging) {
    bool currentChargingState = battery.isCharging();
    if (currentChargingState && !prevChargingState) {
      // Just started charging - auto-activate battery visualization and disable fire
      console.batteryVizEnabled = true;
      console.fireDisabled = true;
      // Clear fire display immediately
      clearAllLEDs();
      leds.show();
      Serial.println(F("Auto-activated battery visualization (charging detected)"));
    } else if (!currentChargingState && prevChargingState) {
      // Just stopped charging - auto-deactivate and re-enable fire
      if (console.batteryVizEnabled) {
        console.batteryVizEnabled = false;
        console.fireDisabled = false;
        Serial.println(F("Auto-deactivated battery visualization (charging stopped)"));
      }
    }
    prevChargingState = currentChargingState;
  }

  // Render fire effect or visualizations
  if (console.testPatternEnabled) {
    // Test pattern mode - highest priority for layout verification
    console.renderTestPattern();
  } else if (console.batteryVizEnabled) {
    // Battery visualization mode - override other displays
    console.renderBatteryVisualization();
  } else if (console.imuVizEnabled) {
    // IMU visualization mode - override fire display
    console.renderIMUVisualization();
  } else if (console.heatVizEnabled) {
    // Cylinder top visualization mode - show fire + top indicator
    if (!console.fireDisabled) {
      updateFireEffect(energy, hit);
      renderFireEffect(); // Render fire to matrix but don't show yet
      console.renderTopVisualization(); // Add top indicator and show
    } else {
      // Fire disabled - clear display and show only top indicator
      clearAllLEDs();
      console.renderTopVisualization();
    }
  } else {
    // Normal fire mode
    if (!console.fireDisabled) {
      updateFireEffect(energy, hit);
      showFireEffect();
    } else {
      // Fire disabled - clear display
      clearAllLEDs();
      leds.show();
    }
  }

  console.update();

  // Battery monitoring
  static uint32_t lastBatteryCheck = 0;
  if (millis() - lastBatteryCheck > Constants::BATTERY_CHECK_INTERVAL_MS) { // Check every 30 seconds
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
