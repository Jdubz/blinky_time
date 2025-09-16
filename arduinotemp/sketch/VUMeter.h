#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\VUMeter.h"
#pragma once
#include <Adafruit_NeoPixel.h>
#include <stdint.h>

class VUMeter {
public:
  VUMeter(int width, int height);

  // control
  void setEnabled(bool on);
  bool isEnabled() const;

  // frame hooks
  // NO smoothing: this just latches the instantaneous energy (0..1)
  void update(float energy, float /*dt*/);
  void renderTopRow(Adafruit_NeoPixel* strip);

  // optional color tweak (kept simple)
  void setColor(uint8_t r, uint8_t g, uint8_t b);

private:
  int   width, height;
  bool  enabled = false;
  float level   = 0.0f;          // instantaneous, not smoothed
  uint8_t cr=0, cg=120, cb=0;    // green bar

  inline uint16_t idx(int x, int y) const { return (uint16_t)y * width + x; }
};
