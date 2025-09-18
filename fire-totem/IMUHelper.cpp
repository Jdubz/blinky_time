#include "IMUHelper.h"
#include <Wire.h>
#include <LSM6DS3.h>
#include <math.h>

#define IMU_ADDR 0x6A
LSM6DS3 senseIMU(I2C_MODE, IMU_ADDR);

IMUHelper::IMUHelper() {}

bool IMUHelper::begin() {
    Wire.begin();
    delay(500);

    if (senseIMU.begin() != 0) {
        Serial.println(F("IMU Device error"));
        imuReady = false;
        return false;
    } else {
        Serial.println(F("IMU Device OK!"));
        imuReady = true;
        return true;
    }
}

bool IMUHelper::getAccel(float &ax, float &ay, float &az) {
    if (!imuReady) return false;

    ax = senseIMU.readFloatAccelX();
    ay = senseIMU.readFloatAccelY();
    az = senseIMU.readFloatAccelZ();

    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az)) {
        ax = ay = az = 0;
        return false;
    }
    return true;
}

bool IMUHelper::getGyro(float &gx, float &gy, float &gz) {
    if (!imuReady) return false;

    gx = senseIMU.readFloatGyroX();
    gy = senseIMU.readFloatGyroY();
    gz = senseIMU.readFloatGyroZ();

    if (!isfinite(gx) || !isfinite(gy) || !isfinite(gz)) {
        gx = gy = gz = 0;
        return false;
    }
    return true;
}

float IMUHelper::getTempC() {
    if (!imuReady) return NAN;
    return senseIMU.readTempC();
}

bool IMUHelper::readAccelGyro_(float& ax, float& ay, float& az,
                               float& gx, float& gy, float& gz) {
    bool ok = getAccel(ax, ay, az) && getGyro(gx, gy, gz);
    // ax..az: m/s^2, gx..gz: rad/s
    return ok;

    // --- Default: not wired yet ---
    (void)ax; (void)ay; (void)az; (void)gx; (void)gy; (void)gz;
    return false;
}

bool IMUHelper::updateMotion(float dt) {
   float ax, ay, az, gx, gy, gz;
   if (!readAccelGyro_(ax, ay, az, gx, gy, gz) || dt <= 0.f) {
     // Decay toward rest when no fresh data
     motion_.wind.x *= 0.9f;
     motion_.wind.y *= 0.9f;
     motion_.stoke  *= 0.85f;
     return false;
   }
   return updateMotionWithRaw(ax, ay, az, gx, gy, gz, dt);
}

bool IMUHelper::updateMotionWithRaw(float ax, float ay, float az,
                                    float gx, float gy, float gz,
                                    float dt) {
  if (dt <= 0.f) return false;

  // 1) gravity low-pass (complementary-like)
  float aLP = (cfg_.tauLP>0.f) ? (1.f - expf(-dt/cfg_.tauLP)) : 1.f;
  gLP_.x = gLP_.x*(1.f-aLP) + ax*aLP;
  gLP_.y = gLP_.y*(1.f-aLP) + ay*aLP;
  gLP_.z = gLP_.z*(1.f-aLP) + az*aLP;

  // 2) up vector is opposite gravity
  Vec3 up = vnorm_( Vec3{-gLP_.x, -gLP_.y, -gLP_.z} );
  motion_.up = up;

  // 3) linear accel (remove gravity estimate)
  Vec3 lin { ax - gLP_.x, ay - gLP_.y, az - gLP_.z };

  // Map to LED plane:
  //   X = around-cylinder (lateral shake),
  //   Y = vertical along up (used for stoke).
  float lateralX = lin.x;                 // adjust if your axes differ
  float vertical = vdot_(lin, up);        // upward accel along world up

  // 4) yaw rate around up (spin) contributes to wind
  Vec3 gyro {gx, gy, gz};                 // rad/s
  float yawRate = vdot_(gyro, up);        // spin around up

  // 5) wind: lightly damped integrator for a visible drift
  motion_.wind.x = 0.90f*motion_.wind.x + 0.10f*(cfg_.kAccel*lateralX + cfg_.kSpin*yawRate);
  // motion_.wind.y reserved (if you later want vertical drift)

  // 6) stoke: upward motion boosts base heat (0..1)
  float s = vertical * cfg_.kStoke;
  if (s < 0) s = 0;
  motion_.stoke = 0.85f*motion_.stoke + 0.15f*s;
  if (motion_.stoke > cfg_.stokeMax) motion_.stoke = cfg_.stokeMax;

  return true;
}
