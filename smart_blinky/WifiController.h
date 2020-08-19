#ifndef WifiController_h
#define WifiController_h

#include "LED.h"

class WifiController {
  public:
    WifiController(LED led);
    void setup(const char* SSID, const char* PASSWORD);
    bool checkConnection();

  private:
    bool _isConnected();
    bool _connect();
    LED _led;
    char _SSID;
    char _PW;
};

#endif
