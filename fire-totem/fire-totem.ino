#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"
#include "SerialConsole.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"
#include "TotemDefaults.h"
#include "configs/TubeLightConfig.h"

const DeviceConfig& config = TUBE_LIGHT_CONFIG;

Adafruit_NeoPixel leds(config.matrix.width * config.matrix.height, config.matrix.ledPin, config.matrix.ledType);

FireEffect fire(leds, config.matrix.width, config.matrix.height);
AdaptiveMic mic;
SerialConsole console(fire, leds);
BatteryMonitor battery;
IMUHelper imu;

uint32_t lastMs = 0;
bool prevChargingState = false;

void setup() {
  Serial.begin(config.serial.baudRate);
  while (!Serial && millis() < config.serial.initTimeoutMs) {}
  Serial.print(F("Starting device: "));
  Serial.println(config.deviceName);

  leds.begin();
  leds.setBrightness(config.matrix.brightness);
  leds.show();

  fire.begin();

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
  float dt = (lastMs == 0) ? 0.016f : (now - lastMs) * 0.001f;
  dt = constrain(dt, 0.001f, 0.1f); // Clamp dt to reasonable range
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
      // Just started charging - auto-activate battery visualization
      console.batteryVizEnabled = true;
      Serial.println(F("Auto-activated battery visualization (charging detected)"));
    } else if (!currentChargingState && prevChargingState) {
      // Just stopped charging - auto-deactivate if it was auto-activated
      if (console.batteryVizEnabled) {
        console.batteryVizEnabled = false;
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
      fire.update(energy, hit);
      fire.render(); // Render fire to matrix but don't show yet
      console.renderTopVisualization(); // Add top indicator and show
    } else {
      // Fire disabled - clear display and show only top indicator
      for (int i = 0; i < config.matrix.width * config.matrix.height; i++) {
        leds.setPixelColor(i, 0);
      }
      console.renderTopVisualization();
    }
  } else {
    // Normal fire mode
    if (!console.fireDisabled) {
      fire.update(energy, hit);
      fire.show();
    } else {
      // Fire disabled - clear display
      for (int i = 0; i < config.matrix.width * config.matrix.height; i++) {
        leds.setPixelColor(i, 0);
      }
      leds.show();
    }
  }

  console.update();

  // Battery monitoring
  static uint32_t lastBatteryCheck = 0;
  if (millis() - lastBatteryCheck > 30000) { // Check every 30 seconds
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
