#include "Arduino.h"
#include "Routes.h"

#include <ESP8266WebServer.h>
#include "Light.h"
#include "Color.h"
#include "ROM.h"
#include "MQTT.h"

Routes::Routes(Light* light, ROM* rom, MQTT* mqtt) {
  _light = light;
  _rom = rom;

  int PORT = 80;
  ESP8266WebServer _server(PORT);
}

void Routes::setup() {
  _server.on("/", HTTP_GET, std::bind(&Routes::_status, this));
  _server.on("/on", HTTP_POST, std::bind(&Routes::_on, this));
  _server.on("/color", HTTP_POST, std::bind(&Routes::_color, this));

  _server.begin();
}

void Routes::handleClient() {
  _server.handleClient();
}

void Routes::_status() {
  Serial.println("Get /");
  struct color lightColor = _light->getColor();
  bool state = _light->getState();
  byte brightness = _light->getBrightness();

  String statusJson = "{\"on\":" + String(state);
  statusJson += ",\"R\":" +  String(lightColor.R);
  statusJson += ",\"G\":" +  String(lightColor.G);
  statusJson += ",\"B\":" +  String(lightColor.B);
  statusJson += ",\"brightness\":" + String(brightness) + "}";

  _server.send(200, "text/json", statusJson);
}

void Routes::_on() {
  Serial.print("Post /on :");
  Serial.println(_server.arg("on"));
  bool on = _server.arg("on") == "true";

  if (on) {
    _light->on();
  } else {
    _light->off();
  }

  _rom->writeState(on);
  _mqtt->publishState(on);

  _server.send(200, "text/json", "{success:true}");
}

void Routes::_color() {
  String R = _server.arg("R");
  String G = _server.arg("G");
  String B = _server.arg("B");

  Serial.println("Post /color : " + R + "." + G + "." + B);

  _server.arg("plain");

  struct color newColor;
  newColor.R = R.toInt();
  newColor.G = G.toInt();
  newColor.B = B.toInt();

  _light->changeColor(newColor);
  _rom->writeColor(newColor);
  _mqtt->publishColor(newColor);

  _server.send(200, "text/json", "{success:true}");
}
