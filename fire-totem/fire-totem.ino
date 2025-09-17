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

void setup() {
  leds.begin();
  leds.setBrightness(150);
  leds.show();
  fire.begin();
  mic.begin();
  console.begin();
  imu.begin();

  battery.begin();                 // uses default pins/refs for XIAO Sense
  battery.setFastCharge(true);     // optional: enable 100 mA fast charging
}

void loop() {
  static unsigned long lastMs = millis();
  unsigned long now = millis();
  float dt = (now - lastMs) / 1000.0f; if (dt < 0) dt = 0; lastMs = now;

  mic.update(dt);
  mic.autoGainTick(dt);  // NEW: fast software auto-gain
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
