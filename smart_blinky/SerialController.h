#ifndef SerialController_h
#define SerialController_h

#include "ROM.h"

class SerialController {
  public:
    SerialController(Rom rom);
    bool read();

  private:
    ROM _rom;
}

#endif