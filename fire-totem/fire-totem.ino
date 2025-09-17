#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"
#include "SerialConsole.h"

#define WIDTH 16
#define HEIGHT 8
#define LED_PIN D10

Adafruit_NeoPixel leds(WIDTH * HEIGHT, LED_PIN, NEO_RGB + NEO_KHZ800);

FireEffect fire(leds, WIDTH, HEIGHT);
AdaptiveMic mic;
SerialConsole console(fire, leds);

void setup() {
  leds.begin();
  leds.show();
  fire.begin();
  mic.begin();
  console.begin();
}

void loop() {
  static unsigned long lastMs = millis();
  unsigned long now = millis();
  float dt = (now - lastMs) / 1000.0f;
  lastMs = now;

  // Update microphone with elapsed time
  mic.update(dt);

  // Get current audio energy level (0–1)
  float energy = mic.getLevel();

  // Update fire effect
  fire.update(energy);
  fire.show();

  // Handle serial commands
  console.update();
}
