#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "SimplexNoise.h"

class FireEffect {
public:
  FireEffect(Adafruit_NeoPixel* strip, int width, int height);

  // micEnv: fast envelope (transients); micRMS: baseline loudness
  // ax, ay: accelerometer (g). Cylinder upright; Δax drives horizontal slosh; Δay flares height.
  void update(float micEnv, float micRMS, float ax, float ay);
  void render();

private:
  Adafruit_NeoPixel* strip;
  int width, height;

  // Heat buffer (0..255). Size supports up to 32x32; your matrix is 16x8.
  float heat[32][32];

  // Motion state
  float lastAx = 0.0f, lastAy = 0.0f;
  float vx = 0.0f;            // horizontal velocity (for momentum)
  float lastIntensity = 0.4f; // color temperature bias

  SimplexNoise noiseGen;

  int  XY(int x, int y);                    // linear, top-wired, vertical flip
  uint32_t heatToColor(float t, float bias); // bias warms/cools flame with music intensity
};
