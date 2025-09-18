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

    // Apply dynamic range compression for consistent response
    applyDynamicRangeCompression(afterGain);

    levelPostAGC = (afterGain < noiseGate) ? 0.0f : afterGain;

    // Enhanced musical analysis
    analyzeFrequencySpectrum(levelInstant);
    updateEnvironmentClassification(dt);
    detectMusicalPatterns(levelPostAGC, nowMs);
    adaptToEnvironment();

    // --- Enhanced Transient detection with spectral awareness ---
    float x = levelPostAGC;

    // update fast and slow averages
    fastAvg += fastAlpha * (x - fastAvg);
    slowAvg += slowAlpha * (x - slowAvg);

    uint32_t now = millis();
    bool cooldownExpired = (now - lastTransientMs) > transientCooldownMs;

    // Enhanced transient detection with musical awareness
    float dynamicTransientFactor = transientFactor;
    float dynamicLoudFloor = loudFloor;

    // Adjust sensitivity based on frequency content and environment
    if (bassLevel > 0.6f) {
        // Bass-heavy music needs less sensitivity to avoid constant triggering
        dynamicTransientFactor *= 1.3f;
        dynamicLoudFloor *= 1.2f;
    }
    if (currentEnv >= ENV_LOUD) {
        // Loud environments need higher thresholds
        dynamicTransientFactor *= 1.2f;
        dynamicLoudFloor *= 1.1f;
    }
    if (spectralCentroid < 500.0f) {
        // Low-frequency dominated content (bass, kick drums)
        dynamicTransientFactor *= 0.9f;  // More sensitive for bass hits
    }

    // condition: sharp jump + loud enough + cooldown + spectral awareness
    if (cooldownExpired &&
        x > dynamicLoudFloor &&
        fastAvg > slowAvg * dynamicTransientFactor) {

        // Scale transient intensity based on frequency content
        float intensity = 1.0f;
        if (bassLevel > 0.7f) intensity *= 1.2f;  // Boost for bass hits
        if (highLevel > 0.8f) intensity *= 1.1f; // Boost for percussive hits

        transient = clamp01(intensity);
        lastTransientMs = now;
    }

    // Decay transient ramp
    float decay = transientDecay * dt;
    if (decay > 1.0f) decay = 1.0f;
    transient -= decay;
    if (transient < 0.0f) transient = 0.0f;
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

// ---------- Enhanced Musical Analysis Methods ----------

void AdaptiveMic::analyzeFrequencySpectrum(float avgAbs) {
    // Fill frequency analysis buffer
    freqBuffer[freqBufferIndex] = avgAbs;
    freqBufferIndex = (freqBufferIndex + 1) % FREQ_BUFFER_SIZE;

    if (freqBufferIndex == 0) {
        freqBufferReady = true;
    }

    if (freqBufferReady) {
        computeSpectralBands();
        spectralCentroid = computeSpectralCentroid();
    }
}

void AdaptiveMic::computeSpectralBands() {
    // Simplified frequency band analysis using time-domain approximations
    // This is a lightweight alternative to full FFT for Arduino constraints

    float lowSum = 0.0f, midSum = 0.0f, highSum = 0.0f;
    int lowCount = 0, midCount = 0, highCount = 0;

    // Analyze different time scales to approximate frequency content
    // Short-term variations = high frequency, long-term = low frequency

    for (int i = 0; i < FREQ_BUFFER_SIZE; i++) {
        float sample = freqBuffer[i];

        // Low frequency approximation (longer-term averages)
        if (i % 8 == 0) {
            float windowSum = 0.0f;
            for (int j = max(0, i-8); j < min(FREQ_BUFFER_SIZE, i+8); j++) {
                windowSum += freqBuffer[j];
            }
            lowSum += windowSum / 16.0f;
            lowCount++;
        }

        // Mid frequency approximation (medium-term variations)
        if (i % 4 == 0) {
            float windowSum = 0.0f;
            for (int j = max(0, i-4); j < min(FREQ_BUFFER_SIZE, i+4); j++) {
                windowSum += freqBuffer[j];
            }
            midSum += abs(windowSum / 8.0f - sample);
            midCount++;
        }

        // High frequency approximation (short-term variations)
        if (i > 0) {
            highSum += abs(sample - freqBuffer[i-1]);
            highCount++;
        }
    }

    // Normalize and smooth the band levels
    float newBass = lowCount > 0 ? lowSum / lowCount : 0.0f;
    float newMid = midCount > 0 ? midSum / midCount : 0.0f;
    float newHigh = highCount > 0 ? highSum / highCount : 0.0f;

    // Smooth band transitions
    float bandSmooth = 0.7f;
    bassLevel = bassLevel * bandSmooth + newBass * (1.0f - bandSmooth);
    midLevel = midLevel * bandSmooth + newMid * (1.0f - bandSmooth);
    highLevel = highLevel * bandSmooth + newHigh * (1.0f - bandSmooth);

    // Normalize band levels
    float totalEnergy = bassLevel + midLevel + highLevel;
    if (totalEnergy > 1e-6f) {
        bassLevel /= totalEnergy;
        midLevel /= totalEnergy;
        highLevel /= totalEnergy;
    }
}

float AdaptiveMic::computeSpectralCentroid() {
    // Approximate spectral centroid using band energies
    // Lower values = bass-heavy, higher values = treble-heavy
    float weightedSum = bassLevel * 150.0f + midLevel * 1000.0f + highLevel * 4000.0f;
    float totalWeight = bassLevel + midLevel + highLevel;
    return totalWeight > 1e-6f ? weightedSum / totalWeight : 1000.0f;
}

void AdaptiveMic::updateEnvironmentClassification(float dt) {
    // Classify audio environment based on level patterns and spectral content
    envHistory[envHistoryIndex] = levelPostAGC;
    envHistoryIndex = (envHistoryIndex + 1) % 8;

    // Compute statistics from recent history
    float avgLevel = 0.0f, maxLevel = 0.0f, variance = 0.0f;
    for (int i = 0; i < 8; i++) {
        avgLevel += envHistory[i];
        if (envHistory[i] > maxLevel) maxLevel = envHistory[i];
    }
    avgLevel /= 8.0f;

    for (int i = 0; i < 8; i++) {
        float diff = envHistory[i] - avgLevel;
        variance += diff * diff;
    }
    variance /= 8.0f;

    // Update ambient noise floor
    float noiseSmooth = 0.99f;
    if (avgLevel < ambientNoise || ambientNoise == 0.0f) {
        ambientNoise = ambientNoise * noiseSmooth + avgLevel * (1.0f - noiseSmooth);
    }

    // Classify environment
    AudioEnvironment newEnv = ENV_UNKNOWN;

    if (avgLevel < 0.1f && variance < 0.01f) {
        newEnv = ENV_QUIET;
    } else if (avgLevel < 0.25f && variance < 0.05f) {
        newEnv = ENV_AMBIENT;
    } else if (avgLevel < 0.5f && variance < 0.15f) {
        newEnv = ENV_MODERATE;
    } else if (avgLevel < 0.75f || (variance > 0.2f && maxLevel > 0.8f)) {
        newEnv = ENV_LOUD;
    } else if (avgLevel > 0.75f && variance > 0.3f) {
        newEnv = ENV_CONCERT;
    } else if (avgLevel > 0.9f) {
        newEnv = ENV_EXTREME;
    }

    // Update environment with confidence tracking
    if (newEnv == currentEnv) {
        envConfidence = min(100U, envConfidence + 1);
    } else if (envConfidence > 0) {
        envConfidence--;
    } else {
        currentEnv = newEnv;
        envConfidence = 1;
    }
}

void AdaptiveMic::detectMusicalPatterns(float level, uint32_t nowMs) {
    // Simple beat detection for BPM estimation
    beatHistory[beatHistoryIndex] = level;
    beatHistoryIndex = (beatHistoryIndex + 1) % 16;

    // Look for periodic patterns in the beat history
    if (level > 0.6f && (nowMs - lastBeatMs) > 200) { // Minimum 200ms between beats
        // Check if this looks like a beat
        float recent = 0.0f, older = 0.0f;
        for (int i = 0; i < 4; i++) {
            int idx = (beatHistoryIndex - 1 - i + 16) % 16;
            recent += beatHistory[idx];
        }
        for (int i = 4; i < 8; i++) {
            int idx = (beatHistoryIndex - 1 - i + 16) % 16;
            older += beatHistory[idx];
        }

        if (recent > older * 1.5f) { // Recent levels much higher
            uint32_t beatInterval = nowMs - lastBeatMs;
            if (beatInterval > 0) {
                float newBPM = 60000.0f / beatInterval;
                if (newBPM >= 60.0f && newBPM <= 200.0f) { // Reasonable BPM range
                    estimatedBPM = estimatedBPM * 0.8f + newBPM * 0.2f; // Smooth BPM
                }
            }
            lastBeatMs = nowMs;
        }
    }
}

void AdaptiveMic::applyDynamicRangeCompression(float& level) {
    // Dynamic range compressor for consistent response across environments
    float compAlpha = 1.0f - expf(-1.0f / (compAttack * _sampleRate));
    float compBeta = 1.0f - expf(-1.0f / (compRelease * _sampleRate));

    // Envelope follower for compressor
    if (level > compEnvelope) {
        compEnvelope += compAlpha * (level - compEnvelope);
    } else {
        compEnvelope += compBeta * (level - compEnvelope);
    }

    // Apply compression if above threshold
    if (compEnvelope > compThresh) {
        float overThresh = compEnvelope - compThresh;
        float compGainReduction = 1.0f / (1.0f + (compRatio - 1.0f) * (overThresh / (1.0f - compThresh)));
        level *= compGainReduction * compGain;
    }
}

void AdaptiveMic::adaptToEnvironment() {
    // Automatically adjust parameters based on detected environment
    switch (currentEnv) {
        case ENV_QUIET:
            agTarget = 0.4f;  // Higher target for quiet environments
            transientFactor = 2.0f; // More sensitive
            noiseGate = 0.03f; // Lower gate
            break;

        case ENV_AMBIENT:
            agTarget = 0.35f;
            transientFactor = 2.5f;
            noiseGate = 0.06f;
            break;

        case ENV_MODERATE:
            agTarget = 0.35f; // Default
            transientFactor = 2.5f;
            noiseGate = 0.06f;
            break;

        case ENV_LOUD:
            agTarget = 0.3f; // Lower target to prevent clipping
            transientFactor = 3.0f; // Less sensitive to avoid false triggers
            noiseGate = 0.08f; // Higher gate
            compRatio = 3.0f; // More compression
            break;

        case ENV_CONCERT:
            agTarget = 0.25f;
            transientFactor = 3.5f;
            noiseGate = 0.1f;
            compRatio = 4.0f;
            break;

        case ENV_EXTREME:
            agTarget = 0.2f;
            transientFactor = 4.0f;
            noiseGate = 0.12f;
            compRatio = 5.0f;
            break;

        default:
            break;
    }

    // Adapt to musical content
    if (bassLevel > 0.6f) {
        bassWeight = 1.3f; // Emphasize bass response
    } else {
        bassWeight = 1.0f;
    }

    if (estimatedBPM > 0 && estimatedBPM < 100) {
        // Slower music - longer transient decay
        transientDecay = 4.0f;
    } else if (estimatedBPM > 140) {
        // Faster music - shorter transient decay
        transientDecay = 8.0f;
    }
}
