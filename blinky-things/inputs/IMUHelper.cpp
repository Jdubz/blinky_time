#include "IMUHelper.h"
#include <Wire.h>
#include <math.h>

// Conditional compilation: Only use LSM6DS3 if available
// To enable IMU, install the LSM6DS3 library and define IMU_ENABLED
#ifdef IMU_ENABLED
#include <LSM6DS3.h>
#define IMU_ADDR 0x6A
LSM6DS3 senseIMU(I2C_MODE, IMU_ADDR);
#endif

IMUHelper::IMUHelper() {}

bool IMUHelper::begin() {
#ifdef IMU_ENABLED
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
#else
    Serial.println(F("IMU disabled (LSM6DS3 library not installed)"));
    imuReady = false;
    return false;
#endif
}

bool IMUHelper::getAccel(float &ax, float &ay, float &az) {
#ifdef IMU_ENABLED
    if (!imuReady) return false;

    ax = senseIMU.readFloatAccelX();
    ay = senseIMU.readFloatAccelY();
    az = senseIMU.readFloatAccelZ();

    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az)) {
        ax = ay = az = 0;
        return false;
    }
    return true;
#else
    ax = ay = az = 0;
    return false;
#endif
}

bool IMUHelper::getGyro(float &gx, float &gy, float &gz) {
#ifdef IMU_ENABLED
    if (!imuReady) return false;

    gx = senseIMU.readFloatGyroX();
    gy = senseIMU.readFloatGyroY();
    gz = senseIMU.readFloatGyroZ();

    if (!isfinite(gx) || !isfinite(gy) || !isfinite(gz)) {
        gx = gy = gz = 0;
        return false;
    }
    return true;
#else
    gx = gy = gz = 0;
    return false;
#endif
}

float IMUHelper::getTempC() {
#ifdef IMU_ENABLED
    if (!imuReady) return NAN;
    return senseIMU.readTempC();
#else
    return NAN;
#endif
}

bool IMUHelper::readAccelGyro_(float& ax, float& ay, float& az,
                               float& gx, float& gy, float& gz) {
    bool ok = getAccel(ax, ay, az) && getGyro(gx, gy, gz);
    return ok;
}

bool IMUHelper::updateMotion(float dt) {
   float ax, ay, az, gx, gy, gz;
   if (!readAccelGyro_(ax, ay, az, gx, gy, gz) || dt <= 0.f) {
     return false;
   }
   return updateMotionWithRaw(ax, ay, az, gx, gy, gz, dt);
}

bool IMUHelper::updateMotionWithRaw(float ax, float ay, float az,
                                    float gx, float gy, float gz,
                                    float dt) {
  if (dt <= 0.f) return false;

  float aLP = (cfg_.tauLP > 0.f) ? (1.f - expf(-dt / cfg_.tauLP)) : 1.f;
  Vec3 rawAccel = {ax, ay, az};
  float accelMagnitude = vlen_(rawAccel);

  if (accelMagnitude > 0.8f && accelMagnitude < 1.2f) {
    gLP_.x = gLP_.x * (1.f - aLP) + ax * aLP;
    gLP_.y = gLP_.y * (1.f - aLP) + ay * aLP;
    gLP_.z = gLP_.z * (1.f - aLP) + az * aLP;
  }

  updateBasicOrientation_(rawAccel, dt);
  updateSimpleMotion_(rawAccel, {gx, gy, gz}, dt);

  return true;
}

void IMUHelper::updateBasicOrientation_(const Vec3& accel, float dt) {
    float gravMag = vlen_(gLP_);
    if (gravMag > 0.1f) {
        motion_.up = vmul_(gLP_, 1.0f / gravMag);
    } else {
        motion_.up = {0, 0, 1};
    }

    float upZ = motion_.up.z;
    motion_.tiltAngle = acos(constrain(fabsf(upZ), 0.0f, 1.0f)) * 180.0f / M_PI;
}

void IMUHelper::updateSimpleMotion_(const Vec3& accel, const Vec3& gyro, float dt) {
    Vec3 linAccel = vsub_(accel, gLP_);
    float linearMag = vlen_(linAccel);
    float gyroMag = vlen_(gyro);

    motion_.motionIntensity = linearMag + gyroMag * 0.1f;
    motion_.isStationary = motion_.motionIntensity < 1.0f;
}

bool IMUHelper::updateIMUData() {
    if (!imuReady) return false;

    float ax, ay, az, gx, gy, gz;
    if (!readAccelGyro_(ax, ay, az, gx, gy, gz)) {
        return false;
    }

    imuData_.accel = {ax, ay, az};
    imuData_.gyro = {gx, gy, gz};
    imuData_.temp = getTempC();
    imuData_.timestamp = millis();

    static Vec3 gravityEstimate = {0, 0, -9.81f};
    static bool firstReading = true;

    float accelMag = sqrt(ax*ax + ay*ay + az*az);

    if (firstReading) {
        gravityEstimate.x = ax;
        gravityEstimate.y = ay;
        gravityEstimate.z = az;
        firstReading = false;
    } else {
        const float alpha = 0.3f;
        if (accelMag > 0.8f && accelMag < 1.2f) {
            gravityEstimate.x = gravityEstimate.x * (1.0f - alpha) + ax * alpha;
            gravityEstimate.y = gravityEstimate.y * (1.0f - alpha) + ay * alpha;
            gravityEstimate.z = gravityEstimate.z * (1.0f - alpha) + az * alpha;
        }
    }

    imuData_.gravity = gravityEstimate;
    imuData_.linearAccel = {
        ax - gravityEstimate.x,
        ay - gravityEstimate.y,
        az - gravityEstimate.z
    };

    float gravMag = sqrt(gravityEstimate.x*gravityEstimate.x +
                        gravityEstimate.y*gravityEstimate.y +
                        gravityEstimate.z*gravityEstimate.z);
    if (gravMag > 0.1f) {
        imuData_.up = {
            gravityEstimate.x / gravMag,
            gravityEstimate.y / gravMag,
            gravityEstimate.z / gravMag
        };
    } else {
        imuData_.up = {0, 0, 1};
    }

    float upZ = imuData_.up.z;
    imuData_.tiltAngle = acos(constrain(fabsf(upZ), 0.0f, 1.0f)) * 180.0f / M_PI;

    float linearMag = sqrt(imuData_.linearAccel.x*imuData_.linearAccel.x +
                          imuData_.linearAccel.y*imuData_.linearAccel.y +
                          imuData_.linearAccel.z*imuData_.linearAccel.z);
    float gyroMag = sqrt(gx*gx + gy*gy + gz*gz);

    imuData_.motionMagnitude = linearMag + gyroMag * 10.0f;
    imuData_.isMoving = imuData_.motionMagnitude > 1.0f;

    return true;
}
