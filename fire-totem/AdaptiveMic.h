#ifndef ADAPTIVE_MIC_H
#define ADAPTIVE_MIC_H

#include <Arduino.h>
#include <PDM.h>

class AdaptiveMic {
public:
    AdaptiveMic();

    bool begin();
    static void onPDMdata();

    void update();
    float getLevel() const;

    // Debug helpers
    int getGain() const { return currentGain; }
    float getEnv() const { return env; }
    float getEnvMean() const { return envMean; }
    float getEnvMin() const { return minEnv; }
    float getEnvMax() const { return maxEnv; }

private:
    static constexpr int sampleBufferSize = 512;
    static int16_t sampleBuffer[sampleBufferSize];
    static volatile int samplesRead;

    static int currentGain;

    float env;
    float envMean;
    float minEnv;
    float maxEnv;

    unsigned long lastCalibMillis;

    void calibrate();
};

#endif
