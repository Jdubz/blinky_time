#pragma once
#include <stdint.h>

namespace Defaults {
  // ---- Window/Range normalization (audio) ----
  constexpr float PeakTau     = 2.0f;   // Peak adaptation time constant (s)
  constexpr float ReleaseTau  = 5.0f;   // Peak release time constant (s)

  // ---- Performance constants ----
  constexpr float CoolingScaleFactor = 0.5f / 255.0f;

  // ---- Fire engine params (used by FireParams + SerialConsole) ----
  constexpr uint8_t BaseCooling         = 85;
  constexpr uint8_t SparkHeatMin        = 40;
  constexpr uint8_t SparkHeatMax        = 200;
  constexpr float   SparkChance         = 0.32f;
  constexpr float   AudioSparkBoost     = 0.3f;
  constexpr int8_t  CoolingAudioBias    = -20;   // negative => taller flames on loud parts
  constexpr uint8_t BottomRowsForSparks = 1;

  // Layout-specific parameters
  constexpr uint8_t SpreadDistance      = 12;      // Heat spread distance for linear/random layouts
  constexpr float   HeatDecay           = 0.92f;   // Heat decay factor for linear layouts

  // Ember parameters
  constexpr uint8_t EmberHeatMax        = 18;      // Maximum ember heat (dim glow)
  constexpr float   EmberNoiseSpeed     = 0.00033f; // Ember animation speed

  // (optional) Ranges you show in help strings
  namespace Ranges {
    constexpr uint8_t CoolingMin = 0, CoolingMax = 255;
    constexpr float   SparkChanceMin = 0.0f, SparkChanceMax = 1.0f;
  }
}
