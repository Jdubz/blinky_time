#include "pendulum.h"
#include "color.h"
#include "pattern.h"

const int BUTTONPIN = 3;
pattern patternValue;

bool pressed = false;

void render() {
  strip.show();
  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

void setup() {
  pinMode(BUTTONPIN, INPUT);
  Serial.begin(9600);
  strip.begin();
  strip.show();
  patternValue = newPattern();
}

void loop() {

  if (digitalRead(BUTTONPIN) == HIGH && !pressed) {
    pressed = true;
    patternValue = newPattern();
  } else if (pressed && digitalRead(BUTTONPIN) == LOW) {
    pressed = false;
  }

  color colorVal = getSingleColorValue(patternValue.color);
  pendulumStep(colorVal, patternValue.phase);
  
  render();
};
