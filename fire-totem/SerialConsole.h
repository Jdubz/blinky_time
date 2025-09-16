#pragma once
#include <Arduino.h>
#include "FireEffect.h"

class SerialConsole {
public:
  SerialConsole(FireParams* params = nullptr) : p(*params) {}
  void begin(FireParams* params);
  void tick();
  void restoreDefaults();
  void printAll();
private:
  FireParams& p;
  bool handleSet(const char* key, float value);
};
