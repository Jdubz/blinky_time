#include "AdaptiveMic.h"
#include <math.h>

// ---------- static data ----------
int16_t AdaptiveMic::sampleBuffer[sampleBufferSize];
volatile int AdaptiveMic::samplesRead = 0;
int AdaptiveMic::currentGain = 20;

// ---------- ctor / begin ----------
AdaptiveMic::AdaptiveMic() {}

bool AdaptiveMic::begin() {
    if (!PDM.begin(1, 16000)) return false;  // mono, 16kHz
    PDM.setBufferSize(sampleBufferSize);
    PDM.onReceive(onPDMdata);
    PDM.setGain(currentGain);

    envAR = floorEMA = peakEMA = 0.0f;
    levelOut = 0.0f;
    initialized = false;

    lastCalibMs = millis();
    winFrames = winActiveRawFrames = winClipped = winTotal = 0;
    return true;
}

void AdaptiveMic::onPDMdata() {
    int bytes = PDM.available();
    if (bytes <= 0) return;
    PDM.read(sampleBuffer, bytes);
    samplesRead = bytes / 2;
}

// ---------- update ----------
void AdaptiveMic::update(float dt) {
    if (samplesRead <= 0) return;

    // 1) Measure instantaneous absolute average & clipping in raw units
    float absSum = 0.0f;
    uint32_t clipped = 0;
    for (int i = 0; i < samplesRead; ++i) {
        int16_t s = sampleBuffer[i];
        absSum += fabsf((float)s);
        if (abs(s) > 30000) ++clipped; // near full-scale
    }
    float avgAbs = absSum / (float)max(1, samplesRead);
    lastAvgAbs = avgAbs;

    winClipped += clipped;
    winTotal   += (uint32_t)samplesRead;

    // 2) Bootstrap on first data
    if (!initialized) {
        floorEMA = avgAbs;
        peakEMA  = avgAbs + 400.0f;          // ensure non-zero span at start
        envAR    = avgAbs;
        initialized = true;
    }

    // 3) Attack/Release envelope (abs average)
    const float aA = (attackTau  > 0.f) ? (1.f - expf(-dt / attackTau )) : 1.f;
    const float aR = (releaseTau > 0.f) ? (1.f - expf(-dt / releaseTau)) : 1.f;
    if (avgAbs > envAR) envAR += aA * (avgAbs - envAR);
    else                envAR += aR * (avgAbs - envAR);

    // 4) Floor: fast down, ultra slow up (prevents swallowing signal)
    if (envAR < floorEMA) {
        floorEMA = 0.80f * floorEMA + 0.20f * envAR;     // quick down
    } else {
        floorEMA = 0.9995f * floorEMA + 0.0005f * envAR; // very slow up
    }
    if (!isfinite(floorEMA)) floorEMA = 0.0f;

    // 5) Peak: fast up, slow down (tracks “music peaks”)
    if (envAR > peakEMA) {
        peakEMA = 0.70f * peakEMA + 0.30f * envAR;       // quick up
    } else {
        peakEMA = 0.995f * peakEMA + 0.005f * envAR;     // gentle pull-down
    }

    // Keep a minimum span so normalization is meaningful
    const float MIN_SPAN = 300.0f;   // raw units; tune if needed
    const float MARGIN   = 10.0f;    // raw units above floor required for “0”
    float low  = floorEMA + MARGIN;
    float high = peakEMA;
    if (high < low + MIN_SPAN) high = low + MIN_SPAN;

    // 6) Normalize, gate, gain, gamma
    float n = (envAR - low) / (high - low);  // can be negative
    if (!isfinite(n)) n = 0.0f;
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    // Gate AFTER normalization: silence becomes exact 0
    if (n <= noiseGate) n = 0.0f;

    // Software gain
    n *= globalGain;
    if (n > 1.0f) n = 1.0f;

    // Gamma shaping
    if (n > 0.0f) n = powf(n, gamma);
    if (!isfinite(n)) n = 0.0f;

    levelOut = constrain(n, 0.0f, 1.0f);

    // 7) Activity for HW calibration (use RAW, not normalized)
    ++winFrames;
    // “Raw activity” = signal meaningfully above floor
    const float ACT_MARGIN = MARGIN * 3.0f;
    if (avgAbs > floorEMA + ACT_MARGIN) ++winActiveRawFrames;

    // 8) Periodic HW gain calibration
    if (millis() - lastCalibMs > 2000) {
        calibrateHW();
        lastCalibMs = millis();
        winFrames = winActiveRawFrames = 0;
        winClipped = winTotal = 0;
    }

    samplesRead = 0;
}

// ---------- HW gain ----------
void AdaptiveMic::calibrateHW() {
    if (winFrames == 0) return;

    const float clipRatio = (winTotal > 0) ? (float)winClipped / (float)winTotal : 0.0f;
    const float activeRawRatio = (float)winActiveRawFrames / (float)winFrames;

    // Back off quickly on clipping
    if (clipRatio > 0.005f && currentGain > 0) {
        currentGain = max(0, currentGain - 4);
        PDM.setGain(currentGain);
        return;
    }

    // Only increase gain if there was real RAW activity above floor
    if (activeRawRatio < 0.05f) return; // mostly silence: do not creep up

    // Compute normalized “center” (using latest trackers) to decide direction
    const float MIN_SPAN = 300.0f, MARGIN = 10.0f;
    float low  = floorEMA + MARGIN;
    float high = peakEMA;
    if (high < low + MIN_SPAN) high = low + MIN_SPAN;
    float center = (envAR - low) / (high - low);
    if (!isfinite(center)) center = 0.0f;

    // If centered very low and no clipping, nudge up; if too high, nudge down
    if (center < 0.20f && clipRatio < 0.0005f && currentGain < 64) {
        currentGain = min(64, currentGain + 2);
        PDM.setGain(currentGain);
    } else if (center > 0.85f && currentGain > 0) {
        currentGain = max(0, currentGain - 2);
        PDM.setGain(currentGain);
    }
}
