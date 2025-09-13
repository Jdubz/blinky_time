#include <Wire.h>
#include <LSM6DS3.h>              // Seeed IMU driver
#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"

// ---------------------- CONFIG ----------------------
#define WIDTH      16
#define HEIGHT     8
#define LED_PIN    D0             // <-- change if needed
#define NUMPIXELS  (WIDTH * HEIGHT)
// ---------------------------------------------------

Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
AdaptiveMic mic;
FireEffect fire(&strip, WIDTH, HEIGHT);

// XIAO nRF52840 Sense IMU on I2C (default address 0x6A)
LSM6DS3 imu(I2C_MODE, 0x6A);

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.show();

  // Mic
  mic.begin();

  // IMU
  if (imu.begin() != 0) {
    Serial.println("Failed to initialize IMU!");
    while (1);
  }

  // Optional: small startup delay for stability
  delay(50);
}

void loop() {
  mic.update();

  // Read acceleration (g)
  float ax = imu.readFloatAccelX();
  float ay = imu.readFloatAccelY();
  // float az = imu.readFloatAccelZ(); // not needed here

  // Use transient envelope for punch, RMS for baseline “heat”
  float musicEnvelope = mic.getEnvelope(); // fast, transient-reactive
  float musicRMS      = mic.getRMS();      // normalized baseline (software gain applied)

  fire.update(musicEnvelope, musicRMS, ax, ay);
  fire.render();

  // Optional debug (uncomment to tune)
  /*
  static uint32_t t0 = 0;
  if (millis() - t0 > 500) {
    Serial.print("RMS: "); Serial.print(musicRMS, 3);
    Serial.print("  Env: "); Serial.print(musicEnvelope, 3);
    Serial.print("  HW Gain: "); Serial.print(mic.getHardwareGain());
    Serial.print("  SW Gain: "); Serial.println(mic.getSoftwareGain(), 2);
    t0 = millis();
  }
  */
}
