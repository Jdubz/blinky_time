#include "FireEffect.h"
#include "TotemDefaults.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

FireEffect::FireEffect(Adafruit_NeoPixel* strip, int width, int height)
: strip(strip), width(width), height(height), heat(nullptr) {
  size_t n = (size_t)width * (size_t)height;
  heat = (uint8_t*)malloc(n);
  if (heat) memset(heat, 0, n);

  // Pull defaults from a single place
  params.baseCooling         = Defaults::BaseCooling;
  params.sparkHeatMin        = Defaults::SparkHeatMin;
  params.sparkHeatMax        = Defaults::SparkHeatMax;
  params.sparkChance         = Defaults::SparkChance;
  params.audioSparkBoost     = Defaults::AudioSparkBoost;
  params.audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
  params.coolingAudioBias    = Defaults::CoolingAudioBias;
  params.bottomRowsForSparks = Defaults::BottomRowsForSparks;
}


void FireEffect::setParams(const FireParams& p) { params = p; }
FireParams FireEffect::getParams() const { return params; }

void FireEffect::update(float level, float dx, float dy) {
  (void)dx; (void)dy;

  if (!isfinite(level)) level = 0.0f;
  level = constrain(level, 0.0f, 1.0f);

  // Cooling (lower value => taller flames). Audio bias reduces cooling on loud parts.
  const int cooling = max(0, (int)params.baseCooling + (int)(params.coolingAudioBias * level));

  // Per-cell cooling
  for (int x = 0; x < width; x++) {
    for (int y = 0; y < height; y++) {
      uint16_t i = idx(x,y);
      uint8_t h  = heat[i];
      if (h) {
        uint8_t cool = random(0, cooling + 1);
        heat[i] = (h > cool) ? (h - cool) : 0;
      }
    }
  }

  // Rise/diffuse upward with X-wrap sampling of three cells below
  for (int x = 0; x < width; x++) {
    for (int y = 0; y < height - 1; y++) {
      int xL = (x == 0) ? (width - 1) : (x - 1);
      int xR = (x == width - 1) ? 0 : (x + 1);
      uint16_t below      = idx(x,   y+1);
      uint16_t belowLeft  = idx(xL,  y+1);
      uint16_t belowRight = idx(xR,  y+1);
      uint8_t avg = (uint8_t)(((int)heat[below] + (int)heat[belowLeft] + (int)heat[belowRight]) / 3);
      uint8_t decay = random(0, 8); // flicker as it rises
      int val = (int)avg - (int)decay;
      if (val < 0) val = 0;
      heat[idx(x,y)] = (uint8_t)val;
    }
  }

  // Bottom sparks driven by audio
  const float    chance  = params.sparkChance + (params.audioSparkBoost * level);
  const uint8_t  addHeat = (uint8_t)min<int>(params.audioHeatBoostMax, (int)(params.audioHeatBoostMax * level));
  const int      bottomStart = max(0, height - (int)params.bottomRowsForSparks);

  for (int x = 0; x < width; x++) {
    if (random(1000) < (int)(chance * 1000.0f)) {
      for (int y = bottomStart; y < height; y++) {
        uint8_t base = random(params.sparkHeatMin, (int)params.sparkHeatMax + 1);
        int v = (int)base + (int)addHeat;
        if (v > 255) v = 255;
        uint16_t i = idx(x,y);
        if (v > heat[i]) heat[i] = (uint8_t)v;
      }
    }
  }
}

void FireEffect::render() {
  const int n = width * height;
  for (int i = 0; i < n; i++) {
    strip->setPixelColor(i, heatToColor(heat[i]));
  }
  strip->show();
}

// Palette: black -> red -> orange -> yellow -> white (blue-ish tip), 50% cap
uint32_t FireEffect::heatToColor(uint8_t h) const {
  uint8_t r=0,g=0,b=0;
  if (h <= 85) {
    r = h * 3;
  } else if (h <= 170) {
    r = 255;
    g = (h - 85) * 3;
  } else {
    r = 255; g = 255;
    b = (h - 170) * 3 / 2;
  }
  // global 50% brightness cap (Stable V1 rule)
  r >>= 1; g >>= 1; b >>= 1;
  return strip->Color(r, g, b);
}

// ---- Telemetry helpers (definitions MUST match header) ----
float FireEffect::getAverageHeat() const {
  uint32_t sum = 0;
  const int n = width * height;
  for (int i = 0; i < n; i++) sum += heat[i];
  return (float)sum / (float)n; // 0..255
}

int FireEffect::getActiveCount(uint8_t thresh) const {
  int c = 0;
  const int n = width * height;
  for (int i = 0; i < n; i++) if (heat[i] > thresh) c++;
  return c;
}
