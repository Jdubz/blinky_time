#pragma once
#include <Arduino.h>

class IMUHelper {
public:
    IMUHelper();

    bool begin();
    bool isReady() const { return imuReady; }

    bool getAccel(float &ax, float &ay, float &az);
    bool getGyro(float &gx, float &gy, float &gz);
    float getTempC();

private:
    bool imuReady = false;
};
