#pragma once
#include <Arduino.h>
#include <PDM.h>

class AdaptiveMic {
public:
    AdaptiveMic(uint8_t channels = 1, uint32_t sampleRate = 16000);

    void begin();
    void update();
    float getLevel() const;         // short-term mic level (0.0–1.0+)
    float getNormalizedRMS() const; // RMS after gain
    int   getHardwareGain() const;

    void setTargetRMS(float target);
    void setHardwareGainLimits(int minGain, int maxGain);

private:
    static void onPDMdataStatic();
    void onPDMdata();

    static AdaptiveMic* instance;

    // Mic data
    static const int BUFFER_SIZE = 256;
    volatile int16_t micBuffer[BUFFER_SIZE];
    volatile int bufferIndex = 0;

    // Gain control
    float softGain = 1.0;
    float targetRMS = 0.5;
    float softGainSmoothing = 0.002;
    int hwGain = 127;
    int hwGainMin = 10;
    int hwGainMax = 250;

    // Long-term stats
    float rmsAccumulator = 0;
    unsigned long sampleCount = 0;
    unsigned long lastGainAdjust = 0;

    // Results
    float micLevel = 0;
};
