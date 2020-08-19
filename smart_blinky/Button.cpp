#include "Arduino.h"
#include "Button.h"

const int PressLength = 3000;

Button:: Button(int pin) {
    pinMode(pin, INPUT);
    _pin = pin;
    lastState = false;
}

bool Button::isShortPress() {
    return shortPress;
}

bool Button::isLongPress() {
    return longPress;
}

void Button::read() {
    int buttonState = digitalRead(_pin);
      unsigned long downNow = millis();
      if (buttonState == HIGH) {
        if (!lastState) {
          lastState = true;
          downPress = downNow;
        }
        if (downNow - downPress > PressLength) {
          longPress = true;
        }
      } else {
        if (longPress) {
          longPress = false;
        }
        if (lastState && downNow - downPress) {
          shortPress = true;
        } else if (!lastState && shortPress) {
          shortPress = false;
        }
        if (lastState) {
          lastState = false;
        }
      }
}