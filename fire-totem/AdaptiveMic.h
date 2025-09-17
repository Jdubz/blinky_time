#ifndef ADAPTIVE_MIC_H
#define ADAPTIVE_MIC_H

#include <Arduino.h>
#include <PDM.h>
#include "TotemDefaults.h"

class AdaptiveMic {
public:
    AdaptiveMic();

    bool begin();
    static void onPDMdata();

    // Call once per frame with elapsed seconds
    void update(float dt);

    // Normalized, gated, gamma-corrected level [0..1]
    float getLevel() const { return levelOut; }

    // Tunables (wired to your Defaults)
    float noiseGate  = Defaults::NoiseGate;   // 0..1, applied after normalization
    float gamma      = Defaults::Gamma;       // perceptual curve
    float globalGain = Defaults::GlobalGain;  // software gain after gate
    float attackTau  = Defaults::AttackTau;   // s
    float releaseTau = Defaults::ReleaseTau;  // s

    // Debug
    int   getGain()     const { return currentGain; }
    float getEnvAR()    const { return envAR; }
    float getFloor()    const { return floorEMA; }
    float getPeak()     const { return peakEMA; }
    float getAvgAbs()   const { return lastAvgAbs; }

private:
    // PDM buffer
    static constexpr int sampleBufferSize = 512;
    static int16_t sampleBuffer[sampleBufferSize];
    static volatile int samplesRead;

    // HW gain (0..64 typical)
    static int currentGain;

    // Raw envelopes (in int16 abs-average units)
    float envAR     = 0.0f;  // attack-release envelope of abs average
    float floorEMA  = 0.0f;  // noise floor (fast down, ultra slow up)
    float peakEMA   = 0.0f;  // decaying peak (fast up, slow down)
    float levelOut  = 0.0f;  // final 0..1
    float lastAvgAbs = 0.0f;

    bool  initialized = false;

    // Window stats for HW calibration
    unsigned long lastCalibMs = 0;
    uint32_t winFrames = 0;
    uint32_t winActiveRawFrames = 0; // activity measured in raw units (not normalized)
    uint32_t winClipped = 0;
    uint32_t winTotal   = 0;

    // Helpers
    void calibrateHW();
};

#endif
