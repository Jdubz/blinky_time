#pragma once
#include <stdint.h>
#include "AdaptiveMic.h"  // for AdaptiveMic::BassMode

namespace Defaults {

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

  // ---- Bass filter defaults (used by SerialConsole -> AdaptiveMic) ----
  constexpr bool     BassEnabledDefault = true;
  constexpr float    BassFc             = 120.0f; // Hz
  constexpr float    BassQ              = 0.8f;
  constexpr AdaptiveMic::BassMode BassModeDefault = AdaptiveMic::BASS_BANDPASS;

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
