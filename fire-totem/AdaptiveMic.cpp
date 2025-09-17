#include "AdaptiveMic.h"
#include "TotemDefaults.h"
#include <math.h>

int16_t AdaptiveMic::sampleBuffer[sampleBufferSize];
volatile int AdaptiveMic::samplesRead = 0;
int AdaptiveMic::currentGain = 20;

AdaptiveMic::AdaptiveMic()
    : env(0.0f), envAR(0.0f), envMean(0.0f),
      minEnv(0.0f), maxEnv(0.0f), lastAvgAbs(0.0f),
      lastCalibMillis(0) {}

bool AdaptiveMic::begin() {
    // Register ISR BEFORE begin() so samples flow immediately
    PDM.onReceive(onPDMdata);
    PDM.setBufferSize(sampleBufferSize);
    if (!PDM.begin(1, 16000)) {
        return false;
    }
    PDM.setGain(currentGain);

    // Seed normalization window with current envelope
    minEnv = envAR;
    maxEnv = envAR;

    lastCalibMillis = millis();
    return true;
}

void AdaptiveMic::onPDMdata() {
    int bytesAvailable = PDM.available();
    if (bytesAvailable <= 0) return;
    if (bytesAvailable > (int)sizeof(sampleBuffer)) {
        bytesAvailable = sizeof(sampleBuffer);
    }
    PDM.read(sampleBuffer, bytesAvailable);
    samplesRead = bytesAvailable / 2;
}

void AdaptiveMic::update(float dt) {
    if (samplesRead <= 0) return;

    // 1) absolute-average of the buffer (raw units)
    float absSum = 0.0f;
    for (int i = 0; i < samplesRead; ++i) {
        absSum += fabsf((float)sampleBuffer[i]);
    }
    float avg = absSum / (float)max(1, samplesRead);
    lastAvgAbs = avg;

    // 2) fast smoothing and very long-term mean
    env     = 0.9f   * env     + 0.1f   * avg;
    envMean = 0.999f * envMean + 0.001f * env;

    // 3) track min/max for normalization window
    if (env < minEnv) minEnv = env;
    if (env > maxEnv) maxEnv = env;

    // 4) attack/release envelope in seconds
    const float aA = (attackTau  > 0.f) ? (1.f - expf(-dt / attackTau )) : 1.f;
    const float aR = (releaseTau > 0.f) ? (1.f - expf(-dt / releaseTau)) : 1.f;
    if (env > envAR) envAR += aA * (env - envAR);
    else             envAR += aR * (env - envAR);

    // 5) occasional hardware gain calibration (gentle)
    if (millis() - lastCalibMillis > 5000UL) {
        calibrate();
        lastCalibMillis = millis();
    }

    // consume this batch
    samplesRead = 0;
}

float AdaptiveMic::getLevel() const {
    float denom = (maxEnv - minEnv);
    if (denom < 1e-6f || !isfinite(denom)) {
        return 0.0f; // not enough range yet
    }

    // Normalize AR envelope into 0..1
    float n = (envAR - minEnv) / denom;
    if (!isfinite(n)) n = 0.0f;
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    // Noise gate (post-normalization)
    if (n <= noiseGate) n = 0.0f;

    // Software gain
    n *= globalGain;
    if (n > 1.0f) n = 1.0f;

    // Gamma shaping
    if (n > 0.0f) n = powf(n, gamma);
    if (!isfinite(n)) n = 0.0f;

    return constrain(n, 0.0f, 1.0f);
}

// Small, robust software auto-gain loop.
// Uses pre-gamma normalized level so it adapts to musical phrasing faster.
void AdaptiveMic::autoGainTick(float dt) {
    if (!autoGainEnabled || dt <= 0.f) return;

    float denom = (maxEnv - minEnv);
    if (denom < 1e-6f || !isfinite(denom)) return; // not ready yet

    // Pre-gamma, pre-gate normalized value (clamped 0..1)
    float n0 = (envAR - minEnv) / denom;
    if (!isfinite(n0)) n0 = 0.0f;
    if (n0 < 0.0f) n0 = 0.0f;
    if (n0 > 1.0f) n0 = 1.0f;

    // Integrate toward target occupancy
    // err = (target - current); positive err => increase gain
    float err = agTarget - n0;
    globalGain += agStrength * err * dt;

    // Clamp globalGain
    if (globalGain < agMin) globalGain = agMin;
    if (globalGain > agMax) globalGain = agMax;
}

void AdaptiveMic::calibrate() {
    // Gentle hardware gain target around nominal raw mean
    const float target = 1000.0f;

    if (envMean < target * 0.5f && currentGain < 64) {
        currentGain = min(64, currentGain + 2);
        PDM.setGain(currentGain);
    } else if (envMean > target * 1.5f && currentGain > 0) {
        currentGain = max(0, currentGain - 2);
        PDM.setGain(currentGain);
    }

    // Reset min/max to adapt to new hardware gain environment
    minEnv = envAR;
    maxEnv = envAR;
}
