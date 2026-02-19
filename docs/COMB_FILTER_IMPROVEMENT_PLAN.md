# Comb Filter Improvement Plan

**Created:** February 16, 2026
**Goal:** Fix the comb filter implementation to match state-of-the-art approaches

---

## Executive Summary

Our current comb filter implementation causes -15-21% F1 regression because it has fundamental architectural issues compared to state-of-the-art approaches. This plan identifies specific gaps and provides actionable fixes.

---

## Research Summary

### State-of-the-Art Approaches

| System | Key Technique | Phase Extraction |
|--------|--------------|------------------|
| **BTrack** (Stark) | ACF + comb filter bank (41 tempos) | Cumulative score with log-Gaussian windows |
| **Real-Time PLP** (AudioLabs 2024) | Complex exponential at tempo | `phase = -angle(c) / 2π` from Fourier |
| **Scheirer (1998)** | 6 bands × comb filter bank | Phase-locked behavior analysis |
| **Comb Filter Matrix** (ICMC 2011) | Matrix retaining tempo/phase | Visual matrix peak tracking |

### Key Sources

- [BTrack GitHub](https://github.com/adamstark/BTrack) - C++ implementation
- [Real-Time PLP GitHub](https://github.com/groupmm/real_time_plp) - Python implementation
- [Scheirer 1998 Paper](https://www.ee.columbia.edu/~dpwe/papers/Schei98-beats.pdf) - Foundational comb filter work
- [ICMC 2011 - Comb Filter Matrix](https://www.researchgate.net/publication/298980631_Real-time_Visual_Beat_Tracking_using_a_Comb_Filter_Matrix)
- [AudioLabs PLP Documentation](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S3_PredominantLocalPulse.html)

---

## Critical Gaps in Our Implementation

### Gap 1: Single Filter vs Filter Bank

**Our approach:**
```cpp
// One comb filter at tempo from autocorrelation
combFilter_.setTempo(bpm_);
combFilter_.process(onsetStrength);
```

**State-of-the-art (BTrack):**
```cpp
// Bank of 41 comb filters covering 80-160 BPM
for (int i = 0; i < 41; i++) {
    float bpm = 80 + i * 2;  // 2 BPM resolution
    combFilterBank[i].process(onset);
}
// Find peak across all filters
int bestTempo = argmax(combFilterBank);
```

**Impact:** Our comb filter is entirely dependent on autocorrelation being correct first. If autocorrelation is wrong, the comb filter amplifies the wrong periodicity.

### Gap 2: Wrong Equation

**Our equation:**
```cpp
resonatorOutput_ = input + feedbackGain * delayed;
// y[n] = x[n] + α·y[n-L]
```

**Scheirer (1998) equation:**
```cpp
resonatorOutput = (1 - alpha) * input + alpha * delayed;
// y[n] = (1-α)·x[n] + α·y[n-L]
```

**Impact:** Without `(1-α)` scaling on input:
- DC bias accumulates in resonator
- Resonator output grows unbounded
- Peak detection becomes unreliable

### Gap 3: Peak Detection vs Complex Phase

**Our approach:**
```cpp
// Detect peaks in resonator output
if (resonatorOutput_ < prevResonatorOutput_ &&
    prevResonatorOutput_ > runningMean_ * 1.5f) {
    // Found peak - assume we're at phase 0
    phase_ -= phaseError * 0.3f;
}
```

**Real-Time PLP approach:**
```python
# Extract phase from complex Fourier coefficient
omega = tempo_bpm / 60.0
exponential = np.exp(-2j * np.pi * omega * t)
c = np.sum(onset_buffer * window * exponential)
phase = -np.angle(c) / (2 * np.pi)  # Direct phase extraction
```

**Impact:** Peak detection is:
- Sensitive to noise (false peaks)
- Has ~1 frame latency (detecting peak after it passes)
- Can miss peaks in noisy signals
- Requires tuning thresholds (runningMean * 1.5f)

Complex phase extraction is:
- Mathematically exact
- Zero latency
- Robust to noise (integrates over window)
- No threshold tuning needed

### Gap 4: No Tempo Transition Model

**BTrack approach:**
```cpp
// 41x41 Gaussian transition matrix
float sigma = 2.0f;  // BPM standard deviation
for (int i = 0; i < 41; i++) {
    for (int j = 0; j < 41; j++) {
        float bpmDiff = abs(i - j) * 2;  // 2 BPM per step
        transitionMatrix[i][j] = exp(-bpmDiff*bpmDiff / (2*sigma*sigma));
    }
}
// Apply during tempo tracking
newTempo = argmax(combOutput * transitionMatrix[currentTempo]);
```

**Our approach:** Multi-hypothesis tracking with LRU eviction, but no probabilistic transitions between hypotheses.

---

## Improvement Options

### Option A: Fix Existing Single Comb Filter (Minimal Change)

Fix the equation and phase extraction without major architectural changes.

**Changes:**
1. Fix equation to `y[n] = (1-α)·x[n] + α·y[n-L]`
2. Replace peak detection with complex phase extraction
3. Add proper normalization

**Estimated effort:** 2-3 hours
**Expected improvement:** Modest (still depends on autocorrelation)

### Option B: Implement Comb Filter Bank (Medium Change)

Add a bank of comb filters covering tempo range, similar to BTrack.

**Changes:**
1. Create CombFilterBank class with 20-30 filters
2. Cover range 60-180 BPM (4-6 BPM resolution)
3. Find peak energy across bank for tempo
4. Extract phase from winning filter
5. Fuse with autocorrelation tempo (agreement increases confidence)

**Estimated effort:** 4-6 hours
**Expected improvement:** Significant (independent tempo validation)

### Option C: Replace with Complex Phase Extraction (Recommended)

Replace comb filter entirely with proper complex exponential phase extraction (Real-Time PLP approach).

**Changes:**
1. Remove CombFilterPhaseTracker class
2. Implement complex phase extraction in `findBestPhase()`
3. Use existing autocorrelation tempo as input
4. Much simpler, more robust

**Implementation:**
```cpp
// Complex phase extraction (Goertzel-like)
float findPhaseAtTempo(float bpm, float frameRate) {
    float omega = bpm / 60.0f;  // Cycles per second
    float omegaNorm = omega / frameRate;  // Cycles per frame

    // Accumulate complex exponential over OSS buffer
    float realSum = 0.0f, imagSum = 0.0f;
    for (int i = 0; i < ossCount_; i++) {
        int idx = (ossWriteIdx_ - 1 - i + OSS_BUFFER_SIZE) % OSS_BUFFER_SIZE;
        float t = (float)i;
        float angle = -2.0f * M_PI * omegaNorm * t;
        realSum += ossBuffer_[idx] * cosf(angle);
        imagSum += ossBuffer_[idx] * sinf(angle);
    }

    // Phase from complex coefficient
    float phase = -atan2f(imagSum, realSum) / (2.0f * M_PI);
    if (phase < 0.0f) phase += 1.0f;
    return phase;
}
```

**Estimated effort:** 2-3 hours
**Expected improvement:** Significant (robust phase, no tuning needed)

---

## Recommended Approach

**Option C (Complex Phase Extraction)** is recommended because:

1. **We already have this partially!** The `pulsePhaseWeight` parameter enables Fourier-based phase extraction
2. The comb filter is redundant - it tries to do what Fourier phase already does
3. Simpler code = fewer bugs
4. Already proven to work (+10-11% F1 improvement)

**Action:** Remove the comb filter entirely and rely on:
- Autocorrelation for tempo
- Fourier phase extraction for phase (already implemented)
- Fusion layer for combining with transient hints

---

## If Keeping Comb Filter: Minimal Fixes

If we want to keep the comb filter as an independent validation system:

### Fix 1: Correct the Equation

```cpp
// Before:
resonatorOutput_ = input + feedbackGain * delayed;

// After:
resonatorOutput_ = (1.0f - feedbackGain) * input + feedbackGain * delayed;
```

### Fix 2: Add DC Blocking

```cpp
// After resonator, remove DC bias
static float dcEstimate = 0.0f;
dcEstimate = 0.99f * dcEstimate + 0.01f * resonatorOutput_;
resonatorOutput_ -= dcEstimate;
```

### Fix 3: Use Envelope Following Instead of Peak Detection

```cpp
// Track envelope of resonator
float envelope = 0.0f;
if (resonatorOutput_ > envelope) {
    envelope = resonatorOutput_;  // Attack
} else {
    envelope *= 0.95f;  // Decay
}

// Phase from envelope peaks (more stable than raw peaks)
if (prevEnvelope > envelope && prevEnvelope > threshold) {
    // Envelope peak detected
}
```

### Fix 4: Add Confidence Based on Resonance Strength

```cpp
// Confidence = how much stronger is resonator than input?
float gain = runningMax_ / (inputMax_ + 0.001f);
confidence_ = 1.0f - 1.0f / (1.0f + gain * 0.5f);
// Only contribute to fusion if gain > 1 (actual resonance)
if (gain < 1.0f) confidence_ = 0.0f;
```

---

## Testing Plan

After any changes, validate with:

```
# Core patterns
run_test pattern=steady-120bpm port=COM11
run_test pattern=full-mix port=COM11
run_test pattern=full-kit port=COM11
run_test pattern=synth-stabs port=COM11

# Regression checks
run_test pattern=pad-rejection port=COM11
run_test pattern=chord-rejection port=COM11
```

**Success criteria:**
- No regression on full-mix F1 (currently 0.873)
- No regression on full-kit F1 (currently 0.692)
- Comb filter contribution should be positive or neutral

---

## Decision

Based on this analysis, the recommendation is:

1. **Keep comb filter disabled** (`combFilterWeight = 0.0`)
2. **Rely on existing Fourier phase extraction** (`pulsePhaseWeight = 1.0`)
3. **Consider removing CombFilterPhaseTracker entirely** to reduce code complexity

The Fourier phase extraction we already implemented in Phase 3 provides the same benefit that a properly-implemented comb filter would provide, but with simpler, more robust code.

If independent tempo validation is needed in the future, implement a proper comb filter bank (Option B) rather than trying to fix the single-filter approach.
