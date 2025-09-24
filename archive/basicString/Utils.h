#ifndef Utils_h
#define Utils_h

#include <Adafruit_NeoPixel.h>

// Pin Constants
const int KNOBPIN = 14;
const int MICPIN = 15;

// Other Constants
const int sampleSize = 30;

// Globals
int knobIn = 0;
int threshhold = 100;
float gain = 50.0;


// TODO use library friendly data with strip.Color instead of this
// Colors stored and set in the order of
// green, red, blue not RGB
typedef struct colorValues {
  uint8_t green;
  uint8_t red;
  uint8_t blue;
}
color;

int getKnobValue() {
  return analogRead(KNOBPIN);
}

color getSingleColorValue() {
  color colorValue;
  knobIn = analogRead(KNOBPIN);
  // which third of circle is the knob in
  int phase = knobIn / 341.333;
  float ramp = (knobIn % 342) / 341.333;
  if (phase == 0) {
    colorValue.green = int(ramp * 255.0);
    colorValue.red = int((1.0 - ramp) * 255.0);
    colorValue.blue = 0;
  } else if (phase == 1) {
    colorValue.blue = int(ramp * 255.0);
    colorValue.green = int((1.0 - ramp) * 255.0);
    colorValue.red = 0;
  } else if (phase == 2) {
    colorValue.red = int(ramp * 255.0);
    colorValue.blue = int((1.0 - ramp) * 255.0);
    colorValue.green = 0;
  }

  return colorValue;
}

color getFlippedColorOf(color referenceColor) {
  color flipped;
  flipped.green = 255 - referenceColor.green;
  flipped.red = 255 - referenceColor.red;
  flipped.blue = 255 - referenceColor.blue;

  return flipped;
}

void keepBatteryOn(Adafruit_NeoPixel *strip) {
  // Keep every fifth light on so phone battery doesn't turn off
  color colorValue = getSingleColorValue();

  for (int on = 0; on < 5; on++) {
    strip->setPixelColor(on * 10,
      colorValue.green,
      colorValue.red,
      colorValue.blue);
  }
}

float getMicLevel() {
  int lvl1;
  unsigned long start = millis();
  int high = 0;
  int low = 1024;
  while (millis() - start < sampleSize) {
    int sample = analogRead(MICPIN);
    if (sample < low) {
      low = sample;
    } else if (sample > high) {
      high = sample;
    }
  }
  lvl1 = high - low;
  float micLvl = lvl1 / 1024.0;
  if (threshhold > 20) {
    threshhold = threshhold - 1;
  }
  if (lvl1 > threshhold) {
    threshhold = lvl1;
  }
  gain = 1024.0 / threshhold;
  micLvl = micLvl * gain;

  return micLvl;
}

#endif
