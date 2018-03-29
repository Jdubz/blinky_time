#include <Adafruit_NeoPixel.h>
int LEDPIN = 2;
const int LEDS = 256;
int frameRate = 30;
int frame = 0;

int rcNum(int R, int C) {
  if (R % 2) {
    return R*16 + (15 - C);
  }
  return (R*16) + C;
}

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip() {
  strip.show();
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(frameRate);
}

void setup() {
  Serial.begin(9600);
  
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 10, 0, 0);
  }
  strip.show();
  delay(300);
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 10, 0);
  }
  strip.show();
  delay(300);
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 10);
  }
  strip.show();
  delay(300);
  renderStrip();
  strip.show();
}

// With `port.write('1,100,80,60,');` in the node code

void loop() {
  int chan;
  int r;
  int g;
  int b;
  if (Serial.available() > 0) {
    chan = Serial.parseInt();
    r = Serial.parseInt();
    g = Serial.parseInt();
    b = Serial.parseInt();
    strip.setPixelColor(chan, r, g, b);
    Serial.println("packet " + String(chan));
  }
}
