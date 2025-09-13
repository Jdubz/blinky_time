#pragma once
#include <Arduino.h>

class SimplexNoise {
public:
    SimplexNoise();

    // 1D, 2D, and 3D simplex noise
    float noise(float x);
    float noise(float x, float y);
    float noise(float x, float y, float z);

private:
    uint8_t perm[512];
    static const uint8_t permTable[256];

    static float grad(int hash, float x, float y, float z);
    static float fade(float t);
    static float lerp(float a, float b, float t);
};
