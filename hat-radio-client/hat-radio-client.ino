#include <Adafruit_NeoPixel.h>

#include "types.h"
#include "radio.h"
#include "sparks.h"
#include "timer.h"
#include "radio.h"

#define LED_PIN     2
#define PULL_PIN    5

#define NUM_LEDS    5

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);

WifiClient* wifiClient = new WifiClient();

Timer* renderTimer = new Timer(30);
Sparks* sparks = new Sparks(NUM_LEDS);

color frame[NUM_LEDS];
void initFrame() {
  for (int i = 0; i < NUM_LEDS; i++) {
    color empty = { 0, 0, 0 };
    frame[i] = empty;
  }
}

void clear() {
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

void startup() {
  strip.begin();
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 50, 0);
  }
  strip.show();
  delay(500);
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 50, 0, 0);
  }
  strip.show();
  delay(500);
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 50);
  }
  strip.show();
  delay(500);
  clear();
  strip.show();
  delay(500);
}

void render() {
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, frame[led].red, frame[led].green, frame[led].blue);
  }
  strip.show();
  clear();
}

void setup() {
  Serial.begin(115200);
  startup();
  initFrame();
  wifiClient->startEsp();
}

void loop() {

  if (renderTimer->trigger()) {
    float micLvl = wifiClient->read();
    sparks->run(frame, micLvl);
    render();
  }
}
