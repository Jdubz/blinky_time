#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "FireEffect.h"

class SerialConsole {
public:
    SerialConsole(FireEffect &fire, Adafruit_NeoPixel &leds);

    void begin();
    void update();
    void handleCommand(const char *cmd);

    void restoreDefaults();
    void printAll();

private:
    FireEffect &fire;
    Adafruit_NeoPixel &leds;

    // ---- AdaptiveMic debug tool ----
    bool micDebugEnabled = false;         // toggled by "mic debug on/off"
    bool micDebugCsv     = false;         // "mic debug csv on/off"
    unsigned long micDebugPeriodMs = 200; // "mic debug rate <ms>"
    unsigned long micDebugLastMs   = 0;

    void micDebugTick();                  // periodic printer
    void micDebugPrintLine();             // one line snapshot
};

#endif
