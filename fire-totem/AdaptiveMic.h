#pragma once
#include <Arduino.h>

class AdaptiveMic {
public:
    AdaptiveMic();
    void begin();
    void update();
    float getLevel();
    float getEnvelope() const { return envelope; }
    float getGain() const { return currentGain; }

private:
    static void onPDMdata();
    static volatile int16_t sampleBuffer[512];
    static volatile int sampleBufferSize;

    bool micReady = false;

    float envelope = 0.0f;
    float envMean = 0.0f;
    float minEnv = 1.0f;
    float maxEnv = 0.0f;
    float recentPeak = 0.0f;

    int currentGain = 50;
    unsigned long lastGainAdjust = 0;
};
