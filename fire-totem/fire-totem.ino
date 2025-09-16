#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "IMUHelper.h"
#include "FireEffect.h"

#define WIDTH       16
#define HEIGHT       8
#define LED_PIN     D10
#define NUMPIXELS  (WIDTH * HEIGHT)

Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
FireEffect fire(&strip, WIDTH, HEIGHT);
AdaptiveMic mic;
IMUHelper imu;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("Booting...");

  strip.begin();
  strip.setBrightness(40);
  strip.fill(strip.Color(255,0,0));
  strip.show();
  delay(1000);
  strip.clear();
  strip.show();

  mic.begin();
  imu.begin();
}

void loop() {
  // --- Mic ---
  mic.update();
  float level = mic.getLevel();
  float env = mic.getEnvelope();
  float gain = mic.getGain();

  // --- IMU ---
  float ax = 0, ay = 0, az = 0;
  if (imu.isReady()) {
    imu.getAccel(ax, ay, az);
  }

  // --- Fire update ---
  unsigned long frameStart = micros();
  fire.update(level, ax, ay); // passing ax/ay as directional bias
  fire.render();
  unsigned long frameTime = micros() - frameStart;

  float avgHeat = fire.getAverageHeat();
  int activePixels = fire.getActiveCount();

  // --- Console output ---
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 500) {
    Serial.print("lvl=");   Serial.print(level, 3);
    Serial.print(" env=");  Serial.print(env, 4);
    Serial.print(" gain="); Serial.print(gain, 1);

    Serial.print("  ax=");  Serial.print(ax, 3);
    Serial.print(" ay=");   Serial.print(ay, 3);
    Serial.print(" az=");   Serial.print(az, 3);

    Serial.print("  frame="); Serial.print(frameTime);
    Serial.print("us avgHeat="); Serial.print(avgHeat, 3);
    Serial.print(" active="); Serial.println(activePixels);

    lastPrint = millis();
  }

  delay(30);
}
