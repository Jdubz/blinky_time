# Rhythmic Pattern Memory (v77 Proposal)

*Drafted: March 18, 2026*

## Core Insight

Music is built on repeating patterns. A kick-snare pattern that has repeated for 8 bars is very likely to repeat on bar 9. By recording onset timestamps and finding periodicity in the intervals, the system discovers the rhythmic pattern WITHOUT assuming BPM is correct.

**Critical design principle: don't assume the BPM.** The pattern memory must work from raw onset timestamps upward — discover the repeating structure first, THEN use that structure to advise BPM selection and enable prediction. Thinking in musical divisions (quarter notes, bars) only makes sense once we have a confident repeating pattern AND a confident tempo. Until then, we work in absolute time.

This enables:

1. **BPM reinforcement** — the most common inter-onset interval IS the tempo (or a subdivision). The pattern tells us what the BPM should be.
2. **Onset prediction** — once a pattern repeats, predict where the next onset should occur.
3. **Visual stability** — blend between reactive (raw onset) and predictive (expected onset) for smoother animation.
4. **Anticipatory response** — subtle energy ramp before predicted events ("the lights know the music").

## Literature Basis

- **Krebs et al. (ISMIR 2013, 2015)**: Bar-position onset distributions in HMM state space. Key insight: pattern repetition disambiguates BPM octaves.
- **Scheirer (1998)**: Comb filter banks as resonant onset accumulators. The filters naturally discover the dominant periodicity.
- **Inter-Onset Interval (IOI) histogram**: Standard musicology tool. Histogram of time gaps between consecutive onsets. Peaks reveal the dominant rhythmic level.
- **ADAM model (van der Steen & Keller 2013)**: Sensorimotor synchronization — humans predict AND react. Pattern memory enables the prediction half.
- **BeatNet (ISMIR 2021)**: Particle filtering with onset pattern observation model.

## Two-Phase Architecture

The system operates in two phases that run concurrently:

### Phase A: IOI Accumulation (BPM-independent)

Record onset timestamps in a circular buffer. Compute inter-onset intervals (IOIs). Build an IOI histogram. The dominant IOI peak IS the tempo period (or a subdivision). This runs independently of the ACF/comb BPM estimate and provides a second opinion.

### Phase B: Bar-Position Histogram (BPM-dependent)

Once the IOI histogram has a confident peak AND the ACF/comb BPM agrees (or the IOI overrides it), project onset timestamps onto a bar-phase grid. Accumulate the bar-position histogram. This reveals the rhythmic pattern and enables prediction.

Phase B only activates when Phase A has established a confident periodicity. Until then, the system is purely reactive (existing v75/v76 behavior).

## Data Structures

```cpp
struct PatternMemory {
    // === Phase A: IOI accumulation (BPM-independent) ===
    // Circular buffer of recent onset timestamps (absolute time, ms)
    uint32_t onsetTimes[64];  // Last 64 onset times
    uint8_t onsetWriteIdx;
    uint8_t onsetCount;       // How many valid entries (0-64)

    // IOI histogram: 128 bins covering 100ms-1600ms (60-600 BPM range)
    // Bin width = ~12ms. Accumulated via EMA.
    // The strongest peak in this histogram = dominant rhythmic interval.
    float ioiBins[128];
    float ioiPeakMs;          // Time (ms) of strongest IOI peak
    float ioiPeakStrength;    // Strength of that peak (0-1)
    float ioiConfidence;      // Grows when IOI peak is stable, decays when shifting

    // === Phase B: Bar-position histogram (active when confident) ===
    // 16 bins per bar (16th-note resolution at the discovered tempo)
    float barBins[16];
    float barEntropy;         // Normalized Shannon entropy (0=predictable, 1=chaotic)
    float patternConfidence;  // Grows with repetition of bar pattern

    uint16_t barsAccumulated;
};
// Phase A: 64*4 + 128*4 + 3*4 + 2 = 782 bytes
// Phase B: 16*4 + 2*4 + 2 = 74 bytes
// Total: ~856 bytes (well under 2 KB)
```

### IOI Histogram Details

The IOI histogram covers the range 100ms to 1600ms in 128 bins (~12ms per bin).

| IOI range | BPM equivalent | Musical meaning |
|-----------|---------------|-----------------|
| 100-150ms | 400-600 BPM | 32nd notes at typical tempo |
| 150-250ms | 240-400 BPM | 16th notes |
| 250-500ms | 120-240 BPM | 8th notes / fast quarter notes |
| 500-1000ms | 60-120 BPM | Quarter notes at typical tempo |
| 1000-1600ms | 37-60 BPM | Half notes / very slow tempo |

The histogram naturally shows peaks at multiple rhythmic levels simultaneously. The STRONGEST peak that falls in the 250-1000ms range (60-240 BPM) is the best BPM candidate. Peaks at half or double that interval confirm the octave.

```cpp
void recordOnset(uint32_t nowMs) {
    // Add to circular buffer
    onsetTimes[onsetWriteIdx] = nowMs;
    onsetWriteIdx = (onsetWriteIdx + 1) % 64;
    if (onsetCount < 64) onsetCount++;

    // Compute IOIs against recent onsets (not just the previous one)
    // This captures patterns where not every onset is consecutive
    for (int back = 1; back <= min(onsetCount - 1, 8); back++) {
        int prevIdx = (onsetWriteIdx - 1 - back + 64) % 64;
        uint32_t ioi = nowMs - onsetTimes[prevIdx];
        if (ioi >= 100 && ioi <= 1600) {
            int bin = (ioi - 100) * 128 / 1500;  // Map to 0-127
            ioiBins[bin] = ioiBins[bin] * (1.0f - ioiLearnRate) + 1.0f * ioiLearnRate;
        }
    }
}
```

## Algorithm

### Phase A: IOI Discovery (runs always, BPM-independent)

Every onset, record the timestamp and compute intervals against the last 8 onsets:

```cpp
void recordOnset(uint32_t nowMs, float strength) {
    onsetTimes[onsetWriteIdx] = nowMs;
    onsetWriteIdx = (onsetWriteIdx + 1) % 64;
    if (onsetCount < 64) onsetCount++;

    // Compare against recent onsets to find recurring intervals
    for (int back = 1; back <= min(onsetCount - 1, 8); back++) {
        int prevIdx = (onsetWriteIdx - 1 - back + 64) % 64;
        uint32_t ioi = nowMs - onsetTimes[prevIdx];
        if (ioi >= 100 && ioi <= 1600) {
            int bin = (ioi - 100) * 128 / 1500;
            ioiBins[bin] += strength * ioiLearnRate;
        }
    }
}
```

Every ~500ms, find the dominant IOI peak and compare against the ACF BPM estimate:

```cpp
void updateIoiAnalysis() {
    // Decay all bins (forget old intervals)
    for (int i = 0; i < 128; i++) ioiBins[i] *= ioiDecayRate;

    // Find strongest peak in the 250-1000ms range (60-240 BPM)
    float bestVal = 0;
    int bestBin = -1;
    for (int i = 12; i < 75; i++) {  // 250ms-1000ms range
        if (ioiBins[i] > bestVal) {
            bestVal = ioiBins[i];
            bestBin = i;
        }
    }

    if (bestBin >= 0) {
        ioiPeakMs = 100.0f + bestBin * (1500.0f / 128.0f);
        ioiPeakBpm = 60000.0f / ioiPeakMs;
        ioiPeakStrength = bestVal;

        // Check if IOI agrees with ACF BPM (within 10% or at octave)
        float acfPeriodMs = 60000.0f / bpm_;
        float ratio = ioiPeakMs / acfPeriodMs;
        bool agrees = (fabsf(ratio - 1.0f) < 0.10f) ||
                      (fabsf(ratio - 0.5f) < 0.10f) ||
                      (fabsf(ratio - 2.0f) < 0.10f);

        if (agrees) {
            ioiConfidence = fminf(ioiConfidence + 0.05f, 1.0f);
        } else {
            ioiConfidence *= 0.9f;  // Disagrees — decay confidence
        }
    }
}
```

**Key insight**: The IOI histogram doesn't need the BPM to be correct — it discovers periodicity from raw onset timestamps. If the ACF says 140 BPM but the IOI histogram peaks at 357ms (168 BPM), the IOI provides corrective information. The IOI peak can nudge the BPM estimate toward the correct value.

### Phase B: Bar-Position Pattern (activates when confident)

Only runs when `ioiConfidence > 0.5` (the IOI histogram and ACF agree on a tempo). Uses the agreed tempo to project onsets onto a 16-bin bar grid:

```cpp
void updateBarHistogram(float onsetStrength, uint32_t nowMs) {
    if (ioiConfidence < 0.5f) return;  // Not confident enough

    // Use IOI peak period (not PLL phase) as the tempo reference
    float barPeriodMs = ioiPeakMs * 4.0f;  // 4 beats per bar
    float barPhase = fmodf((float)nowMs / barPeriodMs, 1.0f);
    int bin = (int)(barPhase * 16.0f) % 16;

    barBins[bin] = barBins[bin] * (1.0f - barLearnRate) + onsetStrength * barLearnRate;
}
```

### Decay (every frame)

```cpp
// IOI bins: half-life ~10 seconds (faster — tempo can change)
for (int i = 0; i < 128; i++) ioiBins[i] *= ioiDecayRate;
// Bar bins: half-life ~22 seconds (slower — pattern persists through breakdowns)
for (int i = 0; i < 16; i++) barBins[i] *= barDecayRate;
```

### Predict (every frame, only when pattern confident)

```cpp
float predictOnset(uint32_t nowMs) {
    if (patternConfidence < 0.3f) return 0.0f;

    float barPeriodMs = ioiPeakMs * 4.0f;
    float barPhase = fmodf((float)nowMs / barPeriodMs, 1.0f);
    int bin = (int)(barPhase * 16.0f) % 16;
    float frac = barPhase * 16.0f - floorf(barPhase * 16.0f);
    return (barBins[bin] * (1.0f - frac) + barBins[(bin + 1) % 16] * frac)
           * patternConfidence;
}
```

### Pattern Statistics (periodic, ~every 2 seconds)

- **Entropy** of bar histogram: low = predictable pattern, high = chaotic/no pattern
- **Pattern confidence**: grows with low entropy + repeating bars, decays fast on deviation
- **IOI-ACF agreement**: when the IOI histogram and ACF/comb agree, BPM confidence is high

The pattern confidence cascades: IOI must agree with ACF → bar histogram must have low entropy → multiple bars must repeat → prediction activates. Each layer gates the next.

## Integration Points

### 1. BPM advisory (IOI → ACF feedback)

The IOI histogram provides a second opinion on BPM, independent of spectral flux ACF. When the IOI peak disagrees with ACF, the IOI is likely more accurate because it's based on actual detected onset intervals rather than spectral periodicity.

```cpp
// In runAutocorrelation(), after ACF selects newBpm:
if (ioiConfidence > 0.5f && fabsf(ioiPeakBpm - newBpm) > newBpm * 0.1f) {
    // IOI disagrees with ACF — blend toward IOI
    float ioiWeight = ioiConfidence * 0.3f;  // Conservative: max 30% IOI influence
    newBpm = newBpm * (1.0f - ioiWeight) + ioiPeakBpm * ioiWeight;
}
```

This is a soft nudge, not an override. The ACF remains primary but the IOI provides corrective pressure when it has high confidence. This naturally helps with the 135 BPM gravity well — the IOI histogram peaks at the actual onset periodicity regardless of ACF bias.

### 2. Onset confidence boost (in confidence modulator)

After the existing cosine proximity calculation, multiply by pattern prediction:

```cpp
float prediction = predictOnset(nowMs);
float patternBoost = 1.0f + prediction * patternGain;
pulse *= patternBoost;
```

When no pattern established (patternConfidence=0), prediction returns 0, boost is 1.0x — invisible.

### 3. Anticipatory energy (the visual "magic")

Slightly ramp energy BEFORE predicted strong positions:

```cpp
float lookaheadMs = patternLookahead * ioiPeakMs;  // e.g., 5% of beat period
float prediction = predictOnset(nowMs + (uint32_t)lookaheadMs);
float anticipation = prediction * anticipationGain;
rawEnergy += anticipation;
```

This creates a subtle pre-flash glow before confident beats. The lights appear to "know" what's coming.

## Graceful Degradation

| Scenario | Behavior |
|----------|----------|
| **Steady pattern** | Confidence grows over 4-8 bars. Full prediction + anticipation. |
| **Breakdown/drop** | No onsets → bins decay but survive (~73% after 8 seconds). Pattern remembered. |
| **Drum fill** | Unexpected positions → entropy rises → confidence drops in 1-2 bars → reactive mode. |
| **Tempo change** | PLL phase shifts → histogram smears → old pattern decays → new accumulates in 8-16 bars. |
| **Ambient/free-form** | Low rhythmStrength → noisy histogram → high entropy → confidence stays 0 → invisible. |
| **Cold start** | All bins zero, confidence 0. System invisible for first 4-8 bars. |

## Parameters

| Parameter | Serial Name | Default | Range | Purpose |
|-----------|-------------|---------|-------|---------|
| patternLearnRate | `patlearn` | 0.15 | 0.01-0.5 | EMA alpha for histogram update |
| patternDecayRate | `patdecay` | 0.9995 | 0.990-0.9999 | Per-frame global decay |
| patternGain | `patgain` | 0.3 | 0.0-1.0 | Prediction boost strength |
| anticipationGain | `patanticipation` | 0.1 | 0.0-0.5 | Energy pre-ramp intensity |
| patternLookahead | `patlookahead` | 0.05 | 0.0-0.15 | Phase lookahead (fraction of beat) |
| patternEnabled | `patenabled` | true | bool | Master enable for A/B testing |

## Resource Cost

| Resource | Cost | Notes |
|----------|------|-------|
| RAM (Phase A) | 782 bytes | 64 timestamps + 128 IOI bins + stats |
| RAM (Phase B) | 74 bytes | 16 bar bins + stats |
| RAM total | **856 bytes** | Well under 2 KB target |
| CPU/onset | ~2 µs | IOI computation against 8 recent onsets |
| CPU/frame | ~1 µs | Decay + predict |
| CPU/500ms | ~5 µs | IOI peak finding + BPM comparison |
| Flash | ~600 bytes | Code for all algorithms |

## Implementation Plan

1. **Phase A: IOI accumulation** — onset timestamp buffer + IOI histogram + peak finding (~100 lines)
2. **IOI-ACF advisory** — soft BPM nudge in runAutocorrelation() (~10 lines)
3. **Phase B: bar histogram** — onset projection + pattern tracking (~60 lines)
4. **Onset confidence boost** — pattern prediction in synthesizeOutputs() (~10 lines)
5. **Anticipatory energy** — lookahead prediction (~8 lines)
6. **ConfigStorage + SerialConsole** — params + `show pattern` debug command (~40 lines)
7. **A/B testing** on blinkyhost

## Future: Template Bank

Pre-computed histograms for common patterns (four-on-the-floor, breakbeat, half-time, dembow, etc.). 8 templates × 64 bytes = 512 bytes flash. Cosine similarity matching. Enables faster cold-start and genre-aware behavior. Defer until core system is validated.

## Key Design Principles

1. **Don't assume BPM is correct.** The IOI histogram discovers periodicity from raw timestamps. Musical divisions only make sense once we have a confident, repeating interval.

2. **Two-phase activation.** Phase A (IOI) runs always. Phase B (bar pattern) only activates when Phase A has established a confident periodicity. Prediction only activates when Phase B has a low-entropy repeating pattern.

3. **Advise, don't override.** The IOI provides a soft nudge to BPM selection (max 30% influence). The pattern prediction boosts onset confidence but doesn't create phantom onsets. Every layer is additive to the existing system.

4. **The histogram is a low-pass filter on onset timing.** It converts irregular real-time events into a stable predictive pattern. Confidence grows with repetition and decays on deviation — no mode switching needed.
