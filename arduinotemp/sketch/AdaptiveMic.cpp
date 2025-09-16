#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\AdaptiveMic.cpp"
#include "AdaptiveMic.h"
#include <Arduino.h>
#include <PDM.h>
#include <math.h>

// -------- Static PDM ISR buffer --------
volatile int16_t AdaptiveMic::sampleBuffer[512];
volatile int AdaptiveMic::sampleBufferSize = 0;

// Internal calibration/state helpers
static bool           s_calibrated     = false;
static unsigned long  s_calibStart     = 0;
static unsigned long  s_lastSound      = 0;

// Median filter for peak spike rejection
static float s_peakBuf[5] = {0};
static int   s_peakIdx    = 0;

// -------------------- PDM ISR --------------------
static void onPDMdata();
void AdaptiveMic::onPDMdata() {
  int bytesAvailable = PDM.available();
  if (bytesAvailable <= 0) return;
  if (bytesAvailable > (int)sizeof(sampleBuffer)) bytesAvailable = sizeof(sampleBuffer);
  int bytesRead = PDM.read((void*)sampleBuffer, bytesAvailable);
  if (bytesRead < 0) return;
  sampleBufferSize = bytesRead / (int)sizeof(sampleBuffer[0]);
}

// -------------------- AdaptiveMic ----------------
AdaptiveMic::AdaptiveMic() {}

void AdaptiveMic::begin() {
  // Basic PDM init (mono @ 16 kHz)
  PDM.onReceive(AdaptiveMic::onPDMdata);
  PDM.setBufferSize(sizeof(sampleBuffer));
  PDM.setGain(currentGain);           // start mid gain
  if (!PDM.begin(1, 16000)) {         // mono @ 16kHz
    Serial.println("PDM init failed — mic disabled");
    micReady = false;
    return;
  }

  // Track rate for filter calc (kept constant here)
  sampleRate = 16000.0f;
  updateBiquad();

  micReady = true;
  s_calibrated  = false;
  s_calibStart  = 0;
  s_lastSound   = millis();
  Serial.println("Mic started");
}

void AdaptiveMic::updateBiquad() {
  // RBJ Audio EQ Cookbook biquads
  float fs   = sampleRate;
  float f0   = bassFc;
  if (f0 < 10.0f)  f0 = 10.0f;
  if (f0 > fs*0.45f) f0 = fs*0.45f;
  float Q    = bassQ;
  if (Q < 0.25f) Q = 0.25f;

  float w0   = 2.0f * PI * (f0 / fs);
  float c    = cosf(w0);
  float s    = sinf(w0);
  float alpha= s / (2.0f * Q);

  float a0, _b0, _b1, _b2, _a1, _a2;

  if (bassMode == BASS_LOWPASS) {
    // Low-pass
    _b0 = (1.0f - c) * 0.5f;
    _b1 =  1.0f - c;
    _b2 = (1.0f - c) * 0.5f;
    a0  =  1.0f + alpha;
    _a1 = -2.0f * c;
    _a2 =  1.0f - alpha;
  } else {
    // Band-pass (constant peak gain)
    _b0 =  s * 0.5f;
    _b1 =  0.0f;
    _b2 = -s * 0.5f;
    a0  =  1.0f + alpha;
    _a1 = -2.0f * c;
    _a2 =  1.0f - alpha;
  }

  // Normalize
  b0 = _b0 / a0;
  b1 = _b1 / a0;
  b2 = _b2 / a0;
  a1 = _a1 / a0;
  a2 = _a2 / a0;

  // Reset state to avoid pops when retuning
  z1 = z2 = 0.0f;
}

void AdaptiveMic::setBassFilter(bool enabled, float centerHz, float q, BassMode mode) {
  bassEnabled = enabled;
  bassFc = centerHz;
  bassQ  = q;
  bassMode = mode;
  updateBiquad();
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

    // Slowly decay min/max during silence to track quiet rooms
    minEnv = min(minEnv * 1.000f + 1e-7f, minEnv * 1.001f + 1e-6f);
    maxEnv *= 0.999f;
    if (maxEnv < envelope) maxEnv = envelope;

    return;
  }

  // --- Block DSP ---
  const float inv32768 = 1.0f / 32768.0f;
  float sumSq = 0.0f;
  float peakF = 0.0f;

  for (int i = 0; i < size; i++) {
    float x = (float)sampleBuffer[i] * inv32768; // -1..1
    if (bassEnabled) x = processBiquad(x);
    float ax = fabsf(x);
    if (ax > peakF) peakF = ax;
    sumSq += x * x;
  }

  if (sumSq > 0.0f || peakF > 0.0f) {
    float rms  = sqrtf(sumSq / (float)size); // 0..~1, mostly <<1
    if (!isfinite(rms) || rms < 0) rms = 0;
    float peak = peakF;
    if (!isfinite(peak) || peak < 0) peak = 0;

    // Envelope follower (attack faster than release)
    const float attack = 0.6f, release = 0.05f;
    if (!isfinite(envelope) || envelope < 0) envelope = 0;
    if (rms > envelope)
      envelope = attack * rms + (1 - attack) * envelope;
    else
      envelope = release * rms + (1 - release) * envelope;
    if (envelope < 0) envelope = 0;

    // Long-term mean (slow)
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

  // Startup auto-calibration (3s)
  if (!s_calibrated) {
    if (s_calibStart == 0) s_calibStart = millis();
    // tighten min/max during the window
    if (envelope < minEnv) minEnv = envelope;
    if (envelope > maxEnv) maxEnv = envelope;

    if (millis() - s_calibStart > 3000UL) {
      if (minEnv < 0)      minEnv = 0;
      if (maxEnv < 0.001f) maxEnv = 0.001f;
      s_calibrated = true;
      Serial.println("Mic calibration complete");
    }
  }

  // ---- NaN & bounds protection ----
  if (!isfinite(envelope)   || envelope   < 0) envelope   = 0;
  if (!isfinite(envMean)    || envMean    < 0) envMean    = 0;
  if (!isfinite(minEnv)     || minEnv     < 0) minEnv     = 0;
  if (!isfinite(maxEnv)     || maxEnv   < 0.00001f) maxEnv = 0.00001f;
  if (!isfinite(recentPeak) || recentPeak < 0) recentPeak = 0;

  // --- Auto gain management every ~5s (unchanged behavior) ---
  if (millis() - lastGainAdjust > 5000UL) {
    lastGainAdjust = millis();
    // Adjust PDM gain to keep recentPeak in a sweet spot
    float target = 0.35f;  // target peak (post-filter)
    if (recentPeak < target * 0.5f && currentGain < 255) {
      currentGain = min(255, currentGain + 3);
      PDM.setGain(currentGain);
    } else if (recentPeak > target * 1.5f && currentGain > 0) {
      currentGain = max(0, currentGain - 3);
      PDM.setGain(currentGain);
    }
  }
}

float AdaptiveMic::getLevel() {
  // Normalize envelope to 0..1 using dynamic window around envMean
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
