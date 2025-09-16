#include "AdaptiveMic.h"
#include "TotemDefaults.h"

int16_t AdaptiveMic::sampleBuffer[sampleBufferSize];
volatile int AdaptiveMic::samplesRead = 0;
int AdaptiveMic::currentGain = 20;

AdaptiveMic::AdaptiveMic()
    : env(0), envAR(0), envMean(0), minEnv(1e6), maxEnv(0), lastCalibMillis(0) {}

bool AdaptiveMic::begin() {
    if (!PDM.begin(1, 16000)) { // mono, 16kHz
        return false;
    }
    PDM.setBufferSize(sampleBufferSize);
    PDM.onReceive(onPDMdata);
    PDM.setGain(currentGain);
    lastCalibMillis = millis();
    return true;
}

void AdaptiveMic::onPDMdata() {
    int bytesAvailable = PDM.available();
    if (bytesAvailable <= 0) return;
    PDM.read(sampleBuffer, bytesAvailable);
    samplesRead = bytesAvailable / 2;
}

void AdaptiveMic::update(float dt) {
    if (samplesRead <= 0) return;

    // Envelope on absolute amplitude
    float absSum = 0.0f;
    for (int i = 0; i < samplesRead; ++i) {
        absSum += fabsf((float)sampleBuffer[i]);
    }
    float avg = absSum / max(1, samplesRead);

    // raw smoothed env
    env = 0.9f * env + 0.1f * avg;

    // long-term mean
    envMean = 0.999f * envMean + 0.001f * env;

    // min/max for normalization
    if (env < minEnv) minEnv = env;
    if (env > maxEnv) maxEnv = env;

    // attack/release envelope (in sample/update time)
    float aA = (attackTau > 0) ? (1.0f - expf(-dt / attackTau)) : 1.0f;
    float aR = (releaseTau > 0) ? (1.0f - expf(-dt / releaseTau)) : 1.0f;
    if (env > envAR) envAR = envAR + aA * (env - envAR);
    else             envAR = envAR + aR * (env - envAR);

    // periodic hardware gain calibration
    if (millis() - lastCalibMillis > 5000) {
        calibrate();
        lastCalibMillis = millis();
    }

    samplesRead = 0;
}

float AdaptiveMic::getLevel() const {
    // Normalize using min/max, then gate, gain, and gamma
    float n = (envAR - minEnv) / (maxEnv - minEnv + 1e-6f);
    if (n < 0) n = 0;
    if (n < noiseGate) n = 0.0f;

    n *= globalGain;
    if (n > 1.0f) n = 1.0f;

    // gamma curve (protect against pow(0, <1))
    n = (n <= 0.0f) ? 0.0f : powf(n, gamma);
    return constrain(n, 0.0f, 1.0f);
}

void AdaptiveMic::calibrate() {
    // Aim to keep envMean near a target band by nudging hardware gain
    const float target = 1000.0f;
    if (envMean < target * 0.5f && currentGain < 64) {
        currentGain = min(64, currentGain + 3);
        PDM.setGain(currentGain);
    } else if (envMean > target * 1.5f && currentGain > 0) {
        currentGain = max(0, currentGain - 3);
        PDM.setGain(currentGain);
    }

    // Reset min/max to track new range
    minEnv = envAR;
    maxEnv = envAR;
}
