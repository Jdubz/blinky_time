#ifndef Pendulum_h
#define Pendulum_h

#include <Adafruit_NeoPixel.h>
#include "Color.h"
#include "LeastCommonMultiple.h"

const int LEDPIN = 2;
const int NUMLEDS = 5;
float brightness = 0.3;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

const uint8_t minWavelength = 400;
const uint8_t waveDifference = 2;

// TODO find least common denominator
unsigned long getCycleLength() {
  unsigned long cycleLength = 1;
  for (int wave = 0; wave < NUMLEDS; wave++) {
    int waveLength = minWavelength + (wave * waveDifference);
    cycleLength = lcm(cycleLength, waveLength);
  }
  return cycleLength;
}

unsigned long fullCycle = getCycleLength();

const int patternSpeed = 1;
unsigned long phaseStep(unsigned long phase) {
  unsigned long nextPhase = (phase + patternSpeed) % fullCycle;
  return nextPhase;
}

void pendulumStep(uint8_t colorVal, unsigned long phase) {
  color colors1 = getSingleColorValue(colorVal);
  color colors2 = getFlippedColorOf(colors1);
  for (int led = 0; led < NUMLEDS; led++) {
    int waveLength = minWavelength + waveDifference * led;
    int offset = phase % waveLength;
    float height;
    if (offset <= (waveLength / 2)) {
      height = float(offset) / (float(waveLength) / 2.0);
    } else {
      float halfSet = offset % (waveLength / 2);
      height = 1.0 - (halfSet / (float(waveLength) / 2));
    }
    color swungColor = calculateSwing(height, colors1, colors2);
    strip.setPixelColor(led, swungColor.green * brightness, swungColor.red * brightness, swungColor.blue * brightness);
  }
}

#endif
