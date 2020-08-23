#include "Arduino.h"
#include "LED.h"

LED::LED(const int pin) {
  pinMode(pin, OUTPUT);
  _pin = pin;
  _isOn = false;
  digitalWrite(_pin, LOW);
}

void LED::on() {
  if (!_isOn) {
    digitalWrite(_pin, HIGH);
    _isOn = true;
  }
}

void LED::off() {
  if (_isOn) {
    digitalWrite(_pin, LOW);
    _isOn = false;
  }
}

void LED::toggle() {
  if (_isOn) {
    off();
  } else {
    on();
  }
}