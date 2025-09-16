#pragma once
#include <Arduino.h>

class Adafruit_NeoPixel;

struct FireParams {
  int   width  = 16;
  int   height = 8;

  // Fluid / transport
  bool  fluidEnabled    = true;
  float viscosity       = 0.05f;
  float heatDiffusion   = 0.02f;

  // Forces
  float updraftBase     = 5.0f;
  float buoyancy        = 16.0f;
  float swirlAmp        = 4.0f;
  float swirlScaleCells = 12.0f;
  float swirlAudioGain  = 1.5f;

  // Cooling / audio coupling
  float baseCooling     = 200.0f;
  float coolingAudioBias= -60.0f;

  // Sparks
  float   sparkChance       = 0.12f;
  float   sparkHeatMin      = 50.0f;
  float   sparkHeatMax      = 200.0f;
  float   audioHeatBoostMax = 200.0f;
  float   audioSparkBoost   = 1.0f;
  uint8_t bottomRowsForSparks = 1; // bottom N rows can spawn sparks
  float radiativeCooling = 90.0f;
  float topCoolingBoost  = 2.5f;
  float velDamping       = 0.985f;

  // UI/output
  bool  vuTopRowEnabled = false; // top row VU (off by default)
  float brightnessCap   = 0.75f; // 0..1, global scale
};

class FireEffect {
 public:
  FireEffect(Adafruit_NeoPixel* strip, const FireParams& params);
  void update(float energy, float dx, float dy);
  void render();
  void restoreDefaults();

  // For SerialConsole compatibility
  inline FireParams getParams() const { return p; }
  inline void setParams(const FireParams& np) { p = np; }

  FireParams& paramsRef() { return p; }

 private:
  FireParams p;
  Adafruit_NeoPixel* strip;

  // internal buffers
  uint8_t* heat   = nullptr; // size w*h
  uint8_t* tmpHeat= nullptr; // size w*h
  float*   vx     = nullptr; // velocity x
  float*   vy     = nullptr; // velocity y
  unsigned long lastMs = 0;
  float lastEnergy = 0.0f;

  inline int idx(int x, int y) const {
    // x wraps (cylinder), y clamps
    x = (x % p.width + p.width) % p.width;
    if (y < 0) y = 0;
    if (y >= p.height) y = p.height - 1;
    return y * p.width + x;
  }

  void addSparks(float energy);
  void addForces(float energy, float dt, float tiltX, float tiltY);
  void advect(float dt);
  void diffuse();
  void cool(float energy, float dt);
  uint32_t heatToColor(uint8_t h) const;
};
