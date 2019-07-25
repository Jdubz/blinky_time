#include <Adafruit_NeoPixel.h>
int LEDPIN = 2;
const int LEDS = 50;
int frameRate = 30;
int frame = 0;
int maxFrames = 300;

int ledLvls[LEDS][2];

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void phase1() {
  if (!((frame / frameRate) % 2)) {
    for (int led = 0; led < LEDS; led++) {
      strip.setPixelColor(led, 200, 100, 0);
    }
  }
}

void phase2() {
  strip.setPixelColor(LEDS - ((frame) % LEDS), 0, 200, 100);
  strip.setPixelColor(LEDS - ((frame+1) % LEDS), 0, 200, 80);
  strip.setPixelColor(LEDS - ((frame+2) % LEDS), 0, 200, 60);
  strip.setPixelColor(LEDS - ((frame+3) % LEDS), 0, 200, 40);
  strip.setPixelColor(LEDS - ((frame+4) % LEDS), 0, 200, 20);
  strip.setPixelColor(LEDS - ((frame+5) % LEDS), 0, 200, 00);
}

void nextFrame() {
  frame++;
  if (frame == maxFrames) {
    frame = 0;
  }
}

void renderStrip() {
  strip.show();
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(1000/frameRate);
}

void setup() {
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 50, 0);
  }
  strip.show();
  delay(1000);
}

void loop() {
  if (frame > (maxFrames / 2)) {
    phase2();
  } else {
    phase1();
  }
  
  nextFrame();
  renderStrip();
}
