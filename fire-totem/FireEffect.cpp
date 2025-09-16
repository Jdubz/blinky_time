#include "FireEffect.h"

// ---- Constructor ----
FireEffect::FireEffect(Adafruit_NeoPixel* strip, int width, int height)
: strip(strip), width(width), height(height) {
  heat = (uint8_t*)malloc((size_t)width * height);
  if (heat) {
    // Start cool (no preheat) for deterministic boot
    memset(heat, 0, (size_t)width * height);
  }
}

// ---- Doom-style update (restored, tuned for 8-high) ----
void FireEffect::update(float level, float dx, float dy) {
  (void)dx; (void)dy;
  // Keep parameter for API, but ensure fire still lives at silence
  if (level < 0.0f) level = 0.0f;
  if (level > 1.0f) level = 1.0f;

  const float base = 0.20f;                      // baseline activity
  const float intensity = base + level * (1.0f - base);

  // 1) Inject fuel along the *bottom* row (y = height-1)
  for (int x = 0; x < width; x++) {
    // 60% chance of fuel → more dark gaps
    if (random(0, 100) < 60) {
      uint8_t fuel = (uint8_t)(random(50, 180) * intensity);
      heat[idx(x, height - 1)] = fuel;
    } else {
      heat[idx(x, height - 1)] = 0;
    }
  }

  // 2) Propagate upward with cooling (classic Doom)
  //    y = 0 is the TOP; we pull from row below (y+1) so flames rise
  const int minCool = 30; // stronger cooling keeps height low
  const int maxCool = 70;

  for (int y = 0; y < height - 1; y++) {
    for (int x = 0; x < width; x++) {
      // Horizontal drift: sample from one of the three below pixels
      int srcX = (x + random(-1, 2) + width) % width; // wrap in X
      uint8_t below = heat[idx(srcX, y + 1)];

      int cooled = (int)below - random(minCool, maxCool);
      if (cooled < 0) cooled = 0;

      heat[idx(x, y)] = (uint8_t)cooled;
    }
  }
}

// ---- Render with 50% brightness cap ----
void FireEffect::render() {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t h = heat[idx(x, y)];
      strip->setPixelColor(idx(x, y), heatToColor(h));
    }
  }
  strip->show();
}

// Simple red→yellow palette (no blue), capped at 50% brightness
uint32_t FireEffect::heatToColor(uint8_t h) const {
  // Map heat (0..255) to (r,g,b) in a warm palette
  // Split into three bands for a fire-like gradient
  uint16_t r=0, g=0, b=0;

  if (h <= 80) {                  // dark → dim red
    r = h * 3;                    // up to ~240
    g = h >> 2;                   // subtle ember glow
  } else if (h <= 170) {          // red → orange/yellow
    r = 240 + (h - 80);           // up to ~255
    g = (h - 80) * 2;             // up to ~180
  } else {                        // bright yellow tip
    r = 255;
    g = 180 + (h - 170) * 3 / 2;  // up to ~255
  }
  if (g > 255) g = 255;

  // Hard cap overall brightness to ~50%
  r >>= 1; g >>= 1; b >>= 1;      // divide by 2

  return strip->Color((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// ---- Debug helpers ----
float FireEffect::getAverageHeat() const {
  uint32_t sum = 0;
  const int n = width * height;
  for (int i = 0; i < n; i++) sum += heat[i];
  return (float)sum / (float)n;   // 0..255 range
}

int FireEffect::getActiveCount(uint8_t thresh) const {
  int c = 0;
  const int n = width * height;
  for (int i = 0; i < n; i++) if (heat[i] > thresh) c++;
  return c;
}
