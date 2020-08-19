#include "Arduino.h"
#include "Light.h"

#include "Color.h"

const byte fadeSpeed = 2;

Light::Light(int Rpin, int Gpin, int Bpin) {
  _rpin = Rpin;
  pinMode(Rpin, OUTPUT);
  _gpin = Gpin;
  pinMOde(Gpin, OUTPUT);
  _bpin = Bpin;
  pinMode(Bpin, OUTPUT);
}

void Light::changeColor(color RGB) {
  _RGB = RGB;
}

void Light::_showColor(color RGB) {
  analogWrite(_rpin, RGB.R);
  analogWrite(_gpin, RGB.G);
  analogWrite(_bpin, RGB.B);
}

void Light::fade() {
  if (_shouldFade()) {
    byte R = 0;
    byte G = 0;
    byte B = 0;
    if (_isOn) {
      R = _RGB.R;
      G = _RGB.G;
      B = _RGB.B;
    }

    color newColor;
    newColor.R = _getNextColor(_RGBNow.R, R);
    newColor.G = _getNextColor(_RGBNow.G, G);
    newColor.B = _getNextColor(_RGBNow.B, B);

    _showColor(newColor);
    _RGBNow = newColor;
  }
}

bool Light::status() {
  return _isOn;
}

void Light::on() {
  if (!_isOn) {
    _isOn = true;
  }
}

void Light::off() {
  if (_isOn) {
    _isOn = false;
  }
}

bool Light::_shouldFade() {
  byte R = 0;
  byte G = 0;
  byte B = 0;
  if (_isOn) {
    R = _RGB.R;
    G = _RGB.G;
    B = _RGB.B;
  }

  return !(_RGBNow.R == R && _RGBNow.G == G && _RGBNow.B == B);
}

int Light::_getNextColor(byte now, byte end) {
  int difference = end - now;
  int newColor;
  if (difference > 0) {
    newColor = now + fadeSpeed;
    if (newColor > end) {
      newColor = end;
    }
  } else {
    newColor = now - fadeSpeed;
    if (newColor < end) {
      newColor = end;
    }
  }

  return newColor;
}