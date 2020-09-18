#ifndef WifiController_h
#define WifiController_h

#include "LED.h"

class WifiController {
  public:
    WifiController(LED* led);
    void setup(ROM* rom);
    bool checkConnection();
    String getIp();
    bool connect();

  private:
    bool _isConnected();
    LED* _led;
    String _SSID;
    String _PW;
};

#endif
