#pragma once

namespace Defaults {
  // Matrix size
  constexpr int Width  = 16;
  constexpr int Height = 8;

  // ===== AdaptiveMic defaults =====
  constexpr float NoiseGate       = 0.06f;
  constexpr float GlobalGain      = 1.35f;
  constexpr float AttackSeconds   = 0.08f;
  constexpr float ReleaseSeconds  = 0.30f;
  constexpr float Gamma           = 0.60f;

  // ===== Fire sim defaults (retro tuned) =====
  constexpr bool  FluidEnabled          = true;
  constexpr float Viscosity             = 0.05f;   // lower = more lively
  constexpr float HeatDiffusion         = 0.02f;   // lower = crisper
  constexpr float UpdraftBase           = 5.0f;
  constexpr float Buoyancy              = 16.0f;
  constexpr float SwirlAmp              = 4.0f;
  constexpr float SwirlScaleCells       = 12.0f;   // larger = big visible column twist
  constexpr float SwirlAudioGain        = 1.5f;    // more swirl on loud audio
  constexpr float BaseCooling           = 200.0f;
  constexpr float CoolingAudioBias      = -60.0f;  // louder => less cooling => taller flame
  constexpr float SparkChance           = 0.12f;   // frequent base flicker
  constexpr float SparkHeatMin          = 50.0f;
  constexpr float SparkHeatMax          = 200.0f;
  constexpr float AudioHeatBoostMax     = 200.0f;  // allow hot pops
  constexpr float AudioSparkBoost       = 1.00f;

  // ===== LED output defaults =====
  constexpr float   BrightnessCap       = 0.75f;   // 75% cap (user request)
  constexpr bool    VuTopRowEnabled     = false;   // OFF by default (user request)

  // LED global brightness
  constexpr uint8_t StripBrightness     = 255;
}
