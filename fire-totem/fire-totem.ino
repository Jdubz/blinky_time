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
FireParams    fp;

// Audio params block expected by SerialConsole
// Serial console expects simplified constructor signature (no AudioParams)
FireEffect*   fire = nullptr;
SerialConsole* consolePtr = nullptr;

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(Defaults::StripBrightness);
  strip.show();

  // Load defaults (Fire)
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
  fp.bottomRowsForSparks = Defaults::BottomRowsForSparks;
  fp.vuTopRowEnabled  = Defaults::VuTopRowEnabled; // OFF by default
  fp.brightnessCap    = Defaults::BrightnessCap;   // 75% cap

  fire = new FireEffect(&strip, fp);

  // Initialize mic & imu according to your existing APIs
  mic.begin();      // your AdaptiveMic has no-arg begin()
  imu.begin();

  // SerialConsole: SerialConsole(FireEffect* fire, AudioParams* audio, uint8_t maxRows, AdaptiveMic* mic);
  consolePtr = new SerialConsole(fire, (uint8_t)fp.height, &mic);
  consolePtr->begin();  // no args per your header
}

void loop() {
  // Audio energy from your mic API (replace if your getter differs)
  float energy = 0.0f;
  energy = mic.getLevel();

  // IMU tilt (accelerometer). If read fails, zeros.
  float ax=0, ay=0, az=0;
  if (!imu.getAccel(ax, ay, az)) { ax = ay = az = 0; }

  // Pass tilt; FireEffect inverts to push opposite gravity
  fire->update(energy, ax, ay);
  fire->render();

  // No console.tick() — your console header doesn't define it
  delay(16); // ~60 fps
}
