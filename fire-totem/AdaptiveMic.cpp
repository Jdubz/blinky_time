#include "AdaptiveMic.h"

int16_t AdaptiveMic::sampleBuffer[sampleBufferSize];
volatile int AdaptiveMic::samplesRead = 0;
int AdaptiveMic::currentGain = 20;  // start midrange gain

AdaptiveMic::AdaptiveMic()
    : env(0), envMean(0), minEnv(1e6), maxEnv(0), lastCalibMillis(0) {}

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
    PDM.read(sampleBuffer, bytesAvailable);
    samplesRead = bytesAvailable / 2; // 16-bit samples
}

void AdaptiveMic::update() {
    if (samplesRead == 0) return;

    // Simple envelope follower
    float absSum = 0;
    for (int i = 0; i < samplesRead; i++) {
        absSum += abs(sampleBuffer[i]);
    }
    float avg = absSum / samplesRead;

    // exponential smoothing for envelope
    env = 0.9f * env + 0.1f * avg;

    // update adaptive min/max
    if (env < minEnv) minEnv = env;
    if (env > maxEnv) maxEnv = env;

    // long-term mean
    envMean = 0.999f * envMean + 0.001f * env;

    // periodically recalibrate range
    if (millis() - lastCalibMillis > 5000) {
        calibrate();
        lastCalibMillis = millis();
    }

    samplesRead = 0;
}

float AdaptiveMic::getLevel() const {
    // normalize envelope to 0..1 based on min/max
    float norm = (env - minEnv) / (maxEnv - minEnv + 1e-6f);
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    return norm;
}

void AdaptiveMic::calibrate() {
    // slowly adapt hardware gain toward target env
    float target = 1000; // desired mid-level envelope
    if (envMean < target * 0.5f && currentGain < 64) {
        currentGain += 3;
        PDM.setGain(currentGain);
    } else if (envMean > target * 1.5f && currentGain > 0) {
        currentGain -= 3;
        PDM.setGain(currentGain);
    }

    // reset min/max so they track the new range
    minEnv = env;
    maxEnv = env;
}
