#include "AdaptiveMic.h"
#include <math.h>

// Helper for constrain
template<typename T>
static T constrainValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

// Helper for max
template<typename T>
static T maxValue(T a, T b) {
    return (a > b) ? a : b;
}

// -------- Static ISR accumulators --------
AdaptiveMic* AdaptiveMic::s_instance = nullptr;
volatile uint32_t AdaptiveMic::s_isrCount   = 0;
volatile uint64_t AdaptiveMic::s_sumAbs     = 0;
volatile uint32_t AdaptiveMic::s_numSamples = 0;
volatile uint16_t AdaptiveMic::s_maxAbs     = 0;

// ---------- Public ----------
AdaptiveMic::AdaptiveMic(IPdmMic& pdm, ISystemTime& time)
    : pdm_(pdm), time_(time) {
}

bool AdaptiveMic::begin(uint32_t sampleRate, int gainInit) {
  _sampleRate    = sampleRate;
  currentHardwareGain  = constrainValue(gainInit, hwGainMin, hwGainMax);
  s_instance     = this;

  pdm_.onReceive(AdaptiveMic::onPDMdata);
  // PDM.setBufferSize(1024); // optional if core supports it

  bool ok = pdm_.begin(1, _sampleRate); // mono @ 16k
  if (!ok) return false;

  pdm_.setGain(currentHardwareGain);

  envAR = envMean = 0.0f; minEnv = 1e9f; maxEnv = 0.0f;
  levelInstant = levelPreGate = levelPostAGC = 0.0f;
  // Note: globalGain intentionally NOT reset here - keep default of 3.0f for faster startup
  lastHwCalibMs = time_.millis();
  lastIsrMs = time_.millis(); pdmAlive = false;
  return true;
}

void AdaptiveMic::end() {
  pdm_.end();
  s_instance = nullptr;
}

void AdaptiveMic::update(float dt) {
  if (dt < MicConstants::MIN_DT_SECONDS) dt = MicConstants::MIN_DT_SECONDS;
  if (dt > MicConstants::MAX_DT_SECONDS) dt = MicConstants::MAX_DT_SECONDS;

  computeCoeffs(dt);

  float avgAbs = 0.0f;
  uint16_t maxAbsVal = 0;
  uint32_t n = 0;
  consumeISR(avgAbs, maxAbsVal, n);

  uint32_t nowMs = time_.millis();
  pdmAlive = !isMicDead(nowMs, 250);

  if (n > 0) {
    // 1. Raw instantaneous average
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

    // Note: Environment adaptation handled naturally by AGC
    // Hardware gain adapts slowly (minutes) to environment
    // Software AGC adapts faster (seconds) to music dynamics

    // --- Simplified Transient detection ---
    float x = levelPostAGC;

    // Slow average tracks the baseline level
    slowAvg += slowAlpha * (x - slowAvg);

    uint32_t now = time_.millis();
    bool cooldownExpired = (now - lastTransientMs) > transientCooldownMs;

    // Transient = current level significantly above baseline
    // transientFactor 1.4 means level must be 40% above baseline
    float threshold = maxValue(loudFloor, slowAvg * transientFactor);

    if (cooldownExpired && x > threshold) {
        transient = 1.0f;
        lastTransientMs = now;
    }

    // Decay transient
    transient -= transientDecay * dt;
    if (transient < 0.0f) transient = 0.0f;
  }

  if (!pdmAlive) return;
  hardwareCalibrate(nowMs, dt);
}

// ---------- Private helpers ----------

void AdaptiveMic::computeCoeffs(float /*dt*/) {
  // Placeholder; coefficients recomputed in updateEnvelope
}

void AdaptiveMic::consumeISR(float& avgAbs, uint16_t& maxAbsVal, uint32_t& n) {
  time_.noInterrupts();
  uint64_t sum = s_sumAbs;
  uint32_t cnt = s_numSamples;
  uint16_t m   = s_maxAbs;
  s_sumAbs = 0;
  s_numSamples = 0;
  s_maxAbs = 0;
  time_.interrupts();

  n = cnt;
  maxAbsVal = m;
  avgAbs = (cnt > 0) ? float(sum) / float(cnt) : 0.0f;
}

void AdaptiveMic::updateEnvelope(float avgAbs, float dt) {
  float aAtkFrame = 1.0f - expf(-dt / maxValue(attackSeconds, 1e-3f));
  float aRelFrame = 1.0f - expf(-dt / maxValue(releaseSeconds, 1e-3f));

  if (avgAbs >= envAR) {
    envAR += aAtkFrame * (avgAbs - envAR);
  } else {
    envAR += aRelFrame * (avgAbs - envAR);
  }

  float meanAlpha = 1.0f - expf(-dt / MicConstants::ENV_MEAN_TAU_SECONDS);
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
    dwellAtMax = maxValue(0.0f, dwellAtMax - dt * (1.0f/limitDwellRelaxSec));
  }

  if (levelPreGate >= 0.98f && globalGain <= agMin * 1.001f) {
    dwellAtMin += dt;
  } else if (globalGain <= agMin * 1.001f) {
    dwellAtMin += dt;
  } else if (dwellAtMin > 0.0f) {
    dwellAtMin = maxValue(0.0f, dwellAtMin - dt * (1.0f/limitDwellRelaxSec));
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
    if (currentHardwareGain < hwGainMax) delta = +hwGainStep;
  } else if (tooLoudEnv || swPinnedLow) {
    if (currentHardwareGain > hwGainMin) delta = -hwGainStep;
  }

  if (delta != 0) {
    int oldGain = currentHardwareGain;
    currentHardwareGain = constrainValue(currentHardwareGain + delta, hwGainMin, hwGainMax);
    if (currentHardwareGain != oldGain) {
      pdm_.setGain(currentHardwareGain);

      float softComp = (delta > 0) ? (1.0f/1.05f) : 1.05f;
      globalGain = clamp01(globalGain * softComp);
      dwellAtMax = 0.0f;
      dwellAtMin = 0.0f;
    }
  }

  lastHwCalibMs = nowMs;
}

// Helper for min
template<typename T>
static T minValue(T a, T b) {
    return (a < b) ? a : b;
}

// ---------- ISR ----------
void AdaptiveMic::onPDMdata() {
  if (!s_instance) return;
  int bytesAvailable = s_instance->pdm_.available();
  if (bytesAvailable <= 0) return;

  static int16_t buffer[512];
  int toRead = minValue(bytesAvailable, (int)sizeof(buffer));
  int bytesRead = s_instance->pdm_.read(buffer, toRead);
  if (bytesRead <= 0) return;

  int samples = bytesRead / (int)sizeof(int16_t);
  uint64_t localSumAbs = 0;
  uint16_t localMaxAbs = 0;

  for (int i = 0; i < samples; ++i) {
    int16_t s = buffer[i];
    // Use int32_t to avoid overflow when s == INT16_MIN (-32768)
    uint16_t a = (uint16_t)((s < 0) ? -((int32_t)s) : s);
    localSumAbs += a;
    if (a > localMaxAbs) localMaxAbs = a;
  }

  // Note: We're already in ISR context, so interrupts are disabled.
  // Direct access to static volatiles is safe here without additional guards.
  s_sumAbs     += localSumAbs;
  s_numSamples += samples;
  if (localMaxAbs > s_maxAbs) s_maxAbs = localMaxAbs;
  s_isrCount++;

  s_instance->lastIsrMs = s_instance->time_.millis();
}
