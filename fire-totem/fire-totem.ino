#include <Wire.h>
#include <LSM6DS3.h>
#include <Adafruit_NeoPixel.h>
#include "AdaptiveMic.h"
#include "FireEffect.h"

#define WIDTH 16
#define HEIGHT 8
#define LED_PIN D0

Adafruit_NeoPixel strip(WIDTH * HEIGHT, LED_PIN, NEO_GRB + NEO_KHZ800);

AdaptiveMic mic;
FireEffect fire(WIDTH, HEIGHT, strip);

// XIAO Sense IMU object
LSM6DS3 imu(I2C_MODE, 0x6A);

void setup() {
  Serial.begin(115200);

  if (imu.begin() != 0) {
    Serial.println("Failed to initialize IMU!");
    while (1);
  }

  mic.begin();
  fire.begin();
}

void loop() {
  mic.update();

  float micLevel = mic.getLevel();
  float micRMS   = mic.getNormalizedRMS();

  float ax = imu.readFloatAccelX();
  float ay = imu.readFloatAccelY();
  float az = imu.readFloatAccelZ();

  fire.update(micLevel, micRMS, ax);
  fire.render();

  delay(30);
}
