# Rhythm Analysis Improvement Plan

**Created:** February 16, 2026
**Goal:** Implement independent, musically-aware rhythm analysis systems that can cooperate but don't rely on each other

---

## Executive Summary

This plan implements 5 independent rhythm analysis improvements, each tested in isolation before integration. The approach maintains the existing energy-periodicity foundation while adding complementary systems for phase estimation, tempo validation, and multi-band adaptation.

**Key Principle:** Each system must work independently. Transients are hints, never control.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                            Audio Input                                   │
│                                 │                                        │
│     ┌───────────────────────────┼───────────────────────────┐           │
│     │                           │                           │           │
│     ▼                           ▼                           ▼           │
│ ┌─────────────┐         ┌─────────────┐           ┌─────────────────┐  │
│ │   System 1  │         │   System 2  │           │    System 3     │  │
│ │ Spectral    │         │  PLP-style  │           │  Comb Filter    │  │
│ │ Flux OSS    │         │  Sinusoidal │           │  Resonator      │  │
│ │ + Autocorr  │         │  Phase Fit  │           │  Phase Tracker  │  │
│ └──────┬──────┘         └──────┬──────┘           └────────┬────────┘  │
│        │                       │                           │           │
│        │ tempo₁, phase₁        │ phase₂, conf₂             │ phase₃    │
│        │ confidence₁           │                           │           │
│        │                       │                           │           │
│     ┌──▼───────────────────────▼───────────────────────────▼──┐        │
│     │                     FUSION LAYER                         │        │
│     │  • Weighted average based on per-system confidence       │        │
│     │  • Transient voting (hint influence, max ±10%)           │        │
│     │  • Consensus detection and conflict resolution           │        │
│     └──────────────────────────┬──────────────────────────────┘        │
│                                │                                        │
│                                ▼                                        │
│                    Final: BPM, Phase, Confidence                        │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Implementation Phases

| Phase | System | Description | Est. Effort |
|-------|--------|-------------|-------------|
| **1** | Spectral Flux OSS | Replace RMS with half-wave rectified spectral flux | 2-3 hours |
| **2** | Adaptive Band Weighting | Weight bands by their periodicity contribution | 2-3 hours |
| **3** | Pulse Train Phase | Cross-correlate OSS with pulse trains for phase | 3-4 hours |
| **4** | Comb Filter Resonator | Parallel phase tracker using comb filter | 3-4 hours |
| **5** | Fusion Layer | Combine all systems with confidence weighting | 2-3 hours |

**Total estimated implementation time:** 12-17 hours (not including testing)

---

## Phase 1: Spectral Flux OSS

### Objective
Replace RMS-based onset strength with half-wave rectified spectral flux, which captures energy *changes* rather than absolute levels.

### Current Implementation (AudioController.cpp:122-148)
```cpp
// Current: Multi-band RMS energy
onsetStrength = 0.5f * bassEnergy + 0.3f * midEnergy + 0.2f * highEnergy;
```

### New Implementation
```cpp
// New: Half-wave rectified spectral flux
float spectralFlux = 0.0f;
const float* magnitudes = spectral.getMagnitudes();
static float prevMagnitudes[128] = {0};

for (int i = 1; i < numBins; i++) {
    float diff = magnitudes[i] - prevMagnitudes[i];
    if (diff > 0) {  // Half-wave rectify: only increases
        spectralFlux += diff;
    }
    prevMagnitudes[i] = magnitudes[i];
}

// Normalize by number of bins
spectralFlux /= (float)(numBins - 1);

// Optional: Apply log compression for dynamic range
onsetStrength = logf(1.0f + spectralFlux * 10.0f) / logf(11.0f);
```

### Implementation Steps

1. **Add previous magnitude buffer to AudioController**
   ```cpp
   // In AudioController.h, add private member:
   float prevMagnitudes_[128] = {0};
   ```

2. **Add tunable parameter for flux vs RMS blend**
   ```cpp
   // In AudioController.h:
   float ossFluxWeight = 1.0f;  // 1.0 = pure flux, 0.0 = pure RMS
   ```

3. **Implement hybrid OSS calculation**
   ```cpp
   // Calculate both
   float fluxOss = computeSpectralFlux(magnitudes, numBins);
   float rmsOss = 0.5f * bassEnergy + 0.3f * midEnergy + 0.2f * highEnergy;

   // Blend
   onsetStrength = ossFluxWeight * fluxOss + (1.0f - ossFluxWeight) * rmsOss;
   ```

4. **Register parameter for serial tuning**
   - Add `ossfluxweight` to SettingsRegistry
   - Range: 0.0 - 1.0, default: 1.0

### Testing Protocol

**Test 1: Baseline comparison (RMS vs Flux)**
```
1. Set ossfluxweight = 0.0 (pure RMS - current behavior)
2. Run: run_test pattern=strong-beats port=COM5
3. Record: F1, detected BPM accuracy, phase consistency
4. Set ossfluxweight = 1.0 (pure flux)
5. Run same test
6. Compare results
```

**Test 2: Sustained content rejection**
```
1. Set ossfluxweight = 1.0
2. Run: run_test pattern=pad-rejection port=COM5
3. Compare phase stability with ossfluxweight = 0.0
4. Expect: Flux should show less phase drift on sustained pads
```

**Note:** Let AGC auto-adapt - do not lock gain unless specifically testing AGC behavior.

**Test 3: Full pattern sweep**
```
Patterns to test (3 runs each, average results):
- strong-beats (baseline)
- pad-rejection (sustained content)
- chord-rejection (chord changes)
- full-mix (realistic)
- tempo-sweep (BPM accuracy)
```

### Success Criteria
- [ ] F1 on strong-beats: ≥ 0.95 (no regression)
- [ ] Phase stability on pad-rejection: Improved or equal
- [ ] BPM accuracy on tempo-sweep: ≤ 5% error across all tempos
- [ ] No increase in false positives on rejection patterns

### Debug Commands
```
# Enable rhythm debug output
debug rhythm on

# Monitor OSS buffer health
send_command "show ossfluxweight"

# Watch spectral flux in real-time
stream_start
# Observe RHYTHM_DEBUG JSON output with flux values
```

---

## Phase 2: Adaptive Band Weighting

### Objective
Track periodicity strength per frequency band and weight bands by their rhythmic contribution.

### Current Implementation
Fixed weights: `0.5 * bass + 0.3 * mid + 0.2 * high`

### New Implementation

**A. Per-band OSS buffers**
```cpp
// In AudioController.h:
static constexpr int BAND_COUNT = 3;  // bass, mid, high
float bandOssBuffers_[BAND_COUNT][OSS_BUFFER_SIZE] = {0};
float bandPeriodicityStrength_[BAND_COUNT] = {0};
float adaptiveBandWeights_[BAND_COUNT] = {0.5f, 0.3f, 0.2f};  // Default
```

**B. Per-band autocorrelation (simplified)**
```cpp
void updateBandPeriodicities(uint32_t nowMs) {
    for (int band = 0; band < BAND_COUNT; band++) {
        // Run simplified autocorrelation on each band
        float maxCorr = computeBandAutocorrelation(band);

        // Smooth the periodicity estimate
        bandPeriodicityStrength_[band] =
            0.9f * bandPeriodicityStrength_[band] + 0.1f * maxCorr;
    }

    // Normalize weights based on periodicity
    float totalStrength = 0.0f;
    for (int i = 0; i < BAND_COUNT; i++) {
        totalStrength += bandPeriodicityStrength_[i];
    }

    if (totalStrength > 0.1f) {
        for (int i = 0; i < BAND_COUNT; i++) {
            // Blend adaptive weight with default (50/50)
            float adaptiveWeight = bandPeriodicityStrength_[i] / totalStrength;
            float defaultWeight = (i == 0) ? 0.5f : (i == 1) ? 0.3f : 0.2f;
            adaptiveBandWeights_[i] = 0.5f * adaptiveWeight + 0.5f * defaultWeight;
        }
    }
}
```

**C. Apply adaptive weights to OSS**
```cpp
onsetStrength = adaptiveBandWeights_[0] * bassFlux +
                adaptiveBandWeights_[1] * midFlux +
                adaptiveBandWeights_[2] * highFlux;
```

### Implementation Steps

1. **Add per-band OSS buffers** (memory: 3 × 360 × 4 = 4.3 KB)
2. **Implement simplified per-band autocorrelation** (runs every 1000ms, not 500ms)
3. **Add adaptive weight calculation**
4. **Add enable/disable parameter** (`adaptivebandweight`)
5. **Add debug output for band weights**

### Testing Protocol

**Test 1: Genre adaptation**
```
1. Enable adaptive weighting: set adaptivebandweight 1
2. Play bass-heavy pattern (bass-line)
   - Observe: Bass weight should increase
3. Play treble-heavy pattern (hat-patterns)
   - Observe: High weight should increase
```

**Test 2: Phase stability comparison**
```
1. Disable adaptive: set adaptivebandweight 0
2. Run: monitor_music pattern=full-mix duration_ms=10000
3. Record: phase variance, BPM stability
4. Enable adaptive: set adaptivebandweight 1
5. Repeat test, compare results
```

### Success Criteria
- [ ] Band weights visibly shift based on content (debug output)
- [ ] No regression on strong-beats F1
- [ ] Improved BPM accuracy on genre-diverse patterns
- [ ] Memory usage confirmed ≤ 5 KB additional

### Debug Commands
```
# Watch band weights in real-time
debug rhythm on
# Output will include: {"bandWeights":[0.45,0.35,0.20]}

# Check per-band periodicity
send_command "show bandperiodicity"
```

---

## Phase 3: Pulse Train Cross-Correlation Phase

### Objective
Replace peak-finding phase derivation with pulse train cross-correlation for more robust phase estimation.

### Current Implementation (AudioController.cpp:589-606)
```cpp
// Find max OSS in recent beat period → derive phase
for (int i = 0; i < recentWindow && i < ossCount_; i++) {
    if (ossBuffer_[idx] > maxRecent) {
        maxRecent = ossBuffer_[idx];
        maxRecentIdx = i;
    }
}
targetPhase_ = static_cast<float>(maxRecentIdx) / static_cast<float>(bestLag);
```

### New Implementation

**A. Pulse train generation**
```cpp
float generatePulseTrain(int phase_offset, int beat_period, int buffer_size, float* output) {
    // Generate ideal pulse train at given tempo and phase
    for (int i = 0; i < buffer_size; i++) {
        int pos_in_beat = (i + phase_offset) % beat_period;
        // Gaussian pulse centered at 0, width ~10% of beat period
        float dist = (float)pos_in_beat / (float)beat_period;
        if (dist > 0.5f) dist = 1.0f - dist;  // Wrap around
        float sigma = 0.1f;  // 10% of beat period
        output[i] = expf(-0.5f * (dist / sigma) * (dist / sigma));
    }
}
```

**B. Cross-correlation for phase**
```cpp
float findBestPhase(int beat_period) {
    float bestCorr = -1.0f;
    int bestPhaseOffset = 0;
    float pulseTrain[OSS_BUFFER_SIZE];

    // Test phase offsets at 10% resolution
    int phaseSteps = 10;
    int stepSize = beat_period / phaseSteps;

    for (int phase = 0; phase < beat_period; phase += stepSize) {
        generatePulseTrain(phase, beat_period, ossCount_, pulseTrain);

        float correlation = 0.0f;
        for (int i = 0; i < ossCount_; i++) {
            int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
            correlation += ossBuffer_[idx] * pulseTrain[i];
        }

        if (correlation > bestCorr) {
            bestCorr = correlation;
            bestPhaseOffset = phase;
        }
    }

    // Convert to phase (0-1)
    return (float)bestPhaseOffset / (float)beat_period;
}
```

**C. Blend with existing phase**
```cpp
// Compute pulse train phase
float pulseTrainPhase = findBestPhase(bestLag);

// Blend with existing targetPhase_ (smooth transition)
targetPhase_ = 0.7f * pulseTrainPhase + 0.3f * targetPhase_;
```

### Implementation Steps

1. **Add pulse train generation function**
2. **Add cross-correlation phase finder**
3. **Add tunable blend parameter** (`pulsephaseweight`)
4. **Integrate into runAutocorrelation()**
5. **Add phase confidence output** (correlation strength)

### Testing Protocol

**Test 1: Phase accuracy**
```
1. Enable pulse train phase: set pulsephaseweight 1.0
2. Run: run_test pattern=strong-beats port=COM5
3. Monitor: Phase alignment with detected transients
4. Compare with pulsephaseweight = 0.0 (current method)
```

**Test 2: Noisy conditions**
```
1. Run: run_test pattern=full-mix port=COM5
2. Observe: Phase stability over 16 seconds
3. Record: Phase variance (std dev of phase values)
4. Compare with/without pulse train method
```

**Test 3: Tempo changes**
```
1. Run: run_test pattern=tempo-sweep port=COM5
2. Monitor: Phase re-lock time after tempo change
3. Expect: Faster phase lock with pulse train method
```

### Success Criteria
- [ ] Phase variance reduced by ≥ 20% on full-mix
- [ ] Phase re-lock time ≤ 2 beats after tempo change
- [ ] No regression on transient F1 scores
- [ ] CPU usage increase ≤ 2%

### Debug Commands
```
# Enable phase debug output
debug hypothesis on
set hypodebug 2

# Monitor phase estimates
# Output includes: targetPhase, pulsePhase, blendedPhase
```

---

## Phase 4: Comb Filter Resonator Phase Tracker

### Objective
Add an independent phase tracking system using comb filter resonance.

### Theory
A comb filter at lag L accumulates energy when the input has periodicity at L samples. The phase of the accumulated signal indicates beat alignment.

### New Implementation

**A. Comb filter state**
```cpp
// In AudioController.h:
class CombFilterPhaseTracker {
public:
    static constexpr int MAX_LAG = 120;  // ~0.5s at 60Hz = 30 BPM min

    float delayLine[MAX_LAG] = {0};
    int writeIdx = 0;
    float resonatorOutput = 0.0f;
    float resonatorPhase = 0.0f;

    // Tuning
    float feedbackGain = 0.95f;  // Resonance strength (0.9-0.99)
    int currentLag = 30;          // Current beat period in samples

    void setTempo(float bpm, float frameRate = 60.0f);
    void process(float input);
    float getPhase() const;
    float getConfidence() const;
};
```

**B. Comb filter processing**
```cpp
void CombFilterPhaseTracker::process(float input) {
    // Read from delay line
    int readIdx = (writeIdx - currentLag + MAX_LAG) % MAX_LAG;
    float delayed = delayLine[readIdx];

    // Resonator: y[n] = x[n] + feedback * y[n-L]
    resonatorOutput = input + feedbackGain * delayed;

    // Write to delay line
    delayLine[writeIdx] = resonatorOutput;
    writeIdx = (writeIdx + 1) % MAX_LAG;

    // Estimate phase from resonator output
    // Phase accumulates as resonator rings
    updatePhaseEstimate();
}

void CombFilterPhaseTracker::updatePhaseEstimate() {
    // Track zero crossings or peak positions in resonator output
    static float prevOutput = 0.0f;
    static int samplesSincePeak = 0;

    samplesSincePeak++;

    if (resonatorOutput > prevOutput && prevOutput < 0.0f) {
        // Rising zero crossing - approximate beat position
        resonatorPhase = (float)samplesSincePeak / (float)currentLag;
        resonatorPhase = fmodf(resonatorPhase, 1.0f);
        samplesSincePeak = 0;
    }

    prevOutput = resonatorOutput;
}
```

**C. Integration into AudioController**
```cpp
// In AudioController.h:
CombFilterPhaseTracker combFilter_;
float combFilterPhaseWeight = 0.0f;  // Disabled by default

// In update():
if (combFilterPhaseWeight > 0.0f) {
    // Set comb filter to track primary hypothesis tempo
    combFilter_.setTempo(bpm_);
    combFilter_.process(onsetStrength);

    float combPhase = combFilter_.getPhase();
    float combConf = combFilter_.getConfidence();

    // Will be used in fusion layer
}
```

### Implementation Steps

1. **Create CombFilterPhaseTracker class** (new file or in AudioController.h)
2. **Add delay line buffer** (120 samples × 4 bytes = 480 bytes)
3. **Implement resonator processing**
4. **Implement phase extraction from resonator**
5. **Add tuning parameters** (`combfeedback`, `combweight`)
6. **Integrate into main update loop**

### Testing Protocol

**Test 1: Resonator lock-in**
```
1. Enable comb filter: set combweight 1.0
2. Run: run_test pattern=strong-beats port=COM5
3. Monitor: Resonator output amplitude (should grow and stabilize)
4. Observe: Phase output (should lock to beat)
```

**Test 2: Phase comparison**
```
1. Run: monitor_music pattern=basic-drums duration_ms=15000
2. Compare: Comb filter phase vs autocorrelation phase
3. Measure: Phase difference over time
4. Expect: < 0.1 phase difference when both locked
```

**Test 3: Tempo tracking**
```
1. Run: run_test pattern=tempo-sweep port=COM5
2. Monitor: Comb filter adaptation to tempo changes
3. Measure: Lag until phase re-lock after tempo change
```

### Success Criteria
- [ ] Comb filter phase tracks within ±0.1 of autocorr phase on steady tempo
- [ ] Resonator stabilizes within 4 beats of music start
- [ ] Memory usage: < 1 KB additional
- [ ] CPU usage: < 1% additional

### Debug Commands
```
# Enable comb filter debug
debug rhythm on

# Monitor resonator state
send_command "show combphase"
send_command "show combconf"
```

---

## Phase 5: Fusion Layer

### Objective
Combine outputs from all systems with confidence-based weighting and transient hints.

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        FUSION LAYER                              │
│                                                                  │
│  Inputs:                                                         │
│  ├─ System 1: tempo₁, phase₁, confidence₁ (autocorr)            │
│  ├─ System 2: phase₂, confidence₂ (pulse train)                 │
│  └─ System 3: phase₃, confidence₃ (comb filter)                 │
│                                                                  │
│  Transient Hints:                                                │
│  └─ transientPhaseVote: ±0.1 max influence                       │
│                                                                  │
│  Processing:                                                     │
│  1. Normalize confidences to sum to 1.0                          │
│  2. Weighted average of phases                                   │
│  3. Apply transient hint (clamped to ±0.1)                       │
│  4. Compute consensus metric                                     │
│                                                                  │
│  Outputs:                                                        │
│  ├─ fusedPhase: Final phase estimate                             │
│  ├─ fusedConfidence: Agreement-based confidence                  │
│  └─ consensusMetric: How much systems agree (0-1)                │
└─────────────────────────────────────────────────────────────────┘
```

### Implementation

**A. Fusion state**
```cpp
// In AudioController.h:
struct RhythmFusion {
    // System outputs
    float phases[3] = {0};        // [autocorr, pulseTrain, combFilter]
    float confidences[3] = {0};

    // Fused output
    float fusedPhase = 0.0f;
    float fusedConfidence = 0.0f;
    float consensusMetric = 0.0f;

    // Transient hint
    float transientHint = 0.0f;
    float transientHintWeight = 0.1f;  // Max 10% influence

    // Enable flags
    bool useAutocorr = true;
    bool usePulseTrain = true;
    bool useCombFilter = true;
};
```

**B. Fusion algorithm**
```cpp
void AudioController::fuseRhythmEstimates() {
    // Collect estimates
    fusion_.phases[0] = phase_;                    // Autocorr
    fusion_.confidences[0] = periodicityStrength_;

    fusion_.phases[1] = pulseTrainPhase_;          // Pulse train
    fusion_.confidences[1] = pulseTrainConfidence_;

    fusion_.phases[2] = combFilter_.getPhase();    // Comb filter
    fusion_.confidences[2] = combFilter_.getConfidence();

    // Normalize confidences
    float totalConf = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (isSystemEnabled(i)) {
            totalConf += fusion_.confidences[i];
        }
    }

    if (totalConf < 0.1f) {
        // No confident estimate - use autocorr as fallback
        fusion_.fusedPhase = phase_;
        fusion_.fusedConfidence = 0.0f;
        fusion_.consensusMetric = 0.0f;
        return;
    }

    // Weighted average of phases (handle wraparound)
    float sinSum = 0.0f, cosSum = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (isSystemEnabled(i)) {
            float weight = fusion_.confidences[i] / totalConf;
            float angle = fusion_.phases[i] * 2.0f * M_PI;
            sinSum += weight * sinf(angle);
            cosSum += weight * cosf(angle);
        }
    }
    fusion_.fusedPhase = atan2f(sinSum, cosSum) / (2.0f * M_PI);
    if (fusion_.fusedPhase < 0.0f) fusion_.fusedPhase += 1.0f;

    // Apply transient hint (clamped)
    float hintInfluence = fusion_.transientHint * fusion_.transientHintWeight;
    hintInfluence = clampf(hintInfluence, -0.1f, 0.1f);
    fusion_.fusedPhase += hintInfluence;
    fusion_.fusedPhase = fmodf(fusion_.fusedPhase + 1.0f, 1.0f);

    // Compute consensus (how much do systems agree?)
    float maxDiff = 0.0f;
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (isSystemEnabled(i) && isSystemEnabled(j)) {
                float diff = fabsf(fusion_.phases[i] - fusion_.phases[j]);
                if (diff > 0.5f) diff = 1.0f - diff;  // Wraparound
                maxDiff = fmaxf(maxDiff, diff);
            }
        }
    }
    fusion_.consensusMetric = 1.0f - maxDiff * 2.0f;  // 0 = 0.5 diff, 1 = 0 diff

    // Fused confidence = avg confidence × consensus
    fusion_.fusedConfidence = (totalConf / 3.0f) * fusion_.consensusMetric;
}
```

### Implementation Steps

1. **Add RhythmFusion struct to AudioController**
2. **Implement weighted circular mean for phase fusion**
3. **Implement consensus metric**
4. **Add transient hint calculation** (from existing phase correction)
5. **Add enable/disable flags per system**
6. **Replace phase_ output with fusedPhase when enabled**
7. **Add debug output for all fusion inputs/outputs**

### Testing Protocol

**Test 1: Single-system validation**
```
1. Enable only autocorr: set fusion_autocorr 1, fusion_pulse 0, fusion_comb 0
2. Run full test suite - should match baseline exactly
3. Enable only pulse train: set fusion_autocorr 0, fusion_pulse 1, fusion_comb 0
4. Run full test suite - record results
5. Enable only comb filter: set fusion_autocorr 0, fusion_pulse 0, fusion_comb 1
6. Run full test suite - record results
```

**Test 2: Pairwise combinations**
```
Test all 3 pairwise combinations:
- autocorr + pulse
- autocorr + comb
- pulse + comb

For each:
1. Run: run_test pattern=full-mix port=COM5
2. Record: Phase stability, consensus metric, confidence
```

**Test 3: Full fusion**
```
1. Enable all: set fusion_autocorr 1, fusion_pulse 1, fusion_comb 1
2. Run full test suite
3. Compare with single-system results
4. Expect: Best overall phase stability
```

**Test 4: Transient hint validation**
```
1. Set transient_hint_weight = 0.0 (disabled)
2. Run: run_test pattern=strong-beats port=COM5
3. Set transient_hint_weight = 0.1 (10% max influence)
4. Run same test
5. Compare: Phase alignment to actual transients
```

### Success Criteria
- [ ] Fusion improves phase stability over any single system
- [ ] Consensus metric > 0.8 on strong-beats
- [ ] No single system can cause > 10% phase deviation
- [ ] Graceful degradation when systems disagree

### Debug Commands
```
# Enable fusion debug
debug fusion on

# Show all system outputs
send_command "show fusion"

# Output format:
# {"phases":[0.12,0.14,0.11],"confs":[0.85,0.72,0.68],
#  "fused":0.13,"consensus":0.94,"hint":0.02}
```

---

## Testing Infrastructure Enhancements

### New MCP Tools Needed

**1. monitor_rhythm** - Extended rhythm monitoring
```typescript
{
  name: 'monitor_rhythm',
  description: 'Monitor rhythm tracking state over duration',
  inputSchema: {
    type: 'object',
    properties: {
      duration_ms: { type: 'number', default: 5000 },
      sample_rate_hz: { type: 'number', default: 10 }
    }
  }
}
```

Output:
```json
{
  "samples": [
    {
      "timestampMs": 0,
      "bpm": 120.2,
      "phase": 0.12,
      "confidence": 0.85,
      "consensus": 0.92,
      "systems": {
        "autocorr": { "phase": 0.12, "conf": 0.88 },
        "pulse": { "phase": 0.14, "conf": 0.75 },
        "comb": { "phase": 0.11, "conf": 0.72 }
      }
    },
    ...
  ],
  "stats": {
    "phaseVariance": 0.012,
    "bpmMean": 120.1,
    "bpmStdDev": 0.8,
    "avgConsensus": 0.89
  }
}
```

**2. compare_systems** - A/B comparison of rhythm systems
```typescript
{
  name: 'compare_systems',
  description: 'Compare rhythm tracking between system configurations',
  inputSchema: {
    type: 'object',
    properties: {
      pattern: { type: 'string' },
      config_a: { type: 'object' },  // System enable flags
      config_b: { type: 'object' }
    }
  }
}
```

### New Test Patterns Needed

**1. steady-tempo-long** (30 seconds)
- Constant 120 BPM for extended stability measurement
- Used for: Phase variance measurement, long-term drift detection

**2. phase-shift-test**
- Abrupt phase shift mid-pattern (beat skips forward/back)
- Used for: Phase recovery testing

**3. tempo-ramp**
- Gradual tempo change: 100 → 140 BPM over 20 seconds
- Used for: Continuous tempo tracking

---

## Implementation Schedule

### Week 1: Foundation
- [ ] Day 1-2: Phase 1 (Spectral Flux OSS)
- [ ] Day 3: Phase 1 testing and tuning
- [ ] Day 4-5: Phase 2 (Adaptive Band Weighting)

### Week 2: Phase Systems
- [ ] Day 1-2: Phase 3 (Pulse Train Phase)
- [ ] Day 3: Phase 3 testing
- [ ] Day 4-5: Phase 4 (Comb Filter Resonator)

### Week 3: Integration
- [ ] Day 1-2: Phase 5 (Fusion Layer)
- [ ] Day 3-4: Full system testing
- [ ] Day 5: Documentation and parameter tuning

---

## Parameter Summary

| Parameter | System | Default | Range | Description |
|-----------|--------|---------|-------|-------------|
| `ossfluxweight` | Phase 1 | 1.0 | 0-1 | Spectral flux vs RMS blend |
| `adaptivebandweight` | Phase 2 | 0 | 0-1 | Enable adaptive band weights |
| `pulsephaseweight` | Phase 3 | 1.0 | 0-1 | Pulse train phase influence |
| `combfeedback` | Phase 4 | 0.95 | 0.9-0.99 | Comb filter resonance |
| `combweight` | Phase 4 | 0.5 | 0-1 | Comb filter contribution |
| `fusion_autocorr` | Phase 5 | 1 | 0-1 | Enable autocorr in fusion |
| `fusion_pulse` | Phase 5 | 1 | 0-1 | Enable pulse train in fusion |
| `fusion_comb` | Phase 5 | 1 | 0-1 | Enable comb filter in fusion |
| `transient_hint_weight` | Phase 5 | 0.1 | 0-0.2 | Max transient influence |

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Memory overflow | Pre-calculate buffer sizes, static allocation only |
| CPU overload | Profile each phase, budget 5% per system max |
| Phase instability | Always maintain fallback to current autocorr system |
| Testing variance | 3-run averaging, locked gain during tests |
| Parameter explosion | Limit to 10 new parameters total |

---

## Success Metrics (Final Validation)

After all phases complete, the system should achieve:

| Metric | Target | Current |
|--------|--------|---------|
| Phase stability (full-mix) | σ < 0.05 | TBD |
| BPM accuracy (tempo-sweep) | < 3% error | ~5% |
| Phase recovery time | < 2 beats | TBD |
| Consensus (strong-beats) | > 0.9 | N/A |
| F1 (transient, avg) | ≥ 0.78 | 0.78 |
| Memory increase | < 8 KB | N/A |
| CPU increase | < 10% | N/A |

---

## References

- [Predominant Local Pulse (PLP)](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S3_PredominantLocalPulse.html)
- [Spectral-Based Novelty](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S1_NoveltySpectral.html)
- [Ellis: Beat Tracking by DP](https://www.ee.columbia.edu/~dpwe/pubs/Ellis07-beattrack.pdf)
- [Scheirer: Tempo and Beat Analysis](https://cagnazzo.wp.imt.fr/files/2013/05/Scheirer98.pdf)
- [Dixon: Onset Detection Revisited](https://www.dafx.de/paper-archive/2006/papers/p_133.pdf)
