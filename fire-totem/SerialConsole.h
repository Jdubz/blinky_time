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
    void renderIMUVisualization();        // IMU orientation visualization

    // Motion control access
    bool motionEnabled = true;
    float windSpeed = 2.0f;              // Wind lean speed multiplier
    float windScale = 0.3f;              // Wind intensity scale

    // IMU visualization mode
    bool imuVizEnabled = false;          // Enable IMU visualization on matrix
    bool fireDisabled = false;           // Disable fire when showing IMU viz

private:
    FireEffect &fire;
    Adafruit_NeoPixel &leds;

    // ---- Debug systems ----
    bool micDebugEnabled = false;         // toggled by "mic debug on/off"
    unsigned long micDebugPeriodMs = 200; // "mic debug rate <ms>"
    unsigned long micDebugLastMs   = 0;

    bool debugEnabled = false;            // General debug output
    unsigned long debugPeriodMs = 500;    // General debug rate
    unsigned long debugLastMs = 0;

    void micDebugTick();                  // periodic printer
    void micDebugPrintLine();             // one line snapshot
    void debugTick();                     // general debug output
    void printFireStats();               // fire engine statistics
};

#endif
