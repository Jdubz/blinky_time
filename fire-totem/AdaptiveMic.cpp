#include "AdaptiveMic.h"
#include <Arduino.h>
#include <PDM.h>

volatile int16_t AdaptiveMic::sampleBuffer[512];
volatile int AdaptiveMic::sampleBufferSize = 0;

void AdaptiveMic::onPDMdata() {
    int bytes = PDM.available();
    if (bytes > 0) {
        PDM.read((void*)sampleBuffer, bytes);
        sampleBufferSize = bytes / 2;
    }
}

AdaptiveMic::AdaptiveMic() {}

void AdaptiveMic::begin() {
    delay(500);  // let mic power up
    PDM.onReceive(AdaptiveMic::onPDMdata);
    PDM.setBufferSize(512);
    PDM.setGain(hwGain);
    if (!PDM.begin(1, 16000)) {
        Serial.println("PDM init failed — mic disabled");
        micReady = false;
        return;
    }
    micReady = true;
    Serial.println("Mic started");
}

void AdaptiveMic::update() {
    if (!micReady) return;

    if (sampleBufferSize == 0) {
        // silence — decay envelope slowly
        envelope *= 0.95f;
        if (envelope < 1e-6f) envelope = 0;
    } else {
        int size;
        noInterrupts();
        size = sampleBufferSize;
        sampleBufferSize = 0;
        interrupts();
        if (size <= 0) return;

        long sumSq = 0;
        int16_t maxS = 0;
        for (int i = 0; i < size; i++) {
            int16_t s = sampleBuffer[i];
            if (abs(s) > maxS) maxS = abs(s);
            sumSq += (long)s * (long)s;
        }

        if (sumSq > 0) {
            float rms = sqrtf((float)sumSq / (float)size) / 32768.0f;
            if (!isfinite(rms)) rms = 0;

            float peak = (float)maxS / 32768.0f;
            if (!isfinite(peak)) peak = 0;

            // Envelope follower
            const float attack = 0.6f, release = 0.05f;
            if (!isfinite(envelope)) envelope = 0;
            if (rms > envelope)
                envelope = attack * rms + (1 - attack) * envelope;
            else
                envelope = release * rms + (1 - release) * envelope;

            if (envelope < 0) envelope = 0;

            if (!isfinite(envMean)) envMean = 0;
            envMean = envMean * 0.995f + envelope * 0.005f;
            if (envMean < 0) envMean = 0;

            // Median filter recent peaks
            static float peakBuf[5] = {0};
            static int peakIdx = 0;
            peakBuf[peakIdx] = peak;
            peakIdx = (peakIdx + 1) % 5;

            float tmp[5];
            memcpy(tmp, peakBuf, sizeof(tmp));
            for (int i = 0; i < 5; i++) for (int j = i+1; j < 5; j++)
                if (tmp[j] < tmp[i]) { float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
            float medPeak = tmp[2];

            if (!isfinite(recentPeak)) recentPeak = 0;
            recentPeak = max(recentPeak * 0.9f, medPeak);

            lastSoundTime = millis(); // mark that we heard sound
        }
    }

    // --- Auto calibration ---
    if (!calibrated) {
        if (calibStart == 0) calibStart = millis();
        if (envelope < minEnv) minEnv = envelope;
        if (envelope > maxEnv) maxEnv = envelope;
        if (millis() - calibStart > 3000) {
            if (minEnv < 0) minEnv = 0;
            if (maxEnv < 0.001f) maxEnv = 0.001f;
            calibrated = true;
            Serial.println("Mic calibration complete");
        }
    } else {
        // If silent for 60s, re-calibrate
        if (millis() - lastSoundTime > 60000) {
            calibrated = false;
            minEnv = 1.0f;
            maxEnv = 0.0f;
            calibStart = 0;
            Serial.println("Mic re-calibrating...");
        }
    }

    // --- Gain logic ---
    if (millis() - lastGainAdjust > 5000) {
        const float loudThresh = 0.75f;
        const float quietThresh = 0.01f;

        if (recentPeak > loudThresh && hwGain > 10) {
            hwGain = max(hwGain - 1, 10);
            PDM.setGain(hwGain);
        } else if (envMean < quietThresh && hwGain < 80) {
            hwGain = min(hwGain + 1, 80);
            PDM.setGain(hwGain);
        }

        recentPeak = 0.0f;
        lastGainAdjust = millis();
    }

    // Debug
    if (millis() - lastPrint > 1000) {
        Serial.print("lvl="); Serial.print(getLevel(), 3);
        Serial.print(" env="); Serial.print(envelope, 4);
        Serial.print(" mean="); Serial.print(envMean, 4);
        Serial.print(" gain="); Serial.println(hwGain);
        lastPrint = millis();
    }
}

float AdaptiveMic::getLevel() {
    if (!calibrated) return 0.0f;

    if (!isfinite(envelope) || envelope < 0) envelope = 0;
    if (!isfinite(envMean)  || envMean  < 0) envMean  = 0;
    if (envMean < 1e-5f) envMean = 1e-5f;

    if (!isfinite(minEnv) || minEnv < 0) minEnv = 0;
    if (!isfinite(maxEnv) || maxEnv < 0.001f) maxEnv = 0.001f;

    float dynMin = min(minEnv, envMean);
    float dynMax = max(maxEnv, envMean * 2.0f);
    if (dynMax <= dynMin) dynMax = dynMin + 1e-5f;

    float norm = (envelope - dynMin) / (dynMax - dynMin);
    if (!isfinite(norm) || norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    return norm;
}
