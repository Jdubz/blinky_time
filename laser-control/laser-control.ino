#include <Adafruit_NeoPixel.h>

#include "microphone.h"

#define LASER_PIN     D0

Microphone* mic;

void startup() {
  mic = new Microphone();
}

void setup() {
  Serial.begin(9600);
  startup();
}

void loop() {
  float micLvl = mic->read();
  int laserLevel = 255 * micLvl;
  analogWrite(LASER_PIN, laserLevel);
}
