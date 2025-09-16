#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\IMUHelper.cpp"
#include "IMUHelper.h"
#include <Wire.h>
#include <LSM6DS3.h>

#define IMU_ADDR 0x6A
LSM6DS3 senseIMU(I2C_MODE, IMU_ADDR);

IMUHelper::IMUHelper() {}

bool IMUHelper::begin() {
    Wire.begin();
    delay(500);

    if (senseIMU.begin() != 0) {
        Serial.println("IMU Device error");
        imuReady = false;
        return false;
    } else {
        Serial.println("IMU Device OK!");
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
