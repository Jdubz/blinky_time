#pragma once
#include <Arduino.h>
#include "FireEffect.h"
#include "AdaptiveMic.h"

// Shared audio parameters that the main loop uses.
struct AudioParams {
  float noiseGate;   // e.g. 0.06
  float gamma;       // e.g. 0.60
  float globalGain;  // e.g. 1.35
  float attackTau;   // seconds, e.g. 0.08
  float releaseTau;  // seconds, e.g. 0.30
};

class SerialConsole {
public:
  // maxRows constrains "sparkrows"
  SerialConsole(FireEffect* fire, AudioParams* audio, uint8_t maxRows, AdaptiveMic* mic);

  void begin();   // call from setup()
  void update();  // call each loop()

  void restoreDefaults();
  void printAll();

private:
  FireEffect*  fire;
  AudioParams* audio;
  AdaptiveMic* mic;
  uint8_t      maxRows;

  static const uint8_t kBufSize = 96;
  char   buf[kBufSize];
  uint8_t len = 0;

  unsigned long lastStatusMs = 0;

  void printHelp();
  bool readLine();
  void handleCommand(const char* line);
  static void toLowerInPlace(char* s);
};
