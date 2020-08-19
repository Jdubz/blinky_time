#include "Arduino.h"
#include "Routes.h"

#include <ESP8266WebServer.h>
#include <ArduinoJson.h>

#include "Light.h"
#include "Color.h"
#include "ROM.h"

const int PORT = 80;

Routes::Routes(Light light, ROM rom) {
  _light = light;
  _rom = rom;

  _server = ESP8266WebServer(HTTP_REST_PORT);

  _server.on("/", HTTP_GET, _status);
  _server.on("/on", HTTP_POST, _on);
  _server.on("/color", HTTP_POST, _color);

  _server.begin();
}

void Routes::handleClient() {
  _server.handleClient();
}

void Routes::_status() {
  Serial.print('Get /');
  Serial.println(_server.arg('plain'));
}

void Routes::_on() {
  Serial.print('Post /on :');
  Serial.println(_server.arg('plain'));
}

void Routes::_color() {
  Serial.print('Post /color :');
  Serial.println(_server.arg('plain'));
}
