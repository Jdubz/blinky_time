#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\TotemDefaults.h"
#pragma once
#include "AdaptiveMic.h"
namespace Defaults {
  constexpr int Width=16, Height=8;
  constexpr uint8_t StripBrightness = 255;
  constexpr float NoiseGate=0.06f, GlobalGain=1.35f, Gamma=0.60f;
  constexpr float AttackTau=0.08f, ReleaseTau=0.30f;
  constexpr bool  BassEnabledDefault=false;
  constexpr float BassFc=120.0f, BassQ=0.707f;
  constexpr AdaptiveMic::BassMode BassModeDefault = AdaptiveMic::BASS_BANDPASS;
  namespace Ranges {
    constexpr float NoiseGateMin=0, NoiseGateMax=0.5, GammaMin=0.1, GammaMax=2.5, GainMin=0.1, GainMax=10;
    constexpr float AttackMin=0.001, AttackMax=1.0, ReleaseMin=0.01, ReleaseMax=2.0;
    constexpr int   CoolingMin=0, CoolingMax=1024;
    constexpr float SparkChanceMin=0.0f, SparkChanceMax=1.0f;
    constexpr float BassFreqMin=20.0f, BassFreqMax=500.0f, BassQMin=0.3f, BassQMax=5.0f;
  }
  constexpr bool  FluidEnabled = true;
  constexpr float Viscosity=0.08f, HeatDiffusion=0.03f;
  constexpr float UpdraftBase=6.5f, Buoyancy=12.0f, SwirlAmp=4.0f, SwirlScaleCells=12.0f, SwirlAudioGain=1.5f;
  constexpr float BaseCooling=280.0f, CoolingAudioBias=-80.0f;
  constexpr float SparkChance=0.06f, SparkHeatMin=35.0f, SparkHeatMax=110.0f, AudioHeatBoostMax=110.0f, AudioSparkBoost=0.60f;
  constexpr uint8_t BottomRowsForSparks=1;
  constexpr float RadiativeCooling=90.0f, TopCoolingBoost=2.5f, VelocityDamping=0.985f;
  constexpr float BrightnessCap=0.75f;
  constexpr bool  VuTopRowEnabled=false;
}
