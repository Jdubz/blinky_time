#include "microphone.h"
#include "timer.h"

#define LASER_PIN     A5

Microphone* mic;
Timer* renderTimer = new Timer(20);

void startup() {
  mic = new Microphone();
}

void setup() {
  Serial.begin(9600);
  // charging pin
  pinMode (13, OUTPUT);
  startup();
}

void loop() {
  // fast charge mode
  digitalWrite(13, LOW);

  if (renderTimer->trigger()) {
    float micLvl = mic->read();
    int laserLevel = 255 - (255 * micLvl);
    analogWrite(LASER_PIN, laserLevel);
  }
}
