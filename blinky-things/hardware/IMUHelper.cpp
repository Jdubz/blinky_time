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
    return ok;
}

bool IMUHelper::updateMotion(float dt) {
   float ax, ay, az, gx, gy, gz;
   if (!readAccelGyro_(ax, ay, az, gx, gy, gz) || dt <= 0.f) {
     // No fresh data available
     return false;
   }
   return updateMotionWithRaw(ax, ay, az, gx, gy, gz, dt);
}

bool IMUHelper::updateMotionWithRaw(float ax, float ay, float az,
                                    float gx, float gy, float gz,
                                    float dt) {
  if (dt <= 0.f) return false;

  // Simplified gravity estimation
  float aLP = (cfg_.tauLP > 0.f) ? (1.f - expf(-dt / cfg_.tauLP)) : 1.f;
  Vec3 rawAccel = {ax, ay, az};
  float accelMagnitude = vlen_(rawAccel);

  // Only update gravity when acceleration is close to 1G (not during motion)
  if (accelMagnitude > 0.8f && accelMagnitude < 1.2f) {
    gLP_.x = gLP_.x * (1.f - aLP) + ax * aLP;
    gLP_.y = gLP_.y * (1.f - aLP) + ay * aLP;
    gLP_.z = gLP_.z * (1.f - aLP) + az * aLP;
  }

  // Update basic motion state for fire effect compatibility
  updateBasicOrientation_(rawAccel, dt);
  updateSimpleMotion_(rawAccel, {gx, gy, gz}, dt);

  return true;
}

void IMUHelper::updateBasicOrientation_(const Vec3& accel, float dt) {
    // Calculate up vector (normalized gravity)
    float gravMag = vlen_(gLP_);
    if (gravMag > 0.1f) {
        motion_.up = vmul_(gLP_, 1.0f / gravMag);
    } else {
        motion_.up = {0, 0, 1}; // Default up
    }

    // Calculate tilt angle from vertical
    float upZ = motion_.up.z;
    motion_.tiltAngle = acos(constrain(fabsf(upZ), 0.0f, 1.0f)) * 180.0f / M_PI;
}

void IMUHelper::updateSimpleMotion_(const Vec3& accel, const Vec3& gyro, float dt) {
    // Simple motion detection
    Vec3 linAccel = vsub_(accel, gLP_);
    float linearMag = vlen_(linAccel);
    float gyroMag = vlen_(gyro);

    motion_.motionIntensity = linearMag + gyroMag * 0.1f;
    motion_.isStationary = motion_.motionIntensity < 1.0f;

}

// Clean IMU data interface implementation
bool IMUHelper::updateIMUData() {
    if (!imuReady) return false;

    // Get raw sensor readings
    float ax, ay, az, gx, gy, gz;
    if (!readAccelGyro_(ax, ay, az, gx, gy, gz)) {
        return false;
    }

    // Store raw data
    imuData_.accel = {ax, ay, az};
    imuData_.gyro = {gx, gy, gz};
    imuData_.temp = getTempC();
    imuData_.timestamp = millis();

    // Calculate gravity estimate using immediate accelerometer reading
    // For orientation detection, we want responsive updates
    static Vec3 gravityEstimate = {0, 0, -9.81f}; // Initialize with default
    static bool firstReading = true;

    float accelMag = sqrt(ax*ax + ay*ay + az*az);

    if (firstReading) {
        // Initialize with first reading
        gravityEstimate.x = ax;
        gravityEstimate.y = ay;
        gravityEstimate.z = az;
        firstReading = false;
    } else {
        // Use fast update for responsive orientation (higher alpha = more responsive)
        const float alpha = 0.3f; // More responsive than before

        // Update gravity estimate when acceleration is reasonable (0.8 to 1.2 in sensor units)
        if (accelMag > 0.8f && accelMag < 1.2f) {
            gravityEstimate.x = gravityEstimate.x * (1.0f - alpha) + ax * alpha;
            gravityEstimate.y = gravityEstimate.y * (1.0f - alpha) + ay * alpha;
            gravityEstimate.z = gravityEstimate.z * (1.0f - alpha) + az * alpha;
        }
    }

    imuData_.gravity = gravityEstimate;

    // Calculate linear acceleration (accel with gravity removed)
    imuData_.linearAccel = {
        ax - gravityEstimate.x,
        ay - gravityEstimate.y,
        az - gravityEstimate.z
    };

    // Calculate up vector (normalized gravity - NOT negative)
    // Your accelerometer reads positive when pointing up, so gravity = up direction
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
        imuData_.up = {0, 0, 1}; // Default up
    }

    // Calculate tilt angle from vertical (0° = upright, 90° = on side)
    float upZ = imuData_.up.z;
    imuData_.tiltAngle = acos(constrain(fabsf(upZ), 0.0f, 1.0f)) * 180.0f / M_PI;

    // Simple motion detection
    float linearMag = sqrt(imuData_.linearAccel.x*imuData_.linearAccel.x +
                          imuData_.linearAccel.y*imuData_.linearAccel.y +
                          imuData_.linearAccel.z*imuData_.linearAccel.z);
    float gyroMag = sqrt(gx*gx + gy*gy + gz*gz);

    imuData_.motionMagnitude = linearMag + gyroMag * 10.0f; // Scale gyro for comparison
    imuData_.isMoving = imuData_.motionMagnitude > 1.0f;

    return true;
}
