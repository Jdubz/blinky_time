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

  server = ESP8266WebServer(HTTP_REST_PORT);

  server.on("/", HTTP_GET, status);
  server.on("/on", HTTP_POST, on);
  server.on("/color, HTTP_POST", color);

  server.begin();
}

void Routes::status() {

}

void Routes::on() {
  StaticJsonBuffer<500> jsonBuffer;
  String post_body = http_rest_server.arg("plain");
  Serial.println(post_body);

  
}

void Routes::color() {

}
