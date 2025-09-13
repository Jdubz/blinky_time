#include "AdaptiveMic.h"

AdaptiveMic* AdaptiveMic::instance = nullptr;

AdaptiveMic::AdaptiveMic(uint8_t channels, uint32_t sampleRate) {
    instance = this;
    PDM.begin(channels, sampleRate);
    PDM.onReceive(onPDMdataStatic);
    PDM.setGain(hwGain);
}

void AdaptiveMic::begin() {
    // Already initialized in constructor, but could add error checking here
}

void AdaptiveMic::onPDMdataStatic() {
    if (instance) instance->onPDMdata();
}

void AdaptiveMic::onPDMdata() {
    int bytesAvailable = PDM.available();
    PDM.read((int16_t *)&micBuffer[bufferIndex], bytesAvailable);
    bufferIndex += bytesAvailable / 2;
    if (bufferIndex >= BUFFER_SIZE) bufferIndex = 0;
}

void AdaptiveMic::update() {
    // Calculate RMS
    float sumSq = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        float s = micBuffer[i] / 32768.0;
        sumSq += s * s;
    }
    float rms = sqrt(sumSq / BUFFER_SIZE);

    // Accumulate for hardware gain adjustment
    rmsAccumulator += rms;
    sampleCount++;

    // Short-term software AGC
    float adjRMS = rms * softGain;
    float gainError = targetRMS - adjRMS;
    softGain += gainError * softGainSmoothing;
    softGain = constrain(softGain, 0.1, 10.0);

    micLevel = constrain(adjRMS * 3.0, 0.05, 3.0);

    // Slow hardware gain adjustment every ~1s
    if (millis() - lastGainAdjust > 1000) {
        float avgRMS = rmsAccumulator / (float)sampleCount;
        rmsAccumulator = 0;
        sampleCount = 0;
        lastGainAdjust = millis();

        if (avgRMS < 0.05 && hwGain < hwGainMax) {
            hwGain += 8;
            PDM.setGain(hwGain);
        }
        else if (avgRMS > 0.9 && hwGain > hwGainMin) {
            hwGain -= 8;
            PDM.setGain(hwGain);
        }
    }
}

float AdaptiveMic::getLevel() const {
    return micLevel;
}

float AdaptiveMic::getNormalizedRMS() const {
    return micLevel / 3.0;
}

int AdaptiveMic::getHardwareGain() const {
    return hwGain;
}

void AdaptiveMic::setTargetRMS(float target) {
    targetRMS = target;
}

void AdaptiveMic::setHardwareGainLimits(int minGain, int maxGain) {
    hwGainMin = minGain;
    hwGainMax = maxGain;
}
