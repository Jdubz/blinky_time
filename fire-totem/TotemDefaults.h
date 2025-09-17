#pragma once
#include <stdint.h>

namespace Defaults {
  // ---- Audio mapping (used by fire-totem + SerialConsole) ----
  constexpr float NoiseGate   = 0.06f;
  constexpr float Gamma       = 0.60f;
  constexpr float GlobalGain  = 1.35f;
  constexpr float AttackTau   = 0.08f;   // seconds
  constexpr float ReleaseTau  = 0.30f;   // seconds

  // ---- Software auto-gain (fast) ----
  // Keeps typical normalized level near target across tracks/rooms.
  constexpr float AutoGainTarget   = 0.70f;  // aim to use ~70% of visual range
  constexpr float AutoGainStrength = 0.025f; // integrator step (per second)
  constexpr float AutoGainMin      = 0.60f;  // clamp on mic.globalGain
  constexpr float AutoGainMax      = 2.00f;

  // ---- Fire engine params (used by FireEffect + SerialConsole) ----
  constexpr uint8_t BaseCooling         = 150;
  constexpr uint8_t SparkHeatMin        = 40;
  constexpr uint8_t SparkHeatMax        = 200;
  constexpr float   SparkChance         = 0.25f;
  constexpr float   AudioSparkBoost     = 0.3f;
  constexpr uint8_t AudioHeatBoostMax   = 60;
  constexpr int8_t  CoolingAudioBias    = -20;   // negative => taller flames on loud parts
  constexpr uint8_t BottomRowsForSparks = 1;
  constexpr float   FireDecayTau = 0.28f;  // seconds; larger = smoother/slower fade

  // (optional) Ranges you show in help strings
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
