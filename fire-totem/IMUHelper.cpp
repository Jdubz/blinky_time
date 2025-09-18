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

  // Store raw sensor data in motion history
  accelHistory_[historyIndex_] = {ax, ay, az};
  gyroHistory_[historyIndex_] = {gx, gy, gz};
  historyIndex_ = (historyIndex_ + 1) % MOTION_HISTORY_SIZE;

  // 1) Enhanced gravity estimation with motion rejection
  float aLP = (cfg_.tauLP > 0.f) ? (1.f - expf(-dt / cfg_.tauLP)) : 1.f;

  // Only update gravity estimate when motion is low to avoid corruption
  Vec3 rawAccel = {ax, ay, az};
  float accelMagnitude = vlen_(rawAccel);
  float gravityError = abs(accelMagnitude - 9.81f);

  if (gravityError < cfg_.gravityThresh) {
    // Low motion - update gravity estimate
    gLP_.x = gLP_.x * (1.f - aLP) + ax * aLP;
    gLP_.y = gLP_.y * (1.f - aLP) + ay * aLP;
    gLP_.z = gLP_.z * (1.f - aLP) + az * aLP;
  }

  // 2) Calculate linear acceleration (remove gravity)
  Vec3 linAccel = vsub_(rawAccel, gLP_);
  Vec3 angularVel = {gx, gy, gz};

  // 3) Update torch orientation and physics
  updateTorchOrientation_(rawAccel, angularVel, dt);
  updateWindPhysics_(linAccel, angularVel, dt);
  updateRotationalEffects_(angularVel, dt);
  updateStokePhysics_(linAccel, motion_.velocity, dt);
  updateMotionAnalysis_(rawAccel, angularVel, dt);
  applyMotionSmoothing_(dt);

  // Store for next frame
  prevAccel_ = rawAccel;
  prevAngularVel_ = angularVel;

  return true;
}

// ======== Simplified Motion Implementation ========

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

    // Legacy wind/stoke for fire effect compatibility (minimal values)
    motion_.wind.x *= 0.9f; // Decay existing values
    motion_.wind.y *= 0.9f;
    motion_.stoke *= 0.85f;

    // Add minimal motion if detected
    if (!motion_.isStationary) {
        motion_.wind.x += linAccel.x * cfg_.kAccel;
        motion_.wind.y += linAccel.y * cfg_.kAccel;
        motion_.stoke = constrain(motion_.stoke + linAccel.z * cfg_.kStoke, 0.0f, 1.0f);
    }
}

// Clean IMU data interface implementation
// Now update the legacy motion state with the simplified methods
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

// Clean IMU data interface implementation
bool IMUHelper::updateIMUData() {
    if (!imuReady) return false;
    rawVelocity_ = vadd_(rawVelocity_, deltaV);

    // Apply velocity damping (air resistance)
    float velocityDamping = 0.95f;
    rawVelocity_ = vmul_(rawVelocity_, velocityDamping);

    // Calculate flame velocity with inertia lag
    float flameResponsiveness = 1.0f - cfg_.flameInertia;
    flameVelocity_.x = flameVelocity_.x * cfg_.flameInertia + rawVelocity_.x * flameResponsiveness;
    flameVelocity_.y = flameVelocity_.y * cfg_.flameInertia + rawVelocity_.y * flameResponsiveness;
    flameVelocity_.z = flameVelocity_.z * cfg_.flameInertia + rawVelocity_.z * flameResponsiveness;

    motion_.velocity = flameVelocity_;

    // Convert to wind effect in torch coordinate system
    // X = around cylinder (left/right), Y = up/down
    Vec3 torchSpaceAccel = {
        linAccel.x,  // lateral (around cylinder)
        linAccel.z,  // forward/back (along cylinder axis in horizontal plane)
        linAccel.y   // vertical
    };

    Vec3 torchSpaceVel = {
        motion_.velocity.x,
        motion_.velocity.z,
        motion_.velocity.y
    };

    // Calculate wind from acceleration and velocity
    float windX = cfg_.kAccel * torchSpaceAccel.x + cfg_.kVelocity * torchSpaceVel.x;
    float windY = cfg_.kAccel * torchSpaceAccel.y + cfg_.kVelocity * torchSpaceVel.y;

    // Add rotational contribution to wind
    float yawRate = vdot_(angularVel, motion_.up);
    float pitchRate = angularVel.x; // Around X axis
    float rollRate = angularVel.z;  // Around Z axis

    windX += cfg_.kSpin * (yawRate + rollRate * 0.5f);
    windY += cfg_.kSpin * (pitchRate * 0.3f); // Less influence on vertical

    // Apply wind damping and limits
    motion_.wind.x = motion_.wind.x * cfg_.windDamping + windX * (1.0f - cfg_.windDamping);
    motion_.wind.y = motion_.wind.y * cfg_.windDamping + windY * (1.0f - cfg_.windDamping);

    // Clamp to maximum wind speed
    float windMag = sqrt(motion_.wind.x * motion_.wind.x + motion_.wind.y * motion_.wind.y);
    if (windMag > cfg_.maxWindSpeed) {
        motion_.wind.x = (motion_.wind.x / windMag) * cfg_.maxWindSpeed;
        motion_.wind.y = (motion_.wind.y / windMag) * cfg_.maxWindSpeed;
    }

    // Calculate flame direction (angle flames lean due to motion)
    if (windMag > 0.1f) {
        motion_.flameDirection = atan2(motion_.wind.y, motion_.wind.x) * 180.0f / M_PI;
        motion_.flameBend = constrain(windMag / cfg_.maxWindSpeed, 0.0f, 1.0f);
    } else {
        motion_.flameBend *= 0.9f; // Decay when no motion
    }

    // Calculate inertial drift effects
    motion_.inertiaDrift.x = motion_.inertiaDrift.x * 0.95f + motion_.velocity.x * 0.05f;
    motion_.inertiaDrift.y = motion_.inertiaDrift.y * 0.95f + motion_.velocity.z * 0.05f;
}

void IMUHelper::updateRotationalEffects_(const Vec3& angularVel, float dt) {
    // Smooth angular velocity
    float angularSmooth = cfg_.smoothingFactor;
    motion_.angularVel.x = motion_.angularVel.x * angularSmooth + angularVel.x * (1.0f - angularSmooth);
    motion_.angularVel.y = motion_.angularVel.y * angularSmooth + angularVel.y * (1.0f - angularSmooth);
    motion_.angularVel.z = motion_.angularVel.z * angularSmooth + angularVel.z * (1.0f - angularSmooth);

    motion_.smoothAngularVel = motion_.angularVel;

    // Calculate overall spin magnitude
    motion_.spinMagnitude = vlen_(motion_.smoothAngularVel);

    // Calculate centrifugal force effects
    Vec3 centrifugal = calculateCentrifugalForce_(motion_.smoothAngularVel);
    motion_.centrifugalForce = vlen_(centrifugal) * cfg_.centrifugalFactor;

    // Add Coriolis effects for spiral patterns
    Vec3 coriolis = calculateCoriolisEffect_(motion_.velocity, motion_.smoothAngularVel);

    // Apply rotational damping
    motion_.angularVel = vmul_(motion_.angularVel, cfg_.spinDamping);

    // Influence flame turbulence based on rotation
    motion_.turbulenceLevel = constrain(motion_.spinMagnitude * 0.3f + motion_.centrifugalForce, 0.0f, 1.0f);
}

void IMUHelper::updateStokePhysics_(const Vec3& linAccel, const Vec3& velocity, float dt) {
    // Calculate upward acceleration and velocity components
    float upwardAccel = vdot_(linAccel, motion_.up);
    float upwardVel = vdot_(velocity, motion_.up);

    // Only positive (upward) motion contributes to stoking
    float accelContrib = (upwardAccel > 0) ? upwardAccel * cfg_.kStoke : 0.0f;
    float velContrib = (upwardVel > 0) ? upwardVel * cfg_.stokeVelocityFactor : 0.0f;

    float newStoke = accelContrib + velContrib;

    // Apply stoking with enhanced dynamics
    motion_.stoke = motion_.stoke * cfg_.stokeDecay + newStoke * (1.0f - cfg_.stokeDecay);

    // Consider tilt - tilted torch gets less effective stoking
    float tiltReduction = cos(degToRad_(motion_.tiltAngle * 0.5f));
    motion_.stoke *= tiltReduction;

    // Clamp to maximum
    if (motion_.stoke > cfg_.stokeMax) {
        motion_.stoke = cfg_.stokeMax;
    }
}

void IMUHelper::updateMotionAnalysis_(const Vec3& accel, const Vec3& gyro, float dt) {
    // Calculate jerk (rate of acceleration change)
    if (vlen_(prevAccel_) > 0.001f) {
        Vec3 jerkVec = vmul_(vsub_(accel, prevAccel_), 1.0f / dt);
        motion_.jerkMagnitude = vlen_(jerkVec);

        // Limit excessive jerk for smoother response
        if (motion_.jerkMagnitude > cfg_.jerkLimit) {
            motion_.jerkMagnitude = cfg_.jerkLimit;
        }
    }

    // Calculate overall motion intensity
    float accelIntensity = constrain(vlen_(vsub_(accel, gLP_)) / 5.0f, 0.0f, 1.0f); // 5 m/s² reference
    float gyroIntensity = constrain(vlen_(gyro) / 5.0f, 0.0f, 1.0f);               // 5 rad/s reference
    float jerkIntensity = constrain(motion_.jerkMagnitude / cfg_.jerkLimit, 0.0f, 1.0f);

    motion_.motionIntensity = (accelIntensity + gyroIntensity + jerkIntensity) / 3.0f;

    // Determine if torch is stationary
    motion_.isStationary = (motion_.motionIntensity < 0.05f) &&
                          (vlen2_(motion_.wind) < 0.5f) &&
                          (motion_.spinMagnitude < 0.2f);
}

void IMUHelper::applyMotionSmoothing_(float dt) {
    // Apply smoothing to reduce jitter while maintaining responsiveness
    float smoothFactor = cfg_.smoothingFactor;

    motion_.smoothAccel.x = motion_.smoothAccel.x * smoothFactor + (prevAccel_.x - gLP_.x) * (1.0f - smoothFactor);
    motion_.smoothAccel.y = motion_.smoothAccel.y * smoothFactor + (prevAccel_.y - gLP_.y) * (1.0f - smoothFactor);
    motion_.smoothAccel.z = motion_.smoothAccel.z * smoothFactor + (prevAccel_.z - gLP_.z) * (1.0f - smoothFactor);

    // Adaptive smoothing - more smoothing when motion is erratic
    if (motion_.jerkMagnitude > cfg_.jerkLimit * 0.5f) {
        // High jerk - apply extra smoothing
        float extraSmooth = 0.85f;
        motion_.wind.x = motion_.wind.x * extraSmooth + motion_.wind.x * (1.0f - extraSmooth);
        motion_.wind.y = motion_.wind.y * extraSmooth + motion_.wind.y * (1.0f - extraSmooth);
    }
}

Vec3 IMUHelper::calculateCentrifugalForce_(const Vec3& angularVel) {
    // F_centrifugal = ω × (ω × r)
    // For a cylindrical torch, this creates outward force proportional to rotation speed
    float radius = cfg_.torchRadius * 0.0254f; // Convert inches to meters
    Vec3 radiusVec = {radius, 0, 0}; // Radial vector in torch coordinate system

    Vec3 temp = vcross_(angularVel, radiusVec);
    Vec3 centrifugalForce = vcross_(angularVel, temp);

    return centrifugalForce;
}

Vec3 IMUHelper::calculateCoriolisEffect_(const Vec3& velocity, const Vec3& angularVel) {
    // F_coriolis = -2m(Ω × v)
    // Creates spiral motion patterns in rotating reference frame
    Vec3 coriolisForce = vmul_(vcross_(angularVel, velocity), -2.0f * cfg_.coriolisFactor);

    return coriolisForce;
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
