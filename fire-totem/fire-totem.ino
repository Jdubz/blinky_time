#include <Adafruit_NeoPixel.h>
#include <Arduino_LSM6DS3.h>   // Built-in IMU on XIAO Sense
#include "FireEffect.h"
#include "AdaptiveMic.h"

#define WIDTH       16
#define HEIGHT       8
#define LED_PIN     D10
#define NUMPIXELS  (WIDTH * HEIGHT)

Adafruit_NeoPixel strip(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
FireEffect fire(&strip, WIDTH, HEIGHT);
AdaptiveMic mic;
bool imuReady = false;

void setup() {
  // Start serial and wait for connection
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    // wait up to 5 seconds for USB connection
  }

  Serial.println("Booting...");
  
  strip.begin();
  strip.setBrightness(40);
  strip.fill(strip.Color(255,0,0));
  strip.show();
  delay(1000);
  strip.fill(strip.Color(0,0,0));
  strip.show();

  delay(500);           // give USB & mic time to power up
  mic.begin();
  
if (!IMU.begin()) {
  Serial.println("IMU init failed! (continuing without IMU)");
  imuReady = false;
} else {
  Serial.println("IMU ready");
  imuReady = true;
}
}

void loop() {
  mic.update();
  float level = mic.getLevel();    // ← from AdaptiveMic
  
  fire.update(level, 0.0, 0.0);
  fire.render();
  
  delay(30);
}
//
//  
//  mic.update();
//  
//  float level = mic.getLevel();
//  if (level < 0.3f) level = 0.3f;
//
//  float ax = 0, ay = 0, az = 0;
//  if (imuReady && IMU.accelerationAvailable()) {
//    if (!IMU.readAcceleration(ax, ay, az)) {
//      ax = ay = az = 0;
//    }
//  }
//
//  static float lastAx = 0, lastAy = 0;
//  float dx = ax - lastAx;
//  float dy = ay - lastAy;
//  lastAx = ax;
//  lastAy = ay;
//
//  fire.update(level, dx, dy);
//  fire.render();
//
//  // heartbeat debug
//  static unsigned long lastPrint = 0;
//  if (millis() - lastPrint > 1000) {
//    Serial.print("tick level="); Serial.print(level, 3);
//    Serial.print(" dx="); Serial.print(dx, 3);
//    Serial.print(" dy="); Serial.println(dy, 3);
//    lastPrint = millis();
//  }
//
//  delay(30);
//}
