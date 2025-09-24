#include <Adafruit_NeoPixel.h>
#include <math.h>

const int NUMLEDS = 10;
const int LEDPIN = 2;
const int MICPIN = A0;
const int DELAYTIME = 30;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

int threshhold = 100;
float gain = 50.0;

float getMicLevel() {
  int lvl1;
  unsigned long start = millis();
  int high = 0;
  int low = 1024;
  while (millis() - start < DELAYTIME) {
    int sample = analogRead(MICPIN);
    if (sample < low) {
      low = sample;
    } else if (sample > high) {
      high = sample;
    }
  }
  lvl1 = high - low;
  float micLevel = lvl1 / 1024.0;
  if (threshhold > 20) {
    threshhold = threshhold - 1;
  }
  if (lvl1 > threshhold) {
    threshhold = lvl1;
  }
  gain = 1024.0 / threshhold;
  // Serial.println(String(micLevel) + " : " + String(gain));
  return micLevel * gain;
}

void render() {
  strip.show();
  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

void setup() {
  Serial.begin(9600);
  strip.begin();
  strip.show();

}

int toplvl = 40;

void loop() {
  float micLvl = getMicLevel();

  strip.setPixelColor(4, 0, 0, 100);
  strip.setPixelColor(5, 0, 0, 100);

  int lvl = round(40 * micLvl);
  if (lvl > toplvl) {
    toplvl = lvl;
  }

  for (int meter = 0; meter <= toplvl/4; meter++) {
    strip.setPixelColor(4 - meter, 0, 100, 0);
    strip.setPixelColor(5 + meter, 0, 100, 0);
  }
  toplvl--;

  render();
}
