#pragma once
#include <Arduino.h>

class AdaptiveMic {
public:
    AdaptiveMic();
    void begin();
    void update();
    float getLevel();

private:
    static void onPDMdata();
    static volatile int16_t sampleBuffer[512];
    static volatile int sampleBufferSize;

    bool micReady = false;

    // Level tracking
    float envelope   = 0.0f;
    float envMean    = 0.0f;
    float minEnv     = 1.0f;
    float maxEnv     = 0.0f;
    float recentPeak = 0.0f;

    // Calibration
    bool calibrated = false;
    unsigned long calibStart = 0;
    unsigned long lastSoundTime = 0;

    // Gain control
    int hwGain = 50;             // starting gain
    unsigned long lastGainAdjust = 0;

    // Debug
    unsigned long lastPrint = 0;
};
