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

    void drawTopRowVU();

private:
    FireEffect &fire;
    Adafruit_NeoPixel &leds;   // hold a reference, no more extern
};

#endif
