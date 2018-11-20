#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int NUMLEDS = 17;
const int frameRate = 30;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip() {
  strip.show();
  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(1000/frameRate);
}

void setup() {
  strip.begin();
  strip.show();

  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 50, 0);
  }
  strip.show();
  delay(1000);
}

int sparks[NUMLEDS][3];

void loop() {

  if (random(4) == 1) {
    int sparkId = random(NUMLEDS);
    sparks[sparkId][0] = 80;
    sparks[sparkId][1] = random(4) * 10;
    sparks[sparkId][2] = 1;
  }

  for (int thisSpark = 0; thisSpark < NUMLEDS; thisSpark++) {
    if (sparks[thisSpark][2] == 1) {       
      if (sparks[thisSpark][1] > 2) {
        sparks[thisSpark][1] -= 2;
      } else if (sparks[thisSpark][1] != 0) {
        Serial.println(sparks[thisSpark][1]);
      }
      if (sparks[thisSpark][0] > 2) {
        sparks[thisSpark][0] -= 2;
      } else {
        sparks[thisSpark][0] = 0;
        sparks[thisSpark][1] = 0;
        sparks[thisSpark][2] = 0;
      } 
    }
    strip.setPixelColor(thisSpark, sparks[thisSpark][0], sparks[thisSpark][1], 0);
  }
  
  renderStrip();
}
