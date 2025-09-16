#pragma once
#include <Adafruit_NeoPixel.h>

class FireEffect {
public:
  FireEffect(Adafruit_NeoPixel* strip, int width, int height);

  // level: 0..1 loudness/intensity (kept for API compatibility; not required)
  // dx,dy  : ignored in this restored version
  void update(float level, float dx, float dy);
  void render();

  // Debug helpers for your console readout
  float getAverageHeat() const;
  int   getActiveCount(uint8_t thresh = 10) const;

private:
  Adafruit_NeoPixel* strip;
  int width, height;
  uint8_t* heat;              // size = width*height

  inline uint16_t idx(int x, int y) const { return (uint16_t)y * width + x; }
  uint32_t heatToColor(uint8_t h) const;  // maps 0..255 -> RGB (with 50% cap)
};
