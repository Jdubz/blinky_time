#include "FireEffect.h"
#include <math.h>

FireEffect::FireEffect(Adafruit_NeoPixel* strip, int width, int height)
: strip(strip), width(width), height(height) {
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++)
      heat[y][x] = 0;

    // --- Preheat: seed with random mid-level heat so fire is visible at startup ---
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      heat[y][x] = random(50, 180);  // seed with warm starting values
    }
  }
}

// Map XY to linear index (top row is y=0 in buffer, physical top)
int FireEffect::XY(int x, int y) {
  x = (x + width) % width;
  y = (y + height) % height;
  return y * width + x;
}

// Convert heat (0–255) to fire color
uint32_t FireEffect::heatToColor(uint8_t t) {
  if (t > 255) t = 255;
  uint8_t t192 = (uint16_t)t * 191 / 255;
  uint8_t ramp = (t192 & 63) << 2;

  if (t192 > 128)
    return strip->Color(255, 255, ramp);
  else if (t192 > 64)
    return strip->Color(255, ramp, 0);
  else
    return strip->Color(ramp, 0, 0);
}

// Simple smooth noise (pseudo-Perlin 1D)
float smoothNoise1D(float x) {
  int xi = (int)floor(x);
  float xf = x - xi;
  float r1 = sinf((xi * 12.9898f) * 43758.5453f);
  float r2 = sinf(((xi+1) * 12.9898f) * 43758.5453f);
  float n1 = r1 - floor(r1);
  float n2 = r2 - floor(r2);
  return n1 * (1.0f - xf) + n2 * xf;
}

void FireEffect::update(float level, float dx, float dy) {
  (void)dx; (void)dy; // not using motion yet
  level = constrain(level, 0.0f, 1.0f);

  // Ensure some base activity even when silent
  const float base = 0.2f;
  float intensity = base + level * (1.0f - base);

  // 1) Inject fire at bottom row
  for (int x = 0; x < width; x++) {
    int chance = random(0, 100);
    if (chance < 60) {
      // louder audio → stronger fuel
      uint8_t fuel = (uint8_t)(random(50, 180) * intensity);
      heat[height - 1][x] = fuel;
    } else {
      heat[height - 1][x] = 0; // empty gaps for flicker
    }
  }

  // 2) Doom-style upward propagation
  // louder audio → less cooling → taller fire
  int minCool = (int)(40 - level * 25); // quiet = 40..70, loud = ~15..40
  int maxCool = (int)(70 - level * 35);

  for (int y = 0; y < height - 1; y++) {
    for (int x = 0; x < width; x++) {
      int srcX = (x + random(-1, 2) + width) % width;
      uint8_t below = heat[y+1][srcX];
      int cooling = random(minCool, maxCool);

      int val = (int)below - cooling;
      if (val < 0) val = 0;

      heat[y][x] = (uint8_t)val;
    }
  }
}

void FireEffect::render() {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      strip->setPixelColor(XY(x, y), heatToColor(heat[y][x]));
    }
  }
  strip->show();
}
