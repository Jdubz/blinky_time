#include "VUMeter.h"
#include <Arduino.h>
#include <math.h>

VUMeter::VUMeter(int w, int h) : width(w), height(h) {}

void VUMeter::setEnabled(bool on) { enabled = on; }
bool VUMeter::isEnabled() const { return enabled; }

void VUMeter::setColor(uint8_t r, uint8_t g, uint8_t b) { cr = r; cg = g; cb = b; }

// No smoothing: mirror the exact energy (0..1) that FireEffect uses
void VUMeter::update(float energy, float /*dt*/) {
  if (!isfinite(energy)) energy = 0.0f;
  if (energy < 0) energy = 0;
  if (energy > 1) energy = 1;
  level = energy;
}

// Show the entire 0..1 range uniformly over the 16 pixels.
// Each pixel i lights if level >= (i+1)/width. This guarantees full-scale = all 16.
void VUMeter::renderTopRow(Adafruit_NeoPixel* strip) {
  if (!enabled || !strip) return;

  uint32_t onColor  = strip->Color(cr, cg, cb);
  uint32_t offColor = strip->Color(0, 0, 0);

  for (int x = 0; x < width; ++x) {
    float threshold = (float)(x + 1) / (float)width;  // 1/16, 2/16, ... 16/16
    bool on = (level >= threshold);
    strip->setPixelColor(idx(x, 0), on ? onColor : offColor);
  }
}
