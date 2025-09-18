#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"
#include "SerialConsole.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"
#include "TotemDefaults.h"

#define WIDTH 16
#define HEIGHT 8
#define LED_PIN D10

Adafruit_NeoPixel leds(WIDTH * HEIGHT, LED_PIN, NEO_RGB + NEO_KHZ800);

FireEffect fire(leds, WIDTH, HEIGHT);
AdaptiveMic mic;
SerialConsole console(fire, leds);
BatteryMonitor battery;
IMUHelper imu;

uint32_t lastMs = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println(F("Fire Totem Starting..."));

  leds.begin();
  leds.setBrightness(80);
  leds.show();

  fire.begin();

  bool micOk = mic.begin(16000, 32);
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
    battery.setFastCharge(true);
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

  // Console-controlled IMU motion effects and visualization
  if (imu.isReady() && (console.motionEnabled || console.heatVizEnabled)) {
    imu.updateMotion(dt);
    imu.updateIMUData(); // Update clean IMU data for debugging

    if (console.motionEnabled) {
      const MotionState& m = imu.motion();

      // DISABLED: Wind effect not working as expected
      // fire.setWind(m.wind.x * console.windScale, m.wind.y * console.windScale);

      // Gravity-based heat rising: Invert Z-axis for upside-down mounting
      fire.setUpVector(m.up.x, m.up.y, -m.up.z);  // Note the negative Z
    }
  }

  float energy = mic.getLevel();
  float hit = mic.getTransient();

  // Render fire effect or visualizations
  if (console.imuVizEnabled) {
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
      for (int i = 0; i < 16 * 8; i++) {
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
      for (int i = 0; i < 16 * 8; i++) {
        leds.setPixelColor(i, 0);
      }
      leds.show();
    }
  }

  console.update();
}
