#pragma once
#include <Adafruit_NeoPixel.h>
#include <stdint.h>

struct FireParams {
  uint8_t baseCooling;          // e.g. 45
  uint8_t sparkHeatMin;         // e.g. 110
  uint8_t sparkHeatMax;         // e.g. 160
  float   sparkChance;          // e.g. 0.12
  float   audioSparkBoost;      // e.g. 0.95
  uint8_t audioHeatBoostMax;    // e.g. 95
  int8_t  coolingAudioBias;     // e.g. -18 (negative lowers cooling as audio rises)
  uint8_t bottomRowsForSparks;  // e.g. 2
};

class FireEffect {
public:
  FireEffect(Adafruit_NeoPixel* strip, int width, int height);

  // energy: 0..1 (audio energy mapped in loop)
  void update(float energy, float dx, float dy);
  void render();

  // --- Telemetry helpers (these MUST match the .cpp signatures) ---
  float getAverageHeat() const;
  int   getActiveCount(uint8_t thresh = 10) const;

  // Runtime tuning
  void setParams(const FireParams& p);
  FireParams getParams() const;

private:
  Adafruit_NeoPixel* strip;
  int width, height;
  uint8_t* heat;    // width*height

  FireParams params;

  inline uint16_t idx(int x, int y) const { return (uint16_t)y * width + x; }
  uint32_t heatToColor(uint8_t h) const;  // 50% brightness cap inside
};
