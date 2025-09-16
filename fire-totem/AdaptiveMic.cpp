#include "AdaptiveMic.h"
#include <Arduino.h>
#include <PDM.h>
#include <math.h>

// -------- Static PDM ISR buffer --------
volatile int16_t AdaptiveMic::sampleBuffer[512];
volatile int AdaptiveMic::sampleBufferSize = 0;

static bool     s_calibrated     = false;
static unsigned long s_calibStart = 0;
static unsigned long s_lastSound  = 0;

// Median filter for peak spike rejection
static float s_peakBuf[5] = {0};
static int   s_peakIdx    = 0;

void AdaptiveMic::onPDMdata() {
  int bytes = PDM.available();
  if (bytes <= 0) return;

  // Clamp read size to our static buffer
  int maxBytes = (int)sizeof(sampleBuffer);
  if (bytes > maxBytes) bytes = maxBytes;

  PDM.read((void*)sampleBuffer, bytes);
  sampleBufferSize = bytes / 2; // 16-bit samples
}

AdaptiveMic::AdaptiveMic() {}

void AdaptiveMic::begin() {
  delay(500); // allow mic to power up

  PDM.onReceive(AdaptiveMic::onPDMdata);
  PDM.setBufferSize(sizeof(sampleBuffer));
  PDM.setGain(currentGain);           // start mid gain
  if (!PDM.begin(1, 16000)) {         // mono @ 16kHz
    Serial.println("PDM init failed — mic disabled");
    micReady = false;
    return;
  }

  micReady = true;
  s_calibrated  = false;
  s_calibStart  = 0;
  s_lastSound   = millis();
  Serial.println("Mic started");
}

void AdaptiveMic::update() {
  if (!micReady) return;

  // Snapshot ISR buffer safely
  int size;
  noInterrupts();
  size = sampleBufferSize;
  sampleBufferSize = 0;
  interrupts();

  if (size <= 0) {
    // Silence path: decay envelope gently
    if (!isfinite(envelope) || envelope < 0) envelope = 0;
    envelope *= 0.95f;
    if (envelope < 1e-6f) envelope = 0;

    // Recalibrate if prolonged silence
    if (s_calibrated && (millis() - s_lastSound > 60000UL)) {
      s_calibrated = false;
      minEnv = 1.0f;
      maxEnv = 0.0f;
      s_calibStart = 0;
      Serial.println("Mic re-calibrating...");
    }
  } else {
    // Compute RMS + peak
    long sumSq = 0;
    int16_t peakS = 0;
    for (int i = 0; i < size; i++) {
      int16_t s = sampleBuffer[i];
      int a = abs(s);
      if (a > peakS) peakS = a;
      sumSq += (long)s * (long)s;
    }

    if (sumSq > 0) {
      float rms  = sqrtf((float)sumSq / (float)size) / 32768.0f;
      if (!isfinite(rms) || rms < 0) rms = 0;
      float peak = (float)peakS / 32768.0f;
      if (!isfinite(peak) || peak < 0) peak = 0;

      // Envelope follower (attack faster than release)
      const float attack = 0.6f, release = 0.05f;
      if (!isfinite(envelope) || envelope < 0) envelope = 0;
      if (rms > envelope)
        envelope = attack * rms + (1 - attack) * envelope;
      else
        envelope = release * rms + (1 - release) * envelope;
      if (envelope < 0) envelope = 0;

      // Track long-term mean (slow)
      if (!isfinite(envMean) || envMean < 0) envMean = 0;
      envMean = envMean * 0.995f + envelope * 0.005f;
      if (envMean < 0) envMean = 0;

      // Track min/max window for dynamic range
      if (!isfinite(minEnv) || minEnv < 0) minEnv = 1.0f;
      if (!isfinite(maxEnv) || maxEnv < 0) maxEnv = 0.0f;
      if (envelope < minEnv) minEnv = envelope;
      if (envelope > maxEnv) maxEnv = envelope;

      // Median filter of recent peaks to ignore short spikes
      s_peakBuf[s_peakIdx] = peak;
      s_peakIdx = (s_peakIdx + 1) % 5;
      float tmp[5];
      memcpy(tmp, s_peakBuf, sizeof(tmp));
      for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
          if (tmp[j] < tmp[i]) { float t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
      float medPeak = tmp[2];

      // Decay + hold recentPeak for gain logic
      if (!isfinite(recentPeak) || recentPeak < 0) recentPeak = 0;
      recentPeak = max(recentPeak * 0.9f, medPeak);

      s_lastSound = millis();
    }
  }

  // Startup auto-calibration (3s)
  if (!s_calibrated) {
    if (s_calibStart == 0) s_calibStart = millis();
    // tighten min/max during the window
    if (envelope < minEnv) minEnv = envelope;
    if (envelope > maxEnv) maxEnv = envelope;

    if (millis() - s_calibStart > 3000UL) {
      if (minEnv < 0) minEnv = 0;
      if (maxEnv < 0.001f) maxEnv = 0.001f;
      s_calibrated = true;
      Serial.println("Mic calibration complete");
    }
  }

  // ---- NaN & bounds protection ----
  if (!isfinite(envelope) || envelope < 0) envelope = 0;
  if (!isfinite(envMean)  || envMean  < 0) envMean  = 0;
  if (!isfinite(minEnv)   || minEnv   < 0) minEnv   = 0;
  if (!isfinite(maxEnv)   || maxEnv   < 0.00001f)   maxEnv   = 0.00001f;
  if (!isfinite(recentPeak) || recentPeak < 0) recentPeak = 0;

  // ---- Hardware gain logic (fast down, slow up) every 5s ----
  if (millis() - lastGainAdjust > 5000UL) {
    const float loudThresh  = 0.75f;  // drop gain if sustained loud
    const float quietThresh = 0.01f;  // raise gain if very quiet

    if (recentPeak > loudThresh && currentGain > 10) {
      currentGain = max(currentGain - 1, 10);
      PDM.setGain(currentGain);
    } else if (envMean < quietThresh && currentGain < 80) {
      currentGain = min(currentGain + 1, 80);
      PDM.setGain(currentGain);
    }

    recentPeak = 0.0f; // reset the short-term peak integrator
    lastGainAdjust = millis();
  }
}

float AdaptiveMic::getLevel() {
  if (!isfinite(envelope) || envelope < 0) envelope = 0;
  if (!isfinite(envMean)  || envMean  < 0) envMean  = 0;

  // Keep a small floor so dynMax > dynMin even in silence
  if (envMean < 1e-5f) envMean = 1e-5f;
  if (!isfinite(minEnv) || minEnv < 0) minEnv = 0;
  if (!isfinite(maxEnv) || maxEnv < 0.001f) maxEnv = 0.001f;

  float dynMin = min(minEnv, envMean);
  float dynMax = max(maxEnv, envMean * 2.0f);
  if (!isfinite(dynMin) || dynMin < 0) dynMin = 0;
  if (!isfinite(dynMax) || dynMax <= dynMin) dynMax = dynMin + 1e-5f;

  float norm = (envelope - dynMin) / (dynMax - dynMin);
  if (!isfinite(norm) || norm < 0) norm = 0;
  if (norm > 1) norm = 1;

  return norm;
}
