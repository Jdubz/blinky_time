#pragma once
#include <Adafruit_NeoPixel.h>
#include <stdint.h>
#include "VUMeter.h"

struct FireParams {
  // Existing (from V2)
  uint8_t baseCooling;          // 45
  uint8_t sparkHeatMin;         // 110
  uint8_t sparkHeatMax;         // 160
  float   sparkChance;          // 0.12
  float   audioSparkBoost;      // 0.95
  uint8_t audioHeatBoostMax;    // 95
  int8_t  coolingAudioBias;     // -18
  uint8_t bottomRowsForSparks;  // 2

  // NEW: fluid / swirl controls
  bool    fluidEnabled;         // true
  float   buoyancy;             // 1.2
  float   viscosity;            // 0.10
  float   heatDiffusion;        // 0.08
  float   swirlAmp;             // 1.2
  float   swirlAudioGain;       // 1.0
  float   swirlScaleCells;      // 6.0
  float   updraftBase;          // 0.0;

  bool  vuTopRowEnabled;
};

class FireEffect {
public:
  FireEffect(Adafruit_NeoPixel* strip, int width, int height);

  // energy: 0..1 (audio energy mapped in loop)
  void update(float energy, float dx, float dy);
  void render();

  // Telemetry
  float getAverageHeat() const;
  int   getActiveCount(uint8_t thresh = 10) const;

  // Runtime tuning
  void setParams(const FireParams& p);
  FireParams getParams() const;

private:
  Adafruit_NeoPixel* strip;
  int width, height;

  VUMeter vu;
   
  // Heat grid (0..255)
  uint8_t* heat;

  // Fluid velocity field (cells/sec)
  float* vx;
  float* vy;

  void riseClassic();  // fallback upward transport when fluid is OFF

  // Scratch buffers
  float* heatScratchF;   // for advection/diffusion
  unsigned long lastMicros = 0;
  float vuLevel = 0.0f;  // smoothed energy for VU bar

  FireParams params;

  inline uint16_t idx(int x, int y) const { return (uint16_t)y * width + x; }

  // Sampling helpers with X-wrap, Y-clamp
  float sampleHeatBilinear(float x, float y) const;
  float sampleField(const float* f, float x, float y) const;

  // Physics steps
  float computeDt();
  void  applyCooling(float energy, float dt);
  void  addForces(float energy, float t, float dt);
  void  diffuseVelocity(float dt);
  void  advectHeat(float dt);
  void  diffuseHeat(float dt);
  void  injectSparks(float energy);

  uint32_t heatToColor(uint8_t h) const;  // 50% brightness cap
};
