#include "AdaptiveMic.h"

volatile int16_t AdaptiveMic::sampleBuffer[512];
volatile int     AdaptiveMic::sampleBufferSize = 0;

void AdaptiveMic::begin() {
  PDM.onReceive(AdaptiveMic::onPDMdata);
  PDM.setBufferSize(512);
  PDM.setGain(hwGain);
  PDM.begin(1, 16000); // mono, 16 kHz
}

void AdaptiveMic::update() {
  if (sampleBufferSize == 0) return;

  // Copy & reset buffer atomically
  noInterrupts();
  int size = sampleBufferSize;
  sampleBufferSize = 0;
  interrupts();

  // Compute raw RMS & peak
  long sumSq = 0;
  int16_t maxS = 0;
  for (int i = 0; i < size; i++) {
    int16_t s = sampleBuffer[i];
    if (abs(s) > maxS) maxS = abs(s);
    sumSq += (long)s * (long)s;
  }

  float rawRMS = (size > 0) ? sqrt((float)sumSq / (float)size) / 32768.0f : 0.0f;
  peak = (float)maxS / 32768.0f;

  // ---------- Software gain control (fast) ----------
  // Track short-term RMS (LPF ~0.5s)
  avgRMS = avgRMS * 0.95f + rawRMS * 0.05f;

  // Aim for target average around ~0.3
  if (avgRMS > 0.0005f) {
    const float target = 0.30f;
    float desired = target / avgRMS;
    swGain = swGain * 0.95f + desired * 0.05f; // smooth adjustment
  }
  // ---------- Transient envelope (attack/release) ----------
  // Quick “attack,” slower “release” for punchy hits
  const float attack  = 0.60f;
  const float release = 0.06f;
  if (rawRMS > envelope)
    envelope = attack * rawRMS  + (1.0f - attack)  * envelope;
  else
    envelope = release * rawRMS + (1.0f - release) * envelope;

  // Final “RMS” that FireEffect uses = envelope * software gain
  rms = envelope * swGain;

  // ---------- Hardware gain control (slow ~60s) ----------
  if (millis() - lastHW > 60000) {
    if (peak < 0.05f && hwGain < 80)  hwGain += 2; // too quiet → boost a bit
    if (peak > 0.95f && hwGain > 10)  hwGain -= 2; // too hot → reduce a bit
    PDM.setGain(hwGain);
    lastHW = millis();
  }
}

float AdaptiveMic::getRMS()         { return rms; }
float AdaptiveMic::getPeak()        { return peak; }
float AdaptiveMic::getEnvelope()    { return envelope * swGain; }

float AdaptiveMic::getSoftwareGain(){ return swGain; }
int   AdaptiveMic::getHardwareGain(){ return hwGain; }

void AdaptiveMic::onPDMdata() {
  int bytes = PDM.available();
  PDM.read((void*)sampleBuffer, bytes);
  sampleBufferSize = bytes / 2; // 16-bit samples
}
