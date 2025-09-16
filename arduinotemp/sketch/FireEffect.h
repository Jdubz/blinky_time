#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\FireEffect.h"
#pragma once
#include <Arduino.h>
class Adafruit_NeoPixel;
struct FireParams {
  int   width=16, height=8;
  bool  fluidEnabled=true;
  float viscosity=0.08f, heatDiffusion=0.03f;
  float updraftBase=6.5f, buoyancy=12.0f, swirlAmp=4.0f, swirlScaleCells=12.0f, swirlAudioGain=1.5f;
  float baseCooling=280.0f, coolingAudioBias=-80.0f;
  float sparkChance=0.06f, sparkHeatMin=35.0f, sparkHeatMax=110.0f, audioHeatBoostMax=110.0f, audioSparkBoost=0.60f;
  uint8_t bottomRowsForSparks=1;
  bool  vuTopRowEnabled=false; float brightnessCap=0.75f;
  float radiativeCooling=90.0f, topCoolingBoost=2.5f, velDamping=0.985f;
};
class FireEffect {
 public:
  FireEffect(Adafruit_NeoPixel* strip, const FireParams& params);
  void update(float energy, float dx, float dy);
  void render();
  void restoreDefaults();
  inline FireParams getParams() const { return p; }
  inline void setParams(const FireParams& np) { p = np; }
  FireParams& paramsRef() { return p; }
 private:
  FireParams p; Adafruit_NeoPixel* strip;
  uint8_t* heat=nullptr; uint8_t* tmpHeat=nullptr; float* vx=nullptr; float* vy=nullptr;
  unsigned long lastMs=0; float lastEnergy=0.0f;
  inline int idx(int x, int y) const;
  void addSparks(float energy); void addForces(float energy, float dt, float tiltX, float tiltY);
  void advect(float dt); void diffuse(); void cool(float energy, float dt); uint32_t heatToColor(uint8_t h) const;
};
