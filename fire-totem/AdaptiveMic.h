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

    // Normalized 0..1 level after gate/gain/gamma
    float getLevel() const;

    // ---- Debug / telemetry (for SerialConsole mic debug) ----
    int   getGain()     const { return currentGain; }
    float getEnv()      const { return env; }          // fast smoothed abs-avg
    float getEnvAR()    const { return envAR; }        // attack/release envelope
    float getEnvMean()  const { return envMean; }
    float getEnvMin()   const { return minEnv; }       // running min (floor)
    float getEnvMax()   const { return maxEnv; }       // running max (peak)
    float getFloor()    const { return minEnv; }       // alias for console
    float getPeak()     const { return maxEnv; }       // alias for console
    float getAvgAbs()   const { return lastAvgAbs; }   // raw abs-avg of last buffer

    // Tunables (wired to Defaults)
    float noiseGate  = Defaults::NoiseGate;   // 0..1 gate after normalization
    float gamma      = Defaults::Gamma;       // perceptual curve
    float globalGain = Defaults::GlobalGain;  // software gain after gate
    float attackTau  = Defaults::AttackTau;   // seconds
    float releaseTau = Defaults::ReleaseTau;  // seconds

    // ---- Software auto-gain controls (NEW) ----
    bool  autoGainEnabled = true;
    float agTarget   = Defaults::AutoGainTarget;
    float agStrength = Defaults::AutoGainStrength;
    float agMin      = Defaults::AutoGainMin;
    float agMax      = Defaults::AutoGainMax;

    // Call every frame; gently nudges globalGain
    void autoGainTick(float dt);


private:
    // PDM buffer
    static constexpr int sampleBufferSize = 512;
    static int16_t sampleBuffer[sampleBufferSize];
    static volatile int samplesRead;

    // Hardware gain (0..64 typical)
    static int currentGain;

    // Envelopes & stats (raw int16 abs-average units)
    float env;         // smoothed absolute average (fast)
    float envAR;       // attack/release envelope
    float envMean;     // very long-term mean
    float minEnv;      // running min
    float maxEnv;      // running max
    float lastAvgAbs;  // raw abs-avg from latest ISR batch (for debug)

    unsigned long lastCalibMillis;

    void calibrate();
};

#endif
