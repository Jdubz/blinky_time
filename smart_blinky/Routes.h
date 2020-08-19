#ifndef Routes_h
#define Routes_h

#include <ESP8266WebServer.h>

#include "ROM.h"
#include "Light.h"

class Routes {
  public:
    Routes(Light light, ROM rom);
    void status();
    void on();
    void color();

  private:
    ESP8266WebServer _server;
    Light _light;
    ROM _rom;
}

#endif
