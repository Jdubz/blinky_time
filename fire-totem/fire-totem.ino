#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"
#include "SerialConsole.h"
#include "BatteryMonitor.h"
#include "IMUHelper.h"

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
  leds.begin();
  leds.setBrightness(150);
  leds.show();
  fire.begin();
  bool ok = mic.begin(16000, 32); // sample rate, initial hardware gain
  if (!ok) {
    Serial.println("Microphone failed to start");
  } else {
    Serial.println("Microphone initialized");
  }
  console.begin();
  imu.begin();

  battery.begin();                 // uses default pins/refs for XIAO Sense
  battery.setFastCharge(true);     // optional: enable 100 mA fast charging
}

void loop() {
  uint32_t now = millis();
  float dt = (now - lastMs) * 0.001f;
  lastMs = now;
  mic.update(dt);

  imu.updateMotion(dt);

  const MotionState& m = imu.motion();  // or whatever your helper exposes

  fire.setWind(m.wind.x, m.wind.y);
  fire.setUpVector(m.up.x, m.up.y, m.up.z);
  fire.setStoke(m.stoke);

  float energy = mic.getLevel();
  fire.update(energy);
  fire.show();

  console.update();
}
