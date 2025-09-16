#include <Adafruit_NeoPixel.h>
#include "FireEffect.h"
#include "TotemDefaults.h"
#include "AdaptiveMic.h"
#include "IMUHelper.h"
#include "SerialConsole.h"

// ===== LED strip =====
constexpr uint8_t LED_PIN = D10;
Adafruit_NeoPixel strip(Defaults::Width * Defaults::Height, LED_PIN, NEO_GRB + NEO_KHZ800);

// ===== Modules =====
AdaptiveMic   mic;
IMUHelper     imu;
SerialConsole console;

// Fire effect
FireParams   fp;
FireEffect*  fire = nullptr;

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(Defaults::StripBrightness);
  strip.show();

  // Load defaults
  fp.width            = Defaults::Width;
  fp.height           = Defaults::Height;
  fp.fluidEnabled     = Defaults::FluidEnabled;
  fp.viscosity        = Defaults::Viscosity;
  fp.heatDiffusion    = Defaults::HeatDiffusion;
  fp.updraftBase      = Defaults::UpdraftBase;
  fp.buoyancy         = Defaults::Buoyancy;
  fp.swirlAmp         = Defaults::SwirlAmp;
  fp.swirlScaleCells  = Defaults::SwirlScaleCells;
  fp.swirlAudioGain   = Defaults::SwirlAudioGain;
  fp.baseCooling      = Defaults::BaseCooling;
  fp.coolingAudioBias = Defaults::CoolingAudioBias;
  fp.sparkChance      = Defaults::SparkChance;
  fp.sparkHeatMin     = Defaults::SparkHeatMin;
  fp.sparkHeatMax     = Defaults::SparkHeatMax;
  fp.audioHeatBoostMax= Defaults::AudioHeatBoostMax;
  fp.audioSparkBoost  = Defaults::AudioSparkBoost;
  fp.vuTopRowEnabled  = Defaults::VuTopRowEnabled; // OFF by default
  fp.brightnessCap    = Defaults::BrightnessCap;   // 75% cap

  fire = new FireEffect(&strip, fp);

  mic.begin(Defaults::NoiseGate, Defaults::GlobalGain, Defaults::AttackSeconds, Defaults::ReleaseSeconds, Defaults::Gamma);
  imu.begin();
  console.begin(&fp);
}

void loop() {
  // 1) Audio energy
  float energy = mic.level(); // normalized 0..1

  // 2) IMU tilt (accelerometer). If unavailable, dx=dy=0.
  float ax=0, ay=0, az=0;
  if (imu.available()) {
    imu.getAccel(ax, ay, az);
  }
  float dx = ax; // pass raw; FireEffect inverts to push opposite gravity
  float dy = ay;

  fire->update(energy, dx, dy);
  fire->render();

  console.tick();

  // ~60 fps
  delay(16);
}
