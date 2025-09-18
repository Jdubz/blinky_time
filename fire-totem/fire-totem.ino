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
  leds.setBrightness(150);
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

  if (imu.isReady()) {
    imu.updateMotion(dt);
    const MotionState& m = imu.motion();

    // Enhanced torch motion integration
    fire.setTorchMotion(
      m.wind.x, m.wind.y, m.stoke,
      m.turbulenceLevel, m.centrifugalForce, m.flameBend,
      m.tiltAngle, m.motionIntensity
    );

    fire.setRotationalEffects(m.spinMagnitude, m.centrifugalForce);
    fire.setInertialDrift(m.inertiaDrift.x, m.inertiaDrift.y);
    fire.setFlameDirection(m.flameDirection, m.flameBend);
    fire.setMotionTurbulence(m.turbulenceLevel);

    // Maintain backward compatibility
    fire.setUpVector(m.up.x, m.up.y, m.up.z);
  }

  float energy = mic.getLevel();
  float hit = mic.getTransient();

  fire.update(energy, hit);
  fire.show();

  console.update();
}
