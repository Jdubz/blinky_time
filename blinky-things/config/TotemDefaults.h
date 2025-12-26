#pragma once
#include <stdint.h>

namespace Defaults {
  // ---- Audio settings (used by SerialConsole restoreDefaults) ----
  constexpr float NoiseGate   = 0.06f;

  // ---- Window/Range normalization ----
  constexpr float PeakTau     = 2.0f;   // Peak adaptation time constant (s)
  constexpr float ReleaseTau  = 5.0f;   // Peak release time constant (s)

  // ---- Performance constants ----
  constexpr float CoolingScaleFactor = 0.5f / 255.0f;

  // ---- Fire engine params (used by FireEffect + SerialConsole) ----
  constexpr uint8_t BaseCooling         = 85;
  constexpr uint8_t SparkHeatMin        = 40;
  constexpr uint8_t SparkHeatMax        = 200;
  constexpr float   SparkChance         = 0.32f;
  constexpr float   AudioSparkBoost     = 0.3f;
  constexpr uint8_t AudioHeatBoostMax   = 60;
  constexpr int8_t  CoolingAudioBias    = -20;   // negative => taller flames on loud parts
  constexpr uint8_t BottomRowsForSparks = 1;
  constexpr uint8_t TransientHeatMax = 100; // 0-255

  // (optional) Ranges you show in help strings
  namespace Ranges {
    constexpr float NoiseGateMin = 0.0f, NoiseGateMax = 0.5f;
    constexpr uint8_t CoolingMin = 0, CoolingMax = 255;
    constexpr float   SparkChanceMin = 0.0f, SparkChanceMax = 1.0f;
  }
}
