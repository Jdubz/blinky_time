#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "IMUHelper.h"
#include "FireEffect.h"
#include "SerialConsole.h"
#include <math.h>

#define WIDTH       16
#define HEIGHT       8
#define LED_PIN     D10
#define NUMPIXELS  (WIDTH * HEIGHT)

Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
FireEffect  fire(&strip, WIDTH, HEIGHT);
AdaptiveMic mic;
IMUHelper   imu;

// Shared audio params used by loop() and tuned via SerialConsole
AudioParams audio;

// Serial console (handles commands + 5s prints)
SerialConsole console(&fire, &audio, HEIGHT, &mic);

// State for envelope & telemetry
static float s_audioEnv = 0.0f;
static unsigned long s_lastMicros   = 0;
static unsigned long s_lastTelemMs  = 0;

void setup() {
  Serial.begin(115200);
  delay(50);

  strip.begin();
  strip.setBrightness(255); // per-pixel cap enforced in FireEffect
  strip.show();

  mic.begin();
  imu.begin();

  console.begin(); // sets defaults and prints initial values

  s_lastMicros = micros();
  Serial.println(F("fire-totem: audio-reactive fire + serial console"));
}

void loop() {
  console.update();  // handle serial & 5s print

  // ---- Mic / audio envelope ----
  mic.update();

  unsigned long now = micros();
  float dt = (now - s_lastMicros) / 1e6f;
  if (dt <= 0 || dt > 1.0f) dt = 1.0f;
  s_lastMicros = now;

  float lvl = mic.getLevel();
  if (!isfinite(lvl)) lvl = 0.0f;
  lvl = constrain(lvl, 0.0f, 1.0f);

  float aAttack  = 1.0f - expf(-dt / audio.attackTau);
  float aRelease = 1.0f - expf(-dt / audio.releaseTau);
  float alpha    = (lvl > s_audioEnv) ? aAttack : aRelease;
  s_audioEnv    += alpha * (lvl - s_audioEnv);

  float gated = s_audioEnv - audio.noiseGate;
  if (gated < 0) gated = 0;
  float norm  = (audio.noiseGate < 0.999f) ? (gated / (1.0f - audio.noiseGate)) : 0.0f;
  norm = constrain(norm, 0.0f, 1.0f);

  float energy = powf(norm, audio.gamma) * audio.globalGain;
  energy = constrain(energy, 0.0f, 1.0f);

  fire.update(energy, 0.0f, 0.0f);
  fire.render();

  // ---- Telemetry (10 Hz) ----
//  if (millis() - s_lastTelemMs > 100) {
//    Serial.print(F("lvl="));     Serial.print(lvl, 3);
//    Serial.print(F(" env="));    Serial.print(s_audioEnv, 3);
//    Serial.print(F(" energy=")); Serial.print(energy, 3);
//    Serial.print(F(" gain="));   Serial.print(mic.getGain());
//    Serial.print(F(" avgHeat="));Serial.print(fire.getAverageHeat(), 1);
//    Serial.print(F(" active=")); Serial.print(fire.getActiveCount(10));
//    Serial.println();
//    s_lastTelemMs = millis();
//  }

  delay(16); // ~60 FPS
}
