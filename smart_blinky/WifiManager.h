#ifndef WifiManager_h
#define WifiManager_h

#include "LED.h"

class WifiManager {
  public:
    WifiManager(LED led);
    bool connect(const char* SSID, const char* PASSWORD);
    bool connected();

  private:
    LED _led;
};

#endif
