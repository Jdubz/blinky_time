#pragma once
#include <Adafruit_NeoPixel.h>

class FireEffect {
public:
    FireEffect(int w, int h, Adafruit_NeoPixel &strip);
    ~FireEffect();

    void begin();
    void update(float micLevel, float micRMS, float accelX);
    void render();

private:
    int width, height;
    Adafruit_NeoPixel &strip;
    float **heat;

    int XY(int x, int y);
    uint32_t heatToColor(float temperature);
};
