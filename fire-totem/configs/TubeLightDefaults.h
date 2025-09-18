#pragma once
#include <stdint.h>

namespace TubeLightDefaults {
  // ---- Audio mapping (same as fire-totem) ----
  constexpr float NoiseGate   = 0.06f;
  constexpr float Gamma       = 0.60f;
  constexpr float GlobalGain  = 1.35f;
  constexpr float AttackSeconds   = 0.08f;
  constexpr float ReleaseSeconds  = 0.30f;
  constexpr uint16_t TransientCooldownMs = 500;

  // ---- Performance constants ----
  constexpr float WindSparkFactor = 0.12f;
  constexpr float MaxWindSparkProb = 0.35f;
  constexpr float CoolingScaleFactor = 0.5f / 255.0f;

  // ---- Software auto-gain (same as fire-totem) ----
  constexpr float AutoGainTarget   = 0.55f;
  constexpr float AutoGainStrength = 0.025f;
  constexpr float AutoGainMin      = 0.60f;
  constexpr float AutoGainMax      = 2.00f;

  // ---- Fire engine params (tube-light optimized) ----
  constexpr uint8_t BaseCooling         = 40;    // Slower cooling for better flames
  constexpr uint8_t SparkHeatMin        = 50;    // New optimized range
  constexpr uint8_t SparkHeatMax        = 200;   // New optimized range
  constexpr float   SparkChance         = 0.200f; // Reduced for cleaner fire
  constexpr float   AudioSparkBoost     = 0.300f; // Audio responsiveness
  constexpr uint8_t AudioHeatBoostMax   = 60;    // Audio heat boost
  constexpr int8_t  CoolingAudioBias    = -20;   // Audio cooling bias
  constexpr uint8_t BottomRowsForSparks = 1;     // Single row for sparks
  constexpr uint8_t TransientHeatMax    = 100;   // Transient heat maximum

  // (optional) Ranges
  namespace Ranges {
    constexpr float NoiseGateMin = 0.0f, NoiseGateMax = 0.5f;
    constexpr float GammaMin     = 0.2f, GammaMax     = 2.5f;
    constexpr float GainMin      = 0.0f, GainMax      = 5.0f;
    constexpr float AttackMin    = 0.005f, AttackMax  = 1.0f;
    constexpr float ReleaseMin   = 0.02f,  ReleaseMax = 2.0f;

    constexpr uint8_t CoolingMin = 0, CoolingMax = 255;
    constexpr float   SparkChanceMin = 0.0f, SparkChanceMax = 1.0f;
  }
}