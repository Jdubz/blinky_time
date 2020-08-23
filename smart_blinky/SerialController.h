#ifndef SerialController_h
#define SerialController_h

#include "ROM.h"
#include "WifiController.h"

class SerialController {
  public:
    SerialController(ROM* rom, WifiController* wifi);
    bool read();

  private:
    ROM* _rom;
    WifiController* _wifi;
};

#endif