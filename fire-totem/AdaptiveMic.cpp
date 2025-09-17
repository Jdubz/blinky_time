#include "AdaptiveMic.h"

// -------- Static ISR accumulators --------
AdaptiveMic* AdaptiveMic::s_instance = nullptr;
volatile uint32_t AdaptiveMic::s_isrCount   = 0;
volatile uint64_t AdaptiveMic::s_sumAbs     = 0;
volatile uint32_t AdaptiveMic::s_numSamples = 0;
volatile uint16_t AdaptiveMic::s_maxAbs     = 0;

// ---------- Public ----------
bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate    = sampleRate;
  currentHwGain  = constrain(gainInit, hwGainMin, hwGainMax);
  s_instance     = this;

  PDM.onReceive(AdaptiveMic::onPDMdata);
  // PDM.setBufferSize(1024); // optional if core supports it

  bool ok = PDM.begin(1, _sampleRate); // mono @ 16k
  if (!ok) return false;

  PDM.setGain(currentHwGain);

  envAR = envMean = 0.0f; minEnv = 1e9f; maxEnv = 0.0f;
  globalGain = levelInstant = levelPreGate = levelPostAGC = 0.0f;
  lastHwCalibMs = millis();
  lastIsrMs = millis(); pdmAlive = false;
  return true;
}

void AdaptiveMic::end() {
  PDM.end();
  s_instance = nullptr;
}

void AdaptiveMic::update(float dt) {
  if (dt < 0.0001f) dt = 0.0001f;
  if (dt > 0.1000f) dt = 0.1000f;

  computeCoeffs(dt);

  float avgAbs = 0.0f;
  uint16_t maxAbs = 0;
  uint32_t n = 0;
  consumeISR(avgAbs, maxAbs, n);

  uint32_t nowMs = millis();
  pdmAlive = !isMicDead(nowMs, 250);

  if (n > 0) {
    // Raw instantaneous average (no smoothing)
    levelInstant = avgAbs;

    // Still update envelope & mean for adaptation
    updateEnvelope(avgAbs, dt);

    // Maintain normalization window based on envelope
    updateNormWindow(envAR, dt);

    // Normalize using raw instantaneous magnitude
    float denom = (maxEnv - minEnv);
    float norm = denom > 1e-6f ? (levelInstant - minEnv) / denom : 0.0f;
    norm = clamp01(norm);

    if (norm > 0.0f && norm < 1.0f) {
      float insetLo = normInset, insetHi = 1.0f - normInset;
      norm = insetLo + norm * (insetHi - insetLo);
    }
    levelPreGate = norm;

    if (agEnabled) autoGainTick(dt);

    float afterGain = clamp01(levelPreGate * globalGain);
    levelPostAGC = (afterGain < noiseGate) ? 0.0f : afterGain;
  }

  if (!pdmAlive) return;
  hardwareCalibrate(nowMs, dt);
}

// ---------- Private helpers ----------

void AdaptiveMic::computeCoeffs(float /*dt*/) {
  // Placeholder; coefficients recomputed in updateEnvelope
}

void AdaptiveMic::consumeISR(float& avgAbs, uint16_t& maxAbs, uint32_t& n) {
  noInterrupts();
  uint64_t sum = s_sumAbs;
  uint32_t cnt = s_numSamples;
  uint16_t m   = s_maxAbs;
  s_sumAbs = 0;
  s_numSamples = 0;
  s_maxAbs = 0;
  interrupts();

  n = cnt;
  maxAbs = m;
  avgAbs = (cnt > 0) ? float(sum) / float(cnt) : 0.0f;
}

void AdaptiveMic::updateEnvelope(float avgAbs, float dt) {
  float aAtkFrame = 1.0f - expf(-dt / max(attackSeconds, 1e-3f));
  float aRelFrame = 1.0f - expf(-dt / max(releaseSeconds, 1e-3f));

  if (avgAbs >= envAR) {
    envAR += aAtkFrame * (avgAbs - envAR);
  } else {
    envAR += aRelFrame * (avgAbs - envAR);
  }

  float meanAlpha = 1.0f - expf(-dt / 90.0f); // ~90s
  envMean += meanAlpha * (envAR - envMean);
}

void AdaptiveMic::updateNormWindow(float ref, float dt) {
  if (ref < minEnv) minEnv = ref;
  if (ref > maxEnv) maxEnv = ref;

  minEnv = minEnv * normFloorDecay + ref * (1.0f - normFloorDecay);
  maxEnv = maxEnv * normCeilDecay  + ref * (1.0f - normCeilDecay);

  if (maxEnv < minEnv + 1.0f) {
    maxEnv = minEnv + 1.0f;
  }
}

void AdaptiveMic::autoGainTick(float dt) {
  float err = agTarget - levelPreGate;
  globalGain += agStrength * err * dt;

  if (globalGain < agMin) globalGain = agMin;
  if (globalGain > agMax) globalGain = agMax;

  if (fabsf(levelPreGate) < 1e-6f && globalGain >= agMax * 0.999f) {
    dwellAtMax += dt;
  } else if (globalGain >= agMax * 0.999f) {
    dwellAtMax += dt;
  } else if (dwellAtMax > 0.0f) {
    dwellAtMax = max(0.0f, dwellAtMax - dt * (1.0f/limitDwellRelaxSec));
  }

  if (levelPreGate >= 0.98f && globalGain <= agMin * 1.001f) {
    dwellAtMin += dt;
  } else if (globalGain <= agMin * 1.001f) {
    dwellAtMin += dt;
  } else if (dwellAtMin > 0.0f) {
    dwellAtMin = max(0.0f, dwellAtMin - dt * (1.0f/limitDwellRelaxSec));
  }
}

void AdaptiveMic::hardwareCalibrate(uint32_t nowMs, float /*dt*/) {
  if ((nowMs - lastHwCalibMs) < hwCalibPeriodMs) return;

  bool tooQuietEnv = (envMean < envTargetRaw * envLowRatio);
  bool tooLoudEnv  = (envMean > envTargetRaw * envHighRatio);

  bool swPinnedHigh = (dwellAtMax >= limitDwellTriggerSec);
  bool swPinnedLow  = (dwellAtMin >= limitDwellTriggerSec);

  int delta = 0;
  if (tooQuietEnv || swPinnedHigh) {
    if (currentHwGain < hwGainMax) delta = +hwGainStep;
  } else if (tooLoudEnv || swPinnedLow) {
    if (currentHwGain > hwGainMin) delta = -hwGainStep;
  }

  if (delta != 0) {
    int oldGain = currentHwGain;
    currentHwGain = constrain(currentHwGain + delta, hwGainMin, hwGainMax);
    if (currentHwGain != oldGain) {
      PDM.setGain(currentHwGain);

      float softComp = (delta > 0) ? (1.0f/1.05f) : 1.05f;
      globalGain = clamp01(globalGain * softComp);
      dwellAtMax = 0.0f;
      dwellAtMin = 0.0f;
    }
  }

  lastHwCalibMs = nowMs;
}

// ---------- ISR ----------
void AdaptiveMic::onPDMdata() {
  if (!s_instance) return;
  int bytesAvailable = PDM.available();
  if (bytesAvailable <= 0) return;

  static int16_t buffer[512];
  int toRead = min<int>(bytesAvailable, (int)sizeof(buffer));
  int read = PDM.read(buffer, toRead);
  if (read <= 0) return;

  int samples = read / (int)sizeof(int16_t);
  uint64_t localSumAbs = 0;
  uint16_t localMaxAbs = 0;

  for (int i = 0; i < samples; ++i) {
    int16_t s = buffer[i];
    uint16_t a = (uint16_t)abs(s);
    localSumAbs += a;
    if (a > localMaxAbs) localMaxAbs = a;
  }

  noInterrupts();
  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_isrCount++;
  interrupts();

  s_instance->lastIsrMs = millis();
}
