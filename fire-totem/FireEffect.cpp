#include "FireEffect.h"
#include "TotemDefaults.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
static inline float scaleToDt(float perFrameAt60, float dt) {
  // Keep original tuning "as if 60 FPS", scale proportionally for actual dt
  return perFrameAt60 * (dt * 60.0f);
}

// ------------------------------------------------------------
// Construction / Params
// ------------------------------------------------------------
FireEffect::FireEffect(Adafruit_NeoPixel* strip, int width, int height)
: strip(strip), width(width), height(height),
  heat(nullptr), vx(nullptr), vy(nullptr), heatScratchF(nullptr), vu(width, height)
{
  const size_t N = (size_t)width * (size_t)height;

  heat = (uint8_t*)malloc(N);
  vx   = (float*)  malloc(N * sizeof(float));
  vy   = (float*)  malloc(N * sizeof(float));
  heatScratchF = (float*)malloc(N * sizeof(float));

  if (heat) memset(heat, 0, N);
  if (vx)   memset(vx,   0, N * sizeof(float));
  if (vy)   memset(vy,   0, N * sizeof(float));
  if (heatScratchF) memset(heatScratchF, 0, N * sizeof(float));

  // Defaults (from TotemDefaults.h)
  params.baseCooling         = Defaults::BaseCooling;
  params.sparkHeatMin        = Defaults::SparkHeatMin;
  params.sparkHeatMax        = Defaults::SparkHeatMax;
  params.sparkChance         = Defaults::SparkChance;
  params.audioSparkBoost     = Defaults::AudioSparkBoost;
  params.audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
  params.coolingAudioBias    = Defaults::CoolingAudioBias;
  params.bottomRowsForSparks = Defaults::BottomRowsForSparks;

  params.fluidEnabled        = Defaults::FluidEnabled;
  params.buoyancy            = Defaults::Buoyancy;
  params.viscosity           = Defaults::Viscosity;
  params.heatDiffusion       = Defaults::HeatDiffusion;
  params.swirlAmp            = Defaults::SwirlAmp;
  params.swirlAudioGain      = Defaults::SwirlAudioGain;
  params.swirlScaleCells     = Defaults::SwirlScaleCells;
  params.updraftBase         = Defaults::UpdraftBase;
  params.vuTopRowEnabled     = Defaults::VuTopRowEnabled;

  vu.setEnabled(params.vuTopRowEnabled);

  lastMicros = micros();
}

void FireEffect::setParams(const FireParams& p) { 
  params = p;
  vu.setEnabled(params.vuTopRowEnabled);
}
FireParams FireEffect::getParams() const { return params; }

// ------------------------------------------------------------
// Sampling helpers
// ------------------------------------------------------------

float FireEffect::computeDt() {
  unsigned long now = micros();
  float dt = (now - lastMicros) / 1e6f;
  if (dt <= 0 || dt > 0.1f) dt = 0.016f; // fallback ~60 FPS
  lastMicros = now;
  return dt;
}

float FireEffect::sampleField(const float* f, float xf, float yf) const {
  // X wrap, Y clamp; bilinear interpolation
  while (xf < 0) xf += width;
  while (xf >= width) xf -= width;
  if (yf < 0) yf = 0;
  if (yf > height - 1) yf = (float)(height - 1);

  int x0 = (int)floorf(xf);
  int y0 = (int)floorf(yf);
  int x1 = (x0 + 1) % width;
  int y1 = (y0 + 1 >= height) ? (height - 1) : (y0 + 1);

  float tx = xf - x0;
  float ty = yf - y0;

  float f00 = f[idx(x0,y0)];
  float f10 = f[idx(x1,y0)];
  float f01 = f[idx(x0,y1)];
  float f11 = f[idx(x1,y1)];

  float a = f00 + (f10 - f00) * tx;
  float b = f01 + (f11 - f01) * tx;
  return a + (b - a) * ty;
}

float FireEffect::sampleHeatBilinear(float xf, float yf) const {
  // Same as sampleField but from uint8_t heat
  while (xf < 0) xf += width;
  while (xf >= width) xf -= width;
  if (yf < 0) yf = 0;
  if (yf > height - 1) yf = (float)(height - 1);

  int x0 = (int)floorf(xf);
  int y0 = (int)floorf(yf);
  int x1 = (x0 + 1) % width;
  int y1 = (y0 + 1 >= height) ? (height - 1) : (y0 + 1);

  float tx = xf - x0;
  float ty = yf - y0;

  float f00 = (float)heat[idx(x0,y0)];
  float f10 = (float)heat[idx(x1,y0)];
  float f01 = (float)heat[idx(x0,y1)];
  float f11 = (float)heat[idx(x1,y1)];

  float a = f00 + (f10 - f00) * tx;
  float b = f01 + (f11 - f01) * tx;
  return a + (b - a) * ty;
}

// ------------------------------------------------------------
// Core steps
// ------------------------------------------------------------

// Classic upward "convection" used when fluid is OFF
void FireEffect::riseClassic() {
  for (int x = 0; x < width; ++x) {
    for (int y = 0; y < height - 1; ++y) {
      int xL = (x == 0) ? (width - 1) : (x - 1);
      int xR = (x == width - 1) ? 0 : (x + 1);
      uint16_t below      = idx(x,   y+1);
      uint16_t belowLeft  = idx(xL,  y+1);
      uint16_t belowRight = idx(xR,  y+1);
      uint8_t avg = (uint8_t)(((int)heat[below] + (int)heat[belowLeft] + (int)heat[belowRight]) / 3);
      uint8_t decay = random(0, 8);
      int val = (int)avg - (int)decay;
      if (val < 0) val = 0;
      heat[idx(x,y)] = (uint8_t)val;
    }
  }
}

// dt-scaled cooling so motion is visible regardless of frame rate
void FireEffect::applyCooling(float energy, float dt) {
  float coolingF = (float)params.baseCooling + (float)params.coolingAudioBias * energy;
  if (coolingF < 0) coolingF = 0;
  const float coolingScaled = scaleToDt(coolingF, dt);

  for (int i = 0, N = width*height; i < N; ++i) {
    uint8_t h = heat[i];
    if (h) {
      // Random fractional cooling 0..coolingScaled
      float rnd = (float)random(0, 1000) * (1.0f / 1000.0f);
      int cool  = (int)(coolingScaled * rnd);
      int v     = (int)h - cool;
      heat[i]   = (v > 0) ? (uint8_t)v : 0;
    }
  }
}

// Forces: divergence-free swirl + buoyancy proportional to local heat
void FireEffect::addForces(float energy, float t, float dt) {
  if (!params.fluidEnabled) return;

  const float scale = max(1.0f, params.swirlScaleCells);
  const float kx = TWO_PI / scale;
  const float ky = TWO_PI / scale;
  const float A  = params.swirlAmp * (1.0f + params.swirlAudioGain * energy); // cells/sec
  const float wx = 0.8f;   // time variation
  const float wy = 0.6f;

  for (int y = 0; y < height; ++y) {
    float fy = (float)y;
    for (int x = 0; x < width; ++x) {
      float fx = (float)x;

      float sx = sinf(kx*fx + wx*t), cx = cosf(kx*fx + wx*t);
      float sy = sinf(ky*fy + wy*t), cy = cosf(ky*fy + wy*t);

      float swirlVx =  A * ky * cy * sx;
      float swirlVy = -A * kx * cx * sy;

      uint8_t h = heat[idx(x,y)];
      float h01 = (float)h * (1.0f/255.0f);
      float buoy = -params.buoyancy * h01; // cells/sec^2 upward (negative y)

      // add constant updraft so even cooler parcels keep drifting upward
      float updraft = -params.updraftBase;

      uint16_t i = idx(x,y);
      vx[i] += swirlVx * dt;
      vy[i] += (swirlVy + buoy + updraft) * dt;
    }
  }

  // Optional velocity clamp for stability on coarse grids
  for (int i = 0, N = width*height; i < N; ++i) {
    float sp = sqrtf(vx[i]*vx[i] + vy[i]*vy[i]);
    const float maxSp = 12.0f; // cells/sec
    if (sp > maxSp && sp > 0.0001f) {
      float s = maxSp / sp;
      vx[i] *= s; vy[i] *= s;
    }
  }
}

// Simple one-pass viscosity smoothing (Jacobi-ish)
void FireEffect::diffuseVelocity(float dt) {
  if (!params.fluidEnabled) return;
  const float a = constrain(params.viscosity, 0.0f, 1.0f) * dt * 4.0f;
  if (a <= 0) return;

  for (int y = 0; y < height; ++y) {
    int yU = (y == 0) ? 0 : (y - 1);
    int yD = (y == height - 1) ? (height - 1) : (y + 1);
    for (int x = 0; x < width; ++x) {
      int xL = (x == 0) ? (width - 1) : (x - 1);
      int xR = (x == width - 1) ? 0 : (x + 1);

      uint16_t i = idx(x,y);
      float sumVx = vx[idx(xL,y)] + vx[idx(xR,y)] + vx[idx(x,yU)] + vx[idx(x,yD)];
      float sumVy = vy[idx(xL,y)] + vy[idx(xR,y)] + vy[idx(x,yU)] + vy[idx(x,yD)];
      vx[i] = vx[i] + (sumVx - 4.0f*vx[i]) * (a * 0.25f);
      vy[i] = vy[i] + (sumVy - 4.0f*vy[i]) * (a * 0.25f);
    }
  }
}

// Semi-Lagrangian advection of heat along (vx,vy)
void FireEffect::advectHeat(float dt) {
  const int W = width, H = height;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint16_t i = idx(x,y);
      float fx = (float)x - vx[i] * dt;
      float fy = (float)y - vy[i] * dt;
      heatScratchF[i] = sampleHeatBilinear(fx, fy); // float 0..255
    }
  }
  const int N = W*H;
  for (int i = 0; i < N; ++i) {
    float v = heatScratchF[i];
    if (v < 0) v = 0; if (v > 255) v = 255;
    heat[i] = (uint8_t)(v + 0.5f);
  }
}

// Gentle mixing of heat (1 pass laplacian blur)
void FireEffect::diffuseHeat(float dt) {
  if (params.heatDiffusion <= 0) return;
  const float k = constrain(params.heatDiffusion, 0.0f, 1.0f) * dt;

  for (int y = 0; y < height; ++y) {
    int yU = (y == 0) ? 0 : (y - 1);
    int yD = (y == height - 1) ? (height - 1) : (y + 1);
    for (int x = 0; x < width; ++x) {
      int xL = (x == 0) ? (width - 1) : (x - 1);
      int xR = (x == width - 1) ? 0 : (x + 1);

      uint8_t hC = heat[idx(x,y)];
      float lap = (float)heat[idx(xL,y)] + (float)heat[idx(xR,y)] +
                  (float)heat[idx(x,yU)] + (float)heat[idx(x,yD)] - 4.0f*(float)hC;
      heatScratchF[idx(x,y)] = (float)hC + k * (lap * 0.25f);
    }
  }

  const int N = width*height;
  for (int i = 0; i < N; ++i) {
    float v = heatScratchF[i];
    if (v < 0) v = 0; if (v > 255) v = 255;
    heat[i] = (uint8_t)(v + 0.5f);
  }
}

// Audio-driven sparks at the bottom rows
void FireEffect::injectSparks(float energy) {
  const float chance = params.sparkChance + (params.audioSparkBoost * energy);
  const uint8_t addHeat = (uint8_t)min<int>(params.audioHeatBoostMax, (int)(params.audioHeatBoostMax * energy));
  const int bottomStart = max(0, height - (int)params.bottomRowsForSparks);

  for (int x = 0; x < width; x++) {
    if (random(1000) < (int)(chance * 1000.0f)) {
      for (int y = bottomStart; y < height; y++) {
        uint8_t base = random(params.sparkHeatMin, (int)params.sparkHeatMax + 1);
        int v = (int)base + (int)addHeat;
        if (v > 255) v = 255;
        uint16_t i = idx(x,y);
        if (v > heat[i]) heat[i] = (uint8_t)v;

        // Small immediate swirl kick to fresh sparks (helps visible motion)
        if (params.fluidEnabled) {
          float t = millis() * 0.001f;
          const float scale = max(1.0f, params.swirlScaleCells);
          const float kx = TWO_PI / scale;
          const float ky = TWO_PI / scale;
          const float A  = params.swirlAmp * (1.0f + params.swirlAudioGain * energy);
          float sx = sinf(kx*x + 0.8f*t), cx = cosf(kx*x + 0.8f*t);
          float sy = sinf(ky*y + 0.6f*t), cy = cosf(ky*y + 0.6f*t);
          float swirlVx =  A * ky * cy * sx;
          float swirlVy = -A * kx * cx * sy;
          vx[i] += swirlVx * 0.2f;
          vy[i] += swirlVy * 0.2f;
        }
      }
    }
  }
}

// ------------------------------------------------------------
// Frame update
// ------------------------------------------------------------
void FireEffect::update(float energy, float dx, float dy) {
  (void)dx; (void)dy;
  if (!isfinite(energy)) energy = 0.0f;
  energy = constrain(energy, 0.0f, 1.0f);

  float dt = computeDt();
  vu.update(energy, dt);
  
  float t  = millis() * 0.001f;

  // A) Source FIRST so new sparks can move immediately this frame
  injectSparks(energy);

  if (params.fluidEnabled) {
    // B) Forces & viscosity
    addForces(energy, t, dt);
    diffuseVelocity(dt);

    // C) Move heat with flow
    advectHeat(dt);

    // D) Gentle mixing
    diffuseHeat(dt);
  } else {
    // Classic fallback (keeps old behavior when fluid is off)
    riseClassic();
    diffuseHeat(dt);
  }

  // E) Finally cool (dt-scaled), so transport remains visible this frame
  applyCooling(energy, dt);
}

// ------------------------------------------------------------
// Rendering
// ------------------------------------------------------------
void FireEffect::render() {
  const int n = width * height;
  for (int i = 0; i < n; i++) {
    strip->setPixelColor(i, heatToColor(heat[i]));
  }

  // overlay top-row VU if enabled
  if (params.vuTopRowEnabled) {
    vu.renderTopRow(strip);
  }
  
  strip->show();
}

// Palette: black -> red -> orange -> yellow -> bluish white, with 50% cap
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
  // Global 50% brightness cap (Stable baseline rule)
  r >>= 1; g >>= 1; b >>= 1;
  return strip->Color(r, g, b);
}

// ------------------------------------------------------------
// Telemetry
// ------------------------------------------------------------
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
