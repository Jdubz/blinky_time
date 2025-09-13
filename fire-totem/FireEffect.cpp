#include "FireEffect.h"

FireEffect::FireEffect(uint8_t width, uint8_t height, Adafruit_NeoPixel& stripRef)
  : WIDTH(width), HEIGHT(height), strip(stripRef) {
    heat = new uint8_t[WIDTH * HEIGHT];
    memset(heat, 0, WIDTH * HEIGHT);
}

void FireEffect::begin() {
    strip.begin();
    strip.show();
}

int FireEffect::XY(int x, int y) {
    // Wrap around in X
    x = (x + WIDTH) % WIDTH;
    // Linear layout: row-major, no flipping
    return y * WIDTH + x;
}

uint32_t FireEffect::heatColor(uint8_t temperature, float intensity) {
    float colorBias = constrain(intensity, 0.0f, 1.0f);
    uint8_t t192 = (temperature * 191) / 255;
    uint8_t heatramp = (t192 & 63) << 2;

    uint8_t r = min(255, (uint8_t)(heatramp + 255 * colorBias));
    uint8_t g = (uint8_t)(colorBias * 255);
    uint8_t b = (uint8_t)(colorBias * 128);

    if (t192 > 128) return strip.Color(r, g, b);
    if (t192 > 64)  return strip.Color(r, g / 2, 0);
    return strip.Color(r / 2, 0, 0);
}

void FireEffect::update(float micLevel, float micRMS, float tiltX) {
    tiltOffset = tiltX * 3.0;

    // 1. Cool down
    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            uint8_t cooldown = random(0, ((cooling * 10) / HEIGHT) + 2);
            int idx = x + y * WIDTH;
            heat[idx] = (heat[idx] > cooldown) ? heat[idx] - cooldown : 0;
        }
    }

    // 2. Diffuse heat upward
    for (int x = 0; x < WIDTH; x++) {
        for (int y = HEIGHT - 1; y >= 2; y--) {
            int xL = (x - 1 + (int)round(tiltOffset) + WIDTH) % WIDTH;
            int xR = (x + 1 + (int)round(tiltOffset) + WIDTH) % WIDTH;
            int idx = x + y * WIDTH;
            heat[idx] = (heat[xL + (y - 1) * WIDTH] +
                         heat[x + (y - 1) * WIDTH] +
                         heat[xR + (y - 1) * WIDTH] +
                         heat[x + (y - 2) * WIDTH]) / 4;
        }
    }

    // 3. Add sparks (bottom row)
    noiseTime += noiseSpeed;
    for (int x = 0; x < WIDTH; x++) {
        float n = (noise.noise(x * 0.15f, noiseTime) + 1.0f) * 0.5f;
        if (n > 0.3) {
            int idx = x;
            int amount = (int)(n * 255 * micLevel);
            heat[idx] = constrain(heat[idx] + amount, 0, 255);
        }
    }
}

void FireEffect::render() {
    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            int idx = x + y * WIDTH;
            strip.setPixelColor(XY(x, y), heatColor(heat[idx], 1.0f));
        }
    }
    strip.show();
}
