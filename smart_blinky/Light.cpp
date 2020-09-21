#include <Arduino.h>
#include <ArduinoJson.h>

#include "Light.h"
#include "Color.h"
#include "config.h"

Light::Light(const int Rpin, const int Gpin, const int Bpin) {
  _rpin = Rpin;
  pinMode(Rpin, OUTPUT);
  _gpin = Gpin;
  pinMode(Gpin, OUTPUT);
  _bpin = Bpin;
  pinMode(Bpin, OUTPUT);

  _isOn = false;
  _lastFade = millis();
}

void Light::changeColor(color RGB) {
  _RGB = RGB;
  Serial.print("changing color to: ");
  Serial.print(RGB.R);
  Serial.print(".");
  Serial.print(RGB.G);
  Serial.print(".");
  Serial.println(RGB.B);
}

void Light::_showColor(color RGB) {
  analogWrite(_rpin, RGB.R);
  analogWrite(_gpin, RGB.G);
  analogWrite(_bpin, RGB.B);
}

void Light::update() {
  if (_shouldFade()) {
    color targetColor = _getTargetColor();

    color newColor;
    newColor.R = _getNextColor(_RGBNow.R, targetColor.R);
    newColor.G = _getNextColor(_RGBNow.G, targetColor.G);
    newColor.B = _getNextColor(_RGBNow.B, targetColor.B);

    _showColor(newColor);
    _RGBNow = newColor;
  }
}

char* Light::getState() {
  DynamicJsonBuffer dynamicJsonBuffer;
  JsonObject& root = dynamicJsonBuffer.createObject();
  root["state"] = _isOn ? MQTT_STATE_ON_PAYLOAD : MQTT_STATE_OFF_PAYLOAD;
  root["brightness"] = _brightness;
  JsonObject& color = root.createNestedObject("color");
  color["r"] = _RGB.R;
  color["g"] = _RGB.G;
  color["b"] = _RGB.B;
  root.printTo(_stateBuffer, sizeof(_stateBuffer));
  return _stateBuffer;
}

void Light::on() {
  if (!_isOn) {
    _isOn = true;
    Serial.println("turning light on");
  }
}

void Light::off() {
  if (_isOn) {
    _isOn = false;
    Serial.println("turning light off");
  }
}

void Light::toggle() {
  _isOn = !_isOn;
}

void Light::setBrightness(byte brightness) {
  _brightness = brightness;
}

bool Light::_shouldFade() {
  int fadeTime = 30;
  unsigned long now = millis();
  bool isNextFrame = now - _lastFade >= fadeTime;
  if (isNextFrame) {
    _lastFade = now;
  }
  color targetColor = _getTargetColor();

  bool shouldFade = !(_RGBNow.R == targetColor.R && _RGBNow.G == targetColor.G && _RGBNow.B == targetColor.B);
  
  return shouldFade && isNextFrame;
}

int Light::_getNextColor(byte now, byte end) {
  int fadeSpeed = 1;
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

color Light::_getTargetColor() {
  color targetColor;
  targetColor.R = 0;
  targetColor.G = 0;
  targetColor.B = 0;
  if (_isOn) {
    targetColor.R = (_RGB.R * _brightness) / 255;
    targetColor.G = (_RGB.G * _brightness) / 255;
    targetColor.B = (_RGB.B * _brightness) / 255;
  }

  return targetColor;
}