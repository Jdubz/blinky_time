#include "Arduino.h"
#include "Routes.h"

#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoJson.h>

const int PORT = 80;


Routes::Routes() {
  ESP8266WebServer server(HTTP_REST_PORT);
}