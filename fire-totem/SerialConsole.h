#pragma once
#include <Arduino.h>

// Forward declarations to avoid heavy includes
class FireEffect;
class AdaptiveMic;

// Minimal SerialConsole aligned to your sketch needs:
// - Constructor does NOT require AudioParams
// - begin() takes NO args
// - No tick() method
class SerialConsole {
public:
  SerialConsole(FireEffect* fire, uint8_t maxRows, AdaptiveMic* mic)
    : fire_(fire), maxRows_(maxRows), mic_(mic) {}

  void begin();   // call from setup()
  void service(); // optional

private:
  FireEffect*  fire_;
  uint8_t      maxRows_;
  AdaptiveMic* mic_;
};
