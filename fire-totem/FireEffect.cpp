#include "FireEffect.h"
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <string.h>

FireEffect::FireEffect(Adafruit_NeoPixel* s, const FireParams& def): strip(s), p(def) {
  const int N = p.width * p.height;
  heat    = (uint8_t*)malloc(N);
  tmpHeat = (uint8_t*)malloc(N);
  vx      = (float*)malloc(N * sizeof(float));
  vy      = (float*)malloc(N * sizeof(float));
  memset(heat, 0, N);
  memset(tmpHeat, 0, N);
  memset(vx, 0, N * sizeof(float));
  memset(vy, 0, N * sizeof(float));
  lastMs = millis();
}

void FireEffect::restoreDefaults() {
  // Caller copies Defaults into p externally.
}

void FireEffect::update(float energy, float dx, float dy) {
  unsigned long now = millis();
  float dt = (now - lastMs) * 0.001f;
  if (dt < 0) dt = 0;
  if (dt > 0.05f) dt = 0.05f;
  lastMs = now;

  // keep for VU draw, but clamp for simulation
  lastEnergy = energy;
  float e = energy;
  if (e < 0.0f) e = 0.0f;
  if (e > 0.80f) e = 0.80f;

  addSparks(e);
  addForces(e, dt, dx, dy);
  advect(dt);
  diffuse();
  cool(e, dt);
}

void FireEffect::addSparks(float energy) {
  // spawn probability per column, audio-boosted but controlled
  const float chance = p.sparkChance + (p.audioSparkBoost * energy);
  const uint8_t rows = (p.bottomRowsForSparks == 0) ? 1 : p.bottomRowsForSparks;

  for (int x = 0; x < p.width; ++x) {
    float r = (float)rand() / (float)RAND_MAX;
    if (r < chance) {
      // pick a y within the bottom N rows
      int y = p.height - 1 - (rand() % rows);
      if (y < 0) y = 0;
      const int i = idx(x, y);

      // fuel: smaller, capped; still audio-reactive
      float add = p.sparkHeatMin + ((float)rand() / (float)RAND_MAX) * (p.sparkHeatMax - p.sparkHeatMin);
      add += energy * p.audioHeatBoostMax;
      if (add > 140.0f) add = 140.0f;    // cap injection

      int h = (int)heat[i] + (int)add;
      if (h > 240) h = 240;              // avoid constant bright yellow/white
      heat[i] = (uint8_t)h;

      // gentler initial nudge so it doesn’t rocket upward
      vx[i] += 0.2f * sinf((float)x * 0.7f);
      vy[i] -= 0.45f;
    }
  }
}

void FireEffect::addForces(float energy, float dt, float tiltX, float tiltY) {
  const int W = p.width;
  const int H = p.height;

  const float kx = (2.0f * PI) / (p.swirlScaleCells <= 0 ? 1.0f : p.swirlScaleCells);
  const float ky = (2.0f * PI) / (p.swirlScaleCells <= 0 ? 1.0f : p.swirlScaleCells);

  const float swirl = p.swirlAmp * (1.0f + p.swirlAudioGain * energy);
  const float updraft = p.updraftBase;

  // IMU tilt: push opposite gravity projection on XY
  const float tiltScale = p.buoyancy * 0.5f; // moderate
  const float tiltVX = -tiltX * tiltScale;
  const float tiltVY = -tiltY * tiltScale;

  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const int i = idx(x, y);

      float sx = sinf(kx * (float)x + 0.31f);
      float sy = cosf(ky * (float)y + 1.77f);

      float buoy = -p.buoyancy * (heat[i] / 255.0f); // hotter => up (negative vy)

      vx[i] += (swirl * sy + tiltVX) * dt;
      vy[i] += (buoy - updraft + swirl * sx + tiltVY) * dt;

      const float vmax = 12.0f;
      if (vx[i] >  vmax) vx[i] =  vmax;
      if (vx[i] < -vmax) vx[i] = -vmax;
      if (vy[i] >  vmax) vy[i] =  vmax;
      if (vy[i] < -vmax) vy[i] = -vmax;
    }
  }

  // viscosity (simple blur on velocity)
  if (p.viscosity > 0.0f) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const int i = idx(x, y);
        const int L = idx(x - 1, y);
        const int R = idx(x + 1, y);
        const int U = idx(x, y - 1);
        const int D = idx(x, y + 1);
        vx[i] = vx[i] * (1.0f - p.viscosity) + 0.25f * p.viscosity * (vx[L] + vx[R] + vx[U] + vx[D]);
        vy[i] = vy[i] * (1.0f - p.viscosity) + 0.25f * p.viscosity * (vy[L] + vy[R] + vy[U] + vy[D]);
      }
    }
  }

  if (p.velDamping > 0.0f && p.velDamping < 1.0f) {
    float d = powf(p.velDamping, dt * 60.0f); // normalize to ~60fps
    const int W = p.width, H = p.height;
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        int i = idx(x, y);
        vx[i] *= d;
        vy[i] *= d;
      }
    }
  }
}

void FireEffect::advect(float dt) {
  const int W = p.width;
  const int H = p.height;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const int i = idx(x, y);
      const float px = (float)x - vx[i] * dt;
      const float py = (float)y - vy[i] * dt;

      int x0 = (int)floorf(px);
      int y0 = (int)floorf(py);
      int x1 = x0 + 1;
      int y1 = y0 + 1;

      float fx = px - (float)x0;
      float fy = py - (float)y0;

      const uint8_t h00 = heat[idx(x0, y0)];
      const uint8_t h10 = heat[idx(x1, y0)];
      const uint8_t h01 = heat[idx(x0, y1)];
      const uint8_t h11 = heat[idx(x1, y1)];

      const float h0 = h00 * (1.0f - fx) + h10 * fx;
      const float h1 = h01 * (1.0f - fx) + h11 * fx;
      float h = h0 * (1.0f - fy) + h1 * fy;

      if (h < 0.0f) h = 0.0f;
      if (h > 255.0f) h = 255.0f;
      tmpHeat[i] = (uint8_t)h;
    }
  }
  memcpy(heat, tmpHeat, W * H);
}

void FireEffect::diffuse() {
  if (p.heatDiffusion <= 0.0f) return;
  const int W = p.width;
  const int H = p.height;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const int i = idx(x, y);
      const int L = idx(x - 1, y);
      const int R = idx(x + 1, y);
      const int U = idx(x, y - 1);
      const int D = idx(x, y + 1);
      const float avg = (heat[L] + heat[R] + heat[U] + heat[D]) * 0.25f;
      const float h = heat[i] * (1.0f - p.heatDiffusion) + avg * p.heatDiffusion;
      tmpHeat[i] = (uint8_t)h;
    }
  }
  memcpy(heat, tmpHeat, W * H);
}

void FireEffect::cool(float energy, float dt) {
  float cooling = p.baseCooling + p.coolingAudioBias * energy;
  if (cooling < 0.0f) cooling = 0.0f;

  const int W = p.width;
  const int H = p.height;
  int baseMaxSub = (int)(cooling * dt) + 1;
  if (baseMaxSub < 1) baseMaxSub = 1;

  for (int y = 0; y < H; ++y) {
    float topBoost = (y <= 1) ? p.topCoolingBoost : 1.0f;   // faster near top
    for (int x = 0; x < W; ++x) {
      const int i = idx(x, y);
      int sub = rand() % baseMaxSub;                         // random base cooling
      float hnorm = (float)heat[i] * (1.0f / 255.0f);        // 0..1
      int rad = (int)(p.radiativeCooling * dt * hnorm * hnorm); // stronger when hot
      int dec = (int)((sub + rad) * topBoost);
      if (dec < 0) dec = 0;
      int h = (int)heat[i] - dec;
      if (h < 0)   h = 0;
      if (h > 240) h = 240;
      heat[i] = (uint8_t)h;
    }
  }
}

uint32_t FireEffect::heatToColor(uint8_t h) const {
  // Warm retro palette: red -> orange -> yellow -> warm white
  uint8_t r, g, b;
  if (h <= 85) {
    r = h * 3; g = 0; b = 0;
  } else if (h <= 170) {
    r = 255; g = (h - 85) * 3; b = 0;
  } else {
    r = 255; g = 255;
    b = (h - 170);
    if (b > 85) b = 85; // keep warm
  }
  float cap = p.brightnessCap;
  if (cap < 0.0f) cap = 0.0f;
  if (cap > 1.0f) cap = 1.0f;
  r = (uint8_t)((float)r * cap);
  g = (uint8_t)((float)g * cap);
  b = (uint8_t)((float)b * cap);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void FireEffect::render() {
  for (int y = 0; y < p.height; ++y) {
    for (int x = 0; x < p.width; ++x) {
      const int i = idx(x, y);
      strip->setPixelColor(i, heatToColor(heat[i]));
    }
  }

  // Optional top-row VU meter (OFF by default)
  if (p.vuTopRowEnabled) {
    const int y = 0;
    const int W = p.width;
    int bars = (int)roundf(lastEnergy * (float)W);
    if (bars < 0) bars = 0;
    if (bars > W) bars = W;
    for (int x = 0; x < W; ++x) {
      if (x < bars) {
        // warm white-ish
        strip->setPixelColor(idx(x, y), ((uint32_t)191 << 16) | ((uint32_t)191 << 8) | (uint32_t)160);
      }
    }
  }

  strip->show();
}
