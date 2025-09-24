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

void loop() {
 
  if (Serial.available()) {
    byte read[768];
    Serial.readBytes(read, 768);
    int nextPixel[3]; 
    for (int b = 0; b < 768; b++) {
      nextPixel[b%3] = read[b];
      Serial.println(read[b]);
      if (b%3 == 2) {
        strip.setPixelColor(floor(b/3), nextPixel[0], nextPixel[1], nextPixel[2]);
      }
    }
    renderStrip();
  }
//
//  frame++;
//
//  if (frame == frameRate * 100) {
//    renderStrip();
//    frame = 0;
//  }
}
