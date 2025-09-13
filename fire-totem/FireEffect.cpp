#include "FireEffect.h"
#include "SimplexNoise.h"

static SimplexNoise noiseGen;

FireEffect::FireEffect(int w, int h, Adafruit_NeoPixel &strip)
    : width(w), height(h), strip(strip) {
    heat = new float*[height];
    for (int y = 0; y < height; y++) {
        heat[y] = new float[width];
        for (int x = 0; x < width; x++) heat[y][x] = 0;
    }
}

FireEffect::~FireEffect() {
    for (int y = 0; y < height; y++) delete[] heat[y];
    delete[] heat;
}

void FireEffect::begin() {
    strip.begin();
    strip.show();
}

// XY mapping for a *linear* top-wired matrix (first LEDs = top row)
int FireEffect::XY(int x, int y) {
    x = (x + width) % width;
    y = (height - 1) - ((y + height) % height);  // vertical flip
    return y * width + x;
}

void FireEffect::update(float micLevel, float micRMS, float accelX) {
    float time = millis() * 0.001f;

    // Add noise-based heat to bottom row (y = virtual bottom = height-1)
    for (int x = 0; x < width; x++) {
        float n = noiseGen.noise((float)x * 0.2f, time * 1.5f);
        float baseHeat = constrain((n + 1.0f) * 0.5f, 0, 1); // normalize 0-1
        baseHeat *= micRMS * 2.0f; // scale with mic loudness
        heat[height - 1][x] = baseHeat * 255; // store as 0-255
    }

    // Propagate heat upward with cooling
    for (int y = 0; y < height - 1; y++) {
        for (int x = 0; x < width; x++) {
            int below = (y + 1 < height) ? y + 1 : y;
            float cooling = random(0, 10); // tweak for faster/slower flames
            float newHeat = heat[below][x] - cooling;
            if (newHeat < 0) newHeat = 0;
            heat[y][x] = (heat[y][x] * 0.5f + newHeat * 0.5f);
        }
    }

    // Optional horizontal drift based on accelerometer X-axis
    float drift = accelX * 2.0f; // scale tilt sensitivity
    if (fabs(drift) > 0.05f) {
        for (int y = 0; y < height; y++) {
            float tempRow[32]; // enough for width <= 32
            for (int x = 0; x < width; x++) tempRow[x] = heat[y][x];
            for (int x = 0; x < width; x++) {
                int nx = (x + (int)round(drift) + width) % width;
                heat[y][x] = tempRow[nx];
            }
        }
    }
}

uint32_t FireEffect::heatToColor(float temperature) {
    temperature = constrain(temperature, 0, 255);

    // Convert heat value to color gradient: black -> red -> orange -> yellow -> white
    int t192 = (int)((temperature / 255.0f) * 191);

    uint8_t heatramp = t192 & 63;
    heatramp <<= 2;

    if (t192 > 128) {
        return strip.Color(255, 255, heatramp);
    } else if (t192 > 64) {
        return strip.Color(255, heatramp, 0);
    } else {
        return strip.Color(heatramp, 0, 0);
    }
}

void FireEffect::render() {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t color = heatToColor(heat[y][x]);
            strip.setPixelColor(XY(x, y), color);
        }
    }
    strip.show();
}
