#ifndef WifiController_h
#define WifiController_h

#include "LED.h"

class WifiController {
  public:
    WifiController(LED* led);
    void setup(String SSID, String PW);
    bool checkConnection();
    String getIp();
    bool connect();
    bool isConnected();

  private:
    LED* _led;
    String _SSID;
    String _PW;
};

#endif
