#pragma once
#include <stdint.h>
#include "AdaptiveMic.h"  // for AdaptiveMic::BassMode

namespace Defaults {
  // ---- Monitoring / UI ----
  constexpr bool VuTopRowEnabled = false;  // off by default

  // ---- Audio mapping (used by fire-totem + SerialConsole) ----
  constexpr float NoiseGate   = 0.06f;
  constexpr float Gamma       = 0.60f;
  constexpr float GlobalGain  = 1.35f;
  constexpr float AttackTau   = 0.08f;   // seconds
  constexpr float ReleaseTau  = 0.30f;   // seconds

  // ---- Fire engine params (used by FireEffect + SerialConsole) ----
  constexpr uint8_t BaseCooling         = 200;
  constexpr uint8_t SparkHeatMin        = 50;
  constexpr uint8_t SparkHeatMax        = 200;
  constexpr float   SparkChance         = 0.05f;
  constexpr float   AudioSparkBoost     = 0.5f;
  constexpr uint8_t AudioHeatBoostMax   = 150;
  constexpr int8_t  CoolingAudioBias    = -50;   // negative => taller flames on loud parts
  constexpr uint8_t BottomRowsForSparks = 1;

  // ---- Fluid / swirl defaults ----
  constexpr bool   FluidEnabled       = true;   // master switch
  constexpr float  Buoyancy        = 16.0f;   // up to ~8 cells/sec²
  constexpr float  Viscosity       = 0.10f;
  constexpr float  HeatDiffusion   = 0.08f;
  constexpr float  SwirlAmp        = 1.2f;   // cells/sec
  constexpr float  SwirlAudioGain  = 1.0f;
  constexpr float  SwirlScaleCells = 6.0f;   // curl size ~5 cells
  constexpr float  UpdraftBase     = 5.0f;

  // (optional) Ranges you show in help strings
  namespace Ranges {
    constexpr float NoiseGateMin = 0.0f, NoiseGateMax = 0.5f;
    constexpr float GammaMin     = 0.2f, GammaMax     = 2.5f;
    constexpr float GainMin      = 0.0f, GainMax      = 5.0f;
    constexpr float AttackMin    = 0.005f, AttackMax  = 1.0f;
    constexpr float ReleaseMin   = 0.02f,  ReleaseMax = 2.0f;

    constexpr uint8_t CoolingMin = 0, CoolingMax = 255;
    constexpr float   SparkChanceMin = 0.0f, SparkChanceMax = 1.0f;
    constexpr float   BassFreqMin = 30.0f, BassFreqMax = 400.0f;
    constexpr float   BassQMin    = 0.3f, BassQMax    = 5.0f;
  }
}
