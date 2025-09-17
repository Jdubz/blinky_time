#include "AdaptiveMic.h"

// -------- Static ISR accumulators --------
AdaptiveMic* AdaptiveMic::s_instance = nullptr;
volatile uint32_t AdaptiveMic::s_isrCount   = 0;
volatile uint64_t AdaptiveMic::s_sumAbs     = 0;
volatile uint32_t AdaptiveMic::s_numSamples = 0;
volatile uint16_t AdaptiveMic::s_maxAbs     = 0;

// ---------- Public ----------
bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate = sampleRate;
  currentHwGain = constrain(gainInit, hwGainMin, hwGainMax);
  s_instance = this;

  // Configure PDM
  if (!PDM.begin(1, _sampleRate)) { // 1 = mono
    return false;
  }
  PDM.setGain(currentHwGain);
  PDM.onReceive(AdaptiveMic::onPDMdata);

  // Initialize state
  envAR   = 0.0f;
  envMean = 0.0f;
  minEnv  = 1e9f;
  maxEnv  = 0.0f;
  globalGain = 1.0f;
  levelPreGate = 0.0f;
  levelPostAGC = 0.0f;
  lastHwCalibMs = millis();

  return true;
}

void AdaptiveMic::end() {
  PDM.end();
  s_instance = nullptr;
}

// Should be called every frame with dt in seconds
void AdaptiveMic::update(float dt) {
  // Defensive dt clamp
  if (dt < 0.0001f) dt = 0.0001f;
  if (dt > 0.1000f) dt = 0.1000f;

  computeCoeffs(dt);

  // Drain ISR accumulators
  float avgAbs = 0.0f;
  uint16_t maxAbs = 0;
  uint32_t n = 0;
  consumeISR(avgAbs, maxAbs, n);

  // If no samples yet, keep previous outputs but still run slow processes
  if (n > 0) {
    // 1) Envelope with attack/release
    updateEnvelope(avgAbs, dt);

    // 2) Maintain normalization window slowly & safely
    updateNormWindow(dt);

    // 3) Normalize (pre-gain)
    float denom = (maxEnv - minEnv);
    float norm = denom > 1e-6f ? (envAR - minEnv) / denom : 0.0f;
    norm = clamp01(norm);

    // Inset (keep headroom inside window)
    if (norm > 0.0f && norm < 1.0f) {
      float insetLo = normInset;
      float insetHi = 1.0f - normInset;
      norm = insetLo + norm * (insetHi - insetLo);
    }

    levelPreGate = norm;

    // 4) Software AGC (fast, ~seconds)
    if (agEnabled) {
      autoGainTick(dt);
    }

    // Apply SW gain & gate
    float afterGain = levelPreGate * globalGain;
    afterGain = clamp01(afterGain);
    levelPostAGC = (afterGain < noiseGate) ? 0.0f : afterGain;
  }

  // 5) Slow hardware gain (~minutes)
  uint32_t nowMs = millis();
  hardwareCalibrate(nowMs, dt);
}

// ---------- Private helpers ----------

void AdaptiveMic::computeCoeffs(float /*dt*/) {
  // Convert time constants (seconds) to per-frame smoothing coeffs.
  // We use the standard one-pole formulation on per-update envelope:
  // y += alpha * (x - y), where alpha = 1 - exp(-dt/tau)
  // We don't need dt here because we apply on each update() call with the provided dt.

  // Coeffs are recomputed in updateEnvelope using dt; keep placeholders if needed.
}

void AdaptiveMic::consumeISR(float& avgAbs, uint16_t& maxAbs, uint32_t& n) {
  noInterrupts();
  uint64_t sum = s_sumAbs;
  uint32_t cnt = s_numSamples;
  uint16_t m   = s_maxAbs;
  // reset
  s_sumAbs = 0;
  s_numSamples = 0;
  s_maxAbs = 0;
  interrupts();

  n = cnt;
  maxAbs = m;
  avgAbs = (cnt > 0) ? float(sum) / float(cnt) : 0.0f;
}

void AdaptiveMic::updateEnvelope(float avgAbs, float dt) {
  // Attack/release alpha per frame
  float aAtkFrame = 1.0f - expf(-dt / max(attackSeconds, 1e-3f));
  float aRelFrame = 1.0f - expf(-dt / max(releaseSeconds, 1e-3f));

  // Standard rectified envelope follower
  float x = avgAbs;
  if (x >= envAR) {
    envAR += aAtkFrame * (x - envAR);
  } else {
    envAR += aRelFrame * (x - envAR);
  }

  // Very slow mean (environment)
  // Blend a *tiny* portion each frame so this is minutes-scale.
  // For ~60 FPS, dt ~0.016 => choose meanAlpha ~ 1 - exp(-dt/T) with T ~ 60–120s
  float meanAlpha = 1.0f - expf(-dt / 90.0f); // ~90s time constant
  envMean += meanAlpha * (envAR - envMean);
}

void AdaptiveMic::updateNormWindow(float dt) {
  // Expand window to include current envAR
  if (envAR < minEnv) minEnv = envAR;
  if (envAR > maxEnv) maxEnv = envAR;

  // Slowly relax the window toward envAR to follow long-term drift without abrupt resets.
  // Decay edges slightly toward envAR.
  minEnv = minEnv * normFloorDecay + envAR * (1.0f - normFloorDecay);
  maxEnv = maxEnv * normCeilDecay  + envAR * (1.0f - normCeilDecay);

  // Prevent collapse
  if (maxEnv < minEnv + 1.0f) {
    maxEnv = minEnv + 1.0f;
  }
}

void AdaptiveMic::autoGainTick(float dt) {
  // Error integrates toward target level on ~10s horizon
  float err = agTarget - levelPreGate; // pre-gate/pre-gamma norm
  globalGain += agStrength * err * dt;

  if (globalGain < agMin) globalGain = agMin;
  if (globalGain > agMax) globalGain = agMax;

  // Track dwell at limits to inform hardware calibration
  if (fabsf(levelPreGate) < 1e-6f && globalGain >= agMax * 0.999f) {
    // silence + max gain: still too quiet
    dwellAtMax += dt;
  } else if (globalGain >= agMax * 0.999f) {
    dwellAtMax += dt;
  } else if (dwellAtMax > 0.0f) {
    dwellAtMax = max(0.0f, dwellAtMax - dt * (limitDwellRelaxSec > 0 ? (1.0f/limitDwellRelaxSec) : 1.0f));
  }

  if (levelPreGate >= 0.98f && globalGain <= agMin * 1.001f) {
    // very hot + min gain: still too loud
    dwellAtMin += dt;
  } else if (globalGain <= agMin * 1.001f) {
    dwellAtMin += dt;
  } else if (dwellAtMin > 0.0f) {
    dwellAtMin = max(0.0f, dwellAtMin - dt * (limitDwellRelaxSec > 0 ? (1.0f/limitDwellRelaxSec) : 1.0f));
  }
}

void AdaptiveMic::hardwareCalibrate(uint32_t nowMs, float /*dt*/) {
  if ((nowMs - lastHwCalibMs) < hwCalibPeriodMs) return;

  // Only consider a step if envMean is consistently off target, or SW gain has been pinned.
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

      // IMPORTANT: Do NOT reset minEnv/maxEnv; preserve normalization window.
      // Optional: gently nudge software gain opposite to keep perceived level steady.
      // Example: if HW gain +1 step, bring SW gain down ~5% to soften the change.
      float softComp = (delta > 0) ? (1.0f/1.05f) : 1.05f;
      globalGain = clamp01(globalGain * softComp);
      // Reset dwell timers so we don't immediately step again
      dwellAtMax = 0.0f;
      dwellAtMin = 0.0f;
    }
  }

  lastHwCalibMs = nowMs;
}

// ---------- ISR ----------
void AdaptiveMic::onPDMdata() {
  if (!s_instance) return;

  // Read available samples
  int bytesAvailable = PDM.available();
  if (bytesAvailable <= 0) return;

  static int16_t buffer[256];
  int toRead = min<int>(bytesAvailable, (int)sizeof(buffer));
  int read = PDM.read(buffer, toRead);
  if (read <= 0) return;

  int samples = read / sizeof(int16_t);
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
}
