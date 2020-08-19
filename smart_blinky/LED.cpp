#include "Arduino.h"
#include "LED.h"

LED::LED(int pin) {
  pinMode(pin, OUTPUT);
  _pin = pin;
  isOn = false;
}

void LED::on() {
  if (isOn) {
    digitalWrite(_pin, HIGH);
    isOn = true;
  }
}

void LED::off() {
  if (this->isOn) {
    digitalWrite(_pin, LOW);
    isOn = false;
  }
}

void LED::toggle() {
  if (isOn) {
    *this.off();
  } else {
    *this.on();
  }
}