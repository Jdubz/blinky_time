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

    void update(float dt);
    float getLevel() const;   // 0..1 after gate/gamma/gain & AR env

    int   getGain() const { return currentGain; }
    float getEnv()  const { return env; }
    float getEnvMean() const { return envMean; }
    float getEnvMin()  const { return minEnv; }
    float getEnvMax()  const { return maxEnv; }

    // Tunables (now accessible to SerialConsole)
    float noiseGate  = Defaults::NoiseGate;
    float gamma      = Defaults::Gamma;
    float globalGain = Defaults::GlobalGain;
    float attackTau  = Defaults::AttackTau;
    float releaseTau = Defaults::ReleaseTau;

private:
    static constexpr int sampleBufferSize = 512;
    static int16_t sampleBuffer[sampleBufferSize];
    static volatile int samplesRead;

    static int currentGain;

    // envelope tracking
    float env;       // fast absolute-average (raw)
    float envAR;     // attack/release envelope used for output
    float envMean;   // long-term average
    float minEnv;
    float maxEnv;

    unsigned long lastCalibMillis;

    void calibrate();
};

#endif
