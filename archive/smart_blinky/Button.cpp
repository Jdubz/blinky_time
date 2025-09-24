#include "Arduino.h"
#include "Button.h"

Button::Button(const int pin) {
  pinMode(pin, INPUT);
  _pin = pin;
  _lastState = false;
}

bool Button::isShortPress() {
  return _shortPress;
}

bool Button::isLongPress() {
  return _longPress;
}

void Button::read() {
  int PressLength = 3000;
  int buttonState = digitalRead(_pin);
  unsigned long downNow = millis();

  if (buttonState == HIGH) {
    if (!_lastState) {
      _downPress = downNow;
    }
    if ((downNow - _downPress) > PressLength) {
      _longPress = true;
      Serial.println("long press");
    }

  } else if (buttonState == LOW) {
    if (_lastState && !_longPress) {
      _shortPress = true;
      Serial.println("short press");
    } else if (!_lastState && _shortPress) {
      _shortPress = false;
    }
    if (_longPress) {
      _longPress = false;
    }
  }

  _lastState = (buttonState == HIGH);
}