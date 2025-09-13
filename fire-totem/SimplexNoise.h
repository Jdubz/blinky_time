#pragma once
#include <Arduino.h>

// Minimal hash-based 2D noise for embedded use.
// Output range: ~[-1, 1], smooth enough for flame fuel modulation.
class SimplexNoise {
public:
  float noise(float x, float y = 0.0f) {
    int n = (int)floor(x) + (int)floor(y) * 57;
    n = (n << 13) ^ n;
    // Hash to float in [-1, 1]
    return 1.0f - ( (n * (n * n * 15731 + 789221) + 1376312589L) & 0x7fffffff ) / 1073741824.0f;
  }
};
