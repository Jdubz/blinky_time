#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <Arduino.h>
#include "FireEffect.h"

class SerialConsole {
public:
    SerialConsole(FireEffect &fire);

    void begin();
    void update();
    void handleCommand(const char *cmd);

    void restoreDefaults();
    void printAll();

    void drawTopRowVU();

private:
    FireEffect &fire;
};

#endif
