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

  // ---- Fire engine params (adjusted for tube-light) ----
  constexpr uint8_t BaseCooling         = 95;    // Slightly faster cooling for smaller matrix
  constexpr uint8_t SparkHeatMin        = 45;    // Slightly higher min heat
  constexpr uint8_t SparkHeatMax        = 210;   // Slightly higher max heat
  constexpr float   SparkChance         = 0.34f; // Adjusted for 15 vs 16 bottom positions
  constexpr float   AudioSparkBoost     = 0.3f;  // Same responsiveness
  constexpr uint8_t AudioHeatBoostMax   = 65;    // Slightly higher boost for visibility
  constexpr int8_t  CoolingAudioBias    = -22;   // Slightly more bias for taller flames
  constexpr uint8_t BottomRowsForSparks = 1;     // Same as fire-totem
  constexpr uint8_t TransientHeatMax = 110;      // Slightly higher for visibility

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