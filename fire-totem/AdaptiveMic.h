#pragma once
#include <Arduino.h>

class AdaptiveMic {
public:
    enum BassMode : uint8_t { BASS_BANDPASS = 0, BASS_LOWPASS = 1 };

    AdaptiveMic();
    void begin();
    void update();

    float getLevel();
    float getEnvelope() const { return envelope; }
    float getGain() const { return currentGain; }
    float getSampleRate() const { return sampleRate; }

    // Bass filter controls
    void setBassFilter(bool enabled, float centerHz = 120.0f, float q = 0.8f, BassMode mode = BASS_BANDPASS);
    void getBassFilter(bool& enabled, float& centerHz, float& q, BassMode& mode) const {
        enabled = bassEnabled; centerHz = bassFc; q = bassQ; mode = bassMode;
    }

    // >>> Make the ISR callback public so we can pass it to PDM.onReceive()
    static void onPDMdata();

private:
    // (everything else unchanged)
    static volatile int16_t sampleBuffer[512];
    static volatile int sampleBufferSize;
    bool  micReady = false;
    float envelope = 0.0f, envMean = 0.0f, minEnv = 1.0f, maxEnv = 0.0f, recentPeak = 0.0f;
    int   currentGain = 50;
    unsigned long lastGainAdjust = 0;
    float sampleRate = 16000.0f;

    bool     bassEnabled = true;
    float    bassFc = 120.0f, bassQ = 0.8f;
    BassMode bassMode = BASS_BANDPASS;

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void  updateBiquad();
    inline float processBiquad(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
