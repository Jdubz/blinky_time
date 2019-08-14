#ifndef Pendulum_h
#define Pendulum_h

#include <Adafruit_NeoPixel.h>
#include "color.h"
#include "lcd.h"

const int LEDPIN = 2;
const int NUMLEDS = 5;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

const int DELAYTIME = 30;

const uint8_t minWavelength = 100;
const uint8_t waveDifference = 20;

// TODO find least common denominator
unsigned long getCycleLength() {
  unsigned long cycleLength = 1;
  for (int wave = 0; wave < NUMLEDS; wave++) {
    cycleLength = lcm(cycleLength, minWavelength + (wave * waveDifference));
  }
  return cycleLength;
}

unsigned long fullCycle = getCycleLength();

void pendulumStep(color colors, uint8_t phase) {
  for (int led = 0; led < NUMLEDS; led++) {
    int waveLength = minWavelength + waveDifference * led;
    int offset = phase % waveLength;
    float height;
    if (offset < (waveLength / 2)) {
      height = offset / (waveLength / 2);
    } else {
      height = 1.0 - (offset % (waveLength / 2));
    }
    strip.setPixelColor(led, colors.green * height, colors.red * height, colors.blue * height);
  }
}

#endif
