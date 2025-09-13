#pragma once
#include <Adafruit_NeoPixel.h>
#include "SimplexNoise.h"

class FireEffect {
public:
    FireEffect(uint8_t width, uint8_t height, Adafruit_NeoPixel& strip);

    void begin();
    void update(float micLevel, float micRMS, float tiltX);
    void render();

    void setCooling(uint8_t c) { cooling = c; }
    void setSparking(uint8_t s) { sparking = s; }
    void setNoiseSpeed(float speed) { noiseSpeed = speed; }

private:
    int XY(int x, int y);
    uint32_t heatColor(uint8_t temperature, float intensity);

    uint8_t WIDTH, HEIGHT;
    Adafruit_NeoPixel& strip;
    SimplexNoise noise;

    uint8_t* heat;
    float noiseTime = 0;
    float tiltOffset = 0;

    uint8_t cooling = 55;
    uint8_t sparking = 120;
    float noiseSpeed = 0.04;
};
