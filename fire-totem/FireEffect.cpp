#include "FireEffect.h"

FireEffect::FireEffect(Adafruit_NeoPixel* strip, int width, int height)
: strip(strip), width(width), height(height) {
  // Clear
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++)
      heat[y][x] = 0.0f;
}

// Linear mapping, **top-wired** panel: flip Y so y=0 draws to top row.
int FireEffect::XY(int x, int y) {
  x = (x % width + width) % width;
  y = (y % height + height) % height;
  int physY = (height - 1) - y;   // flip vertically
  return physY * width + x;
}

// Convert heat (0..255) to fire color, modulated by bias (0..1) for color temperature
uint32_t FireEffect::heatToColor(float temperature, float bias) {
  if (temperature < 0) temperature = 0;
  if (temperature > 255) temperature = 255;

  // Map bias (music intensity) → hotter (more yellow/white) vs cooler (red)
  // Clamp to [0,1]
  bias = constrain(bias, 0.0f, 1.0f);

  // Base "Doom fire" palette mapping with a white-hot push from bias
  int t192 = (int)(temperature / 255.0f * 191.0f);
  uint8_t ramp = (t192 & 63) << 2;

  // Blend in whiteness proportional to bias
  uint8_t r, g, b;
  if (t192 > 128) {          // hottest
    r = 255;
    g = (uint8_t)constrain(255.0f * (0.7f + 0.3f * bias), 0, 255);
    b = (uint8_t)constrain(ramp * (0.2f + 0.8f * bias), 0, 255);
  } else if (t192 > 64) {    // medium
    r = 255;
    g = (uint8_t)constrain(ramp * (0.6f + 0.4f * bias), 0, 255);
    b = 0;
  } else {                   // coolest
    r = ramp;
    g = 0;
    b = 0;
  }
  return strip->Color(r, g, b);
}

void FireEffect::update(float micEnv, float micRMS, float ax, float ay) {
  // --------------- 1) Fuel at bottom from noise + music ---------------
  // Time for evolving noise
  const float t = millis() * 0.0015f;

  // Intensity bias for color temperature & overall energy:
  // combine envelope (punch) and RMS (body)
  float intensity = constrain(0.25f + 0.7f * micEnv + 0.3f * micRMS, 0.0f, 1.5f);
  // Smooth bias for nicer color transitions
  lastIntensity = lastIntensity * 0.85f + intensity * 0.15f;

  for (int x = 0; x < width; x++) {
    // Minimal noise in [-1,1] → [0,1]
    float n = (noiseGen.noise(x * 0.28f, t) + 1.0f) * 0.5f;
    // Base fuel: always some flame; scale by loudness (RMS) and add transient punch (Env)
    float base = (0.15f + 0.85f * n) * (0.3f + 1.2f * micRMS + 0.6f * micEnv);
    float h = constrain(base * 255.0f, 0.0f, 255.0f);
    heat[height - 1][x] = h;
  }

  // --------------- 2) Vertical “flare” from vertical movement (Δay) ---------------
  float day = ay - lastAy; lastAy = ay;
  float flare = constrain(fabs(day) * 12.0f, 0.0f, 3.0f); // shake up/down → taller fire
  if (flare > 0.001f) {
    for (int x = 0; x < width; x++) {
      heat[height - 1][x] = min(255.0f, heat[height - 1][x] * (1.0f + flare));
    }
  }

  // --------------- 3) Upward diffusion + cooling ---------------
  // (y=height-1 is bottom; propagate up toward y=0)
  for (int y = 0; y < height - 1; y++) {
    for (int x = 0; x < width; x++) {
      int below = y + 1;
      float cooling = random(0, 12);           // tweak for flame height
      float newHeat = heat[below][x] - cooling;
      if (newHeat < 0) newHeat = 0;
      // Blend current cell with feed from below
      heat[y][x] = 0.45f * heat[y][x] + 0.55f * newHeat;
    }
  }

  // --------------- 4) Horizontal slosh from horizontal movement (Δax) ---------------
  float dax = ax - lastAx; lastAx = ax;
  // Add impulse to horizontal velocity; damping for “liquid” feel
  vx += dax * 6.5f;   // sensitivity
  vx *= 0.86f;        // damping

  int shift = (int)round(vx);
  if (shift != 0) {
    for (int y = 0; y < height; y++) {
      float row[32];
      for (int x = 0; x < width; x++) row[x] = heat[y][x];
      for (int x = 0; x < width; x++) {
        int sx = (x - shift) % width;
        if (sx < 0) sx += width;
        heat[y][x] = row[sx];
      }
    }
  }
}

void FireEffect::render() {
  // Use lastIntensity as color temperature bias
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      strip->setPixelColor(XY(x, y), heatToColor(heat[y][x], constrain(lastIntensity, 0.0f, 1.0f)));
    }
  }
  strip->show();
}
