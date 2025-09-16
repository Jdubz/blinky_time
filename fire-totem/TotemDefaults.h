#pragma once

// Centralized defaults for Fire Totem.
// Compatible with existing SerialConsole expectations.

namespace Defaults {

  // --- Matrix size ---
  constexpr int Width  = 16;
  constexpr int Height = 8;

  // --- LED ---
  constexpr uint8_t StripBrightness = 255;

  // --- Audio (AdaptiveMic / AudioParams) ---
  // Envelope/normalization
  constexpr float NoiseGate      = 0.06f;
  constexpr float GlobalGain     = 1.35f;
  constexpr float Gamma          = 0.60f;
  // Time constants (SerialConsole expects *Tau* names)
  constexpr float AttackTau      = 0.08f;   // seconds
  constexpr float ReleaseTau     = 0.30f;   // seconds

  // Optional bass biquad defaults (SerialConsole/mic may use/expect these)
  constexpr bool  BassEnabledDefault = false;
  constexpr float BassFc             = 120.0f;  // Hz
  constexpr float BassQ              = 0.707f;  // Q-factor
  // Mode: 0=off/none, 1=low-pass, 2=band-pass
  constexpr uint8_t BassModeDefault  = 2;

  // Valid ranges used by SerialConsole.cpp's constrain() calls
  namespace Ranges {
    // Audio
    constexpr float NoiseGateMin   = 0.00f;
    constexpr float NoiseGateMax   = 0.50f;
    constexpr float GammaMin       = 0.10f;
    constexpr float GammaMax       = 2.50f;
    constexpr float GainMin        = 0.10f;
    constexpr float GainMax        = 10.0f;
    constexpr float AttackMin      = 0.001f;
    constexpr float AttackMax      = 1.000f;
    constexpr float ReleaseMin     = 0.010f;
    constexpr float ReleaseMax     = 2.000f;
    // Fire
    constexpr int   CoolingMin     = 0;
    constexpr int   CoolingMax     = 1024;
    constexpr float SparkChanceMin = 0.00f;
    constexpr float SparkChanceMax = 1.00f;
    // Bass filter
    constexpr float BassFreqMin    = 20.0f;
    constexpr float BassFreqMax    = 500.0f;
    constexpr float BassQMin       = 0.30f;
    constexpr float BassQMax       = 5.00f;
  }

  // --- Fire sim defaults (retro tuned) ---
  constexpr bool  FluidEnabled       = true;
  constexpr float Viscosity          = 0.05f;   // lower = more lively
  constexpr float HeatDiffusion      = 0.02f;   // lower = crisper
  constexpr float UpdraftBase        = 5.0f;
  constexpr float Buoyancy           = 16.0f;
  constexpr float SwirlAmp           = 4.0f;
  constexpr float SwirlScaleCells    = 12.0f;   // larger = a single visible twist
  constexpr float SwirlAudioGain     = 1.5f;    // more swirl on loud audio
  constexpr float BaseCooling        = 200.0f;
  constexpr float CoolingAudioBias   = -60.0f;  // louder => less cooling => taller flame
  constexpr float SparkChance        = 0.12f;   // frequent base flicker
  constexpr float SparkHeatMin       = 50.0f;
  constexpr float SparkHeatMax       = 200.0f;
  constexpr float AudioHeatBoostMax  = 200.0f;  // allow hot pops
  constexpr float AudioSparkBoost    = 1.00f;
  constexpr uint8_t BottomRowsForSparks = 1;    // how many bottom rows can spawn sparks

  // Output
  constexpr float BrightnessCap      = 0.75f;   // 75% cap per user request
  constexpr bool  VuTopRowEnabled    = false;   // OFF by default per user request
}
