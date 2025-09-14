#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

class FireEffect {
public:
    FireEffect(Adafruit_NeoPixel* strip, int width, int height);

    // Call every frame: level from mic (0..1), ax/ay from accelerometer
    void update(float level, float ax, float ay);

    // Draw the current heat buffer to the LEDs
    void render();

private:
    Adafruit_NeoPixel* strip;
    int width, height;

    // Heat buffer (max 32x32 so it fits your 16x8)
    uint8_t heat[32][32];

    // Map (x,y) to physical LED index on your top-wired matrix
    int XY(int x, int y);

    // Map a heat value (0..255) to a fire color
    uint32_t heatToColor(uint8_t t);
};
