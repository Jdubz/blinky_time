#include "pendulum.h"
#include "color.h"
#include "pattern.h"

const int BUTTONPIN = 3;
pattern patternValue;
const int patternSpeed = 1;

bool pressed = false;

const int DELAYTIME = 30;

void render() {
  strip.show();
  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(DELAYTIME);
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

  pendulumStep(patternValue.color, patternValue.phase);
  patternValue.phase = phaseStep(patternValue.phase, patternSpeed);
  
  render();
};
