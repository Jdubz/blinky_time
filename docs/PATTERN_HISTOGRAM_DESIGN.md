# Rhythmic Pattern Memory (v77 Proposal)

*Drafted: March 18, 2026*

## Core Insight

Music is built on repeating patterns. A kick-snare pattern that has repeated for 8 bars is very likely to repeat on bar 9. By accumulating a histogram of onset positions within a bar, the system learns the current rhythmic pattern and can predict where onsets SHOULD occur. This enables:

1. **Onset confidence boosting** — onsets at historically active positions are more trustworthy
2. **Anticipatory visual response** — subtle energy ramp before predicted events ("the lights know the music")
3. **BPM validation** — histogram shape disambiguates octave errors (4 peaks = quarter notes, 2 peaks = half-time)
4. **Visual stability** — blend between reactive (raw onset) and predictive (expected onset) for smoother animation

## Literature Basis

- **Krebs et al. (ISMIR 2013, 2015)**: Bar-position onset distributions in HMM state space. 64th-note grid, two frequency bands. "Drastically reduces octave errors."
- **Scheirer (1998)**: Comb filter banks as resonant onset accumulators — conceptually hardware-implemented circular histograms. (Already in our CombFilterBank.)
- **ADAM model (van der Steen & Keller 2013)**: Sensorimotor synchronization combining reactive adaptation with anticipatory prediction. Humans do both — react AND predict.
- **BeatNet (ISMIR 2021)**: Particle filtering with onset pattern observation model.

Our approach is a lightweight adaptation of Krebs's bar-position distributions, scaled for embedded (Cortex-M4F, 208 bytes RAM, ~1µs/frame).

## Data Structure

A **circular onset histogram** dividing one bar into 16 bins (16th-note resolution).

```
Bin:    0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
Beat:   1              2              3              4
Grid:   Q    .   16   8    Q    .   16   8    Q    .   16   8    Q    .   16   8
```

16 bins is the drum machine standard — captures all common subdivisions (quarter, 8th, 16th) without requiring sub-16th resolution that our onset detector can't reliably provide. At 120 BPM, each bin spans ~125ms.

```cpp
struct PatternHistogram {
    float bins[16];           // EMA-accumulated onset weights per bar position
    float bins2bar[32];       // 2-bar variant (captures verse/chorus alternation)
    float peakMean;           // Mean of top-4 bin values (pattern strength)
    float entropy;            // Normalized Shannon entropy (0=predictable, 1=chaotic)
    float patternConfidence;  // Grows with repetition, decays on deviation
    uint16_t barsAccumulated; // Bars observed
    uint8_t currentBin;
    uint8_t currentBar;       // mod 2 for 2-bar tracking
};
// Total: 208 bytes
```

## Algorithm

### Update (every frame with onset)

When an onset fires, add its strength to the current bar-position bin via EMA:

```cpp
void updatePatternHistogram(float onsetStrength, float phase) {
    float barPhase = fmodf(phase + (beatCount_ % 4) * 0.25f, 1.0f);
    int bin = (int)(barPhase * 16.0f) % 16;
    bins[bin] = bins[bin] * (1.0f - patternLearnRate) + onsetStrength * patternLearnRate;
}
```

### Decay (every frame)

All bins decay slowly to forget stale patterns after music changes:

```cpp
// patternDecayRate=0.9995: half-life ~22 seconds
// Old pattern survives through 8-second breakdown, new pattern establishes in 4-8 bars
for (int i = 0; i < 16; i++) bins[i] *= patternDecayRate;
```

### Predict (every frame)

Look up current bar position, interpolate between adjacent bins:

```cpp
float predictOnsetStrength(float phase) {
    float barPhase = fmodf(phase + (beatCount_ % 4) * 0.25f, 1.0f);
    int bin = (int)(barPhase * 16.0f) % 16;
    float frac = barPhase * 16.0f - floorf(barPhase * 16.0f);
    return bins[bin] * (1.0f - frac) + bins[(bin + 1) % 16] * frac;
}
```

### Pattern Statistics (every bar)

Compute entropy and confidence at approximate bar boundaries:

- **Entropy**: Shannon entropy of normalized histogram. Low = peaked/predictable, high = uniform/chaotic.
- **Pattern confidence**: Grows slowly (alpha=0.05) when entropy is low, decays fast (alpha=0.15) when entropy is high. This creates the repetition-builds-confidence dynamic.

## Integration Points

### 1. Onset confidence boost (in confidence modulator)

After the existing cosine proximity calculation, multiply by pattern prediction:

```cpp
float patternExpectation = getPatternPrediction(pllPhase_);
float patternBoost = 1.0f + patternExpectation * patternGain * patternConfidence;
pulse *= patternBoost;
```

When no pattern established (patternConfidence=0), boost is 1.0x — invisible.

### 2. Anticipatory energy (the visual "magic")

Slightly ramp energy BEFORE predicted strong positions:

```cpp
float lookaheadPhase = fmodf(barPhase + patternLookahead, 1.0f);
float anticipation = bins[lookaheadBin] * patternConfidence * anticipationGain;
rawEnergy += anticipation;
```

At `patternLookahead=0.05` (5% of beat = ~25ms at 120 BPM), this creates a subtle pre-flash glow. The lights appear to "know" what's coming.

### 3. BPM diagnostic (informational)

The histogram shape reveals BPM octave status: 4 equal peaks = correct quarter notes, 2 peaks = half-time. Exposed as diagnostic data, not automatic correction.

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
| RAM | 208 bytes | 16+32 floats + stats + state |
| CPU/frame | ~1 µs | 50 MACs for update+decay+predict |
| CPU/bar | ~3 µs | Entropy + confidence (every ~2s) |
| Flash | ~400 bytes | Code for update/decay/predict/stats |

## Future: Template Bank

Pre-computed histograms for common patterns (four-on-the-floor, breakbeat, half-time, dembow, etc.). 8 templates × 64 bytes = 512 bytes flash. Cosine similarity matching. Enables faster cold-start and octave disambiguation. Defer until core histogram is validated.

## Implementation Plan

1. **Core histogram** — struct + update/decay/predict in AudioTracker (~80 lines)
2. **Confidence integration** — boost in synthesizeOutputs() (~10 lines)
3. **Anticipatory energy** — lookahead in synthesizeOutputs() (~8 lines)
4. **ConfigStorage + SerialConsole** — params + debug output (~40 lines)
5. **A/B testing** on blinkyhost

## Key Insight

The histogram is not trying to "understand" the music — it's a low-pass filter on onset timing that converts irregular real-time events into a stable predictive pattern. The pattern confidence mechanism naturally handles the transition between "I have a prediction" (steady groove) and "I don't know what's happening" (breakdown/fill/tempo change) without any mode switching.
