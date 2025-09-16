#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\SerialConsole.h"
#pragma once
#include <Arduino.h>
class FireEffect; class AdaptiveMic;
class SerialConsole {
public:
  SerialConsole(FireEffect* fire, uint8_t maxRows, AdaptiveMic* mic)
    : fire_(fire), maxRows_(maxRows), mic_(mic) {}
  void begin(); void service();
private:
  FireEffect* fire_; uint8_t maxRows_; AdaptiveMic* mic_;
};
