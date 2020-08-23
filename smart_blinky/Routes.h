#ifndef Routes_h
#define Routes_h

#include <ESP8266WebServer.h>

#include "ROM.h"
#include "Light.h"
#include "Color.h"
#include "MQTT.h"

class Routes {
  public:
    Routes(Light* light, ROM* rom, MQTT* mqtt);
    void handleClient();
    void setup();
    struct color color();

  private:
    ESP8266WebServer _server;
    Light* _light;
    ROM* _rom;
    MQTT* _mqtt;

    void _status();
    void _on();
    void _color();
};

#endif
