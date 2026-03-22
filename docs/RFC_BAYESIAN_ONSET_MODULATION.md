# RFC: Pattern-Aware Onset Prediction (Bayesian Onset Modulation)

**Status:** Proposed (not implemented)
**Date:** March 19, 2026
**Prerequisites:** v9 kick-weighted model deployed, pattern param sweep validated

## Context

The pattern memory system (v77-78) works but is purely reactive — it can only act on patterns after they've repeated for 8+ bars (~15-20 seconds). The bar histogram fills, confidence rises, and predictions begin, but the audience has already been listening without visual anticipation.

The v9 onset model learns to suppress hi-hats at the label level. Combined with the histogram's `histogramMinStrength` filter, this produces peaked histograms faster. But the system still only uses pattern knowledge for a small post-detection pulse boost (`patternGain`).

**The opportunity:** Use confident pattern predictions to modulate NN onset detection *before* pulse threshold comparison — not to override the NN, but to sharpen its output. Boost predicted kicks, suppress unpredicted noise.

## Why This Is Different From What Failed Before

Previous multi-hypothesis systems (v38-v57, removed in v64) failed for specific reasons that don't apply here:

| Previous System | Why It Failed | Why This Is Different |
|---|---|---|
| Forward filter (v57) | Drove BPM, severe half-time bias (17/18 octave errors) | Does NOT drive BPM |
| Particle filter (v38) | 100 particles, never outperformed single CBSS | Single histogram, not particles |
| Multi-agent (v48) | 8 competing hypotheses, -4% vs baseline | One hypothesis, confidence-gated |
| Joint HMM (v53) | 900 states, argmax jumps between tempo bins | No tempo states, bar-position only |

**Key architectural difference:** Those systems tried to solve *tempo ambiguity* (octave errors, gravity well). This system solves *onset confidence* — a simpler, more constrained problem. BPM comes from spectral flux (NN-independent, unchanged). Pattern memory is purely advisory.

**NN quality improvement:** Previous systems failed partly because onset quality was poor. The v3 model achieves onset F1=0.718, and v9 (kick-weighted) should improve further by suppressing hi-hat false positives.

## Design: Bayesian Onset Modulation

### Core Idea

Treat the NN output as a **likelihood** and the pattern histogram as a **prior**:

```
modulated_odf = odf × (1 + prior × boostGain)      // when pattern predicts onset
modulated_odf = odf × (1 - confidence × suppressGain) // when pattern predicts silence
```

Currently `patternGain` boosts pulse AFTER detection (AudioTracker.cpp:989). The key change: apply modulation BEFORE pulse detection, so the threshold comparison benefits from pattern knowledge.

### Signal Flow

```
FrameOnsetNN → odf (raw NN output, 0-1)
                 ↓
           [PATTERN MODULATION] ← predictOnsetStrength(nowMs)
           (only when patternConfidence > 0.3 AND ioiConfidence > 0.5)
                 ↓
              modulated_odf (clamped to 0-1)
                 ↓
           updatePulseDetection(modulated_odf)
                 ↓
           pulse fires when: modulated_odf > baseline × threshold
                             AND mic.getLevel() > minLevel
                             AND cooldown elapsed
                 ↓
           recordOnsetForPattern(pulseStrength)
                 ↓
           bar histogram ← accumulates pulse decisions
                 ↓
           [feedback loop: histogram → prediction → modulation]
```

### Feedback Loop Safety

This IS a feedback loop. Bounded by 4 mechanisms:

1. **Multiplicative modulation:** `0 × anything = 0`. Zero NN output cannot be boosted into a false onset.
2. **Mic level gate:** `mic_.getLevel() > pulseMinLevel` check is evaluated BEFORE modulation. No sound = no onset, regardless of pattern prediction.
3. **Baseline tracker adaptation:** If false onsets somehow occur, they raise the floor-tracking baseline, increasing the threshold for subsequent onsets.
4. **Histogram decay:** Stale patterns fade naturally with 22-second half-life. No permanent lock-in.

### What This Enables

1. **Consistent kick detection.** A kick the NN scores at 0.4 (below threshold) but the pattern expects gets boosted to 0.6 (above threshold). Visual: consistent flame spikes on every kick, not just the loud ones.

2. **Hi-hat suppression.** A hi-hat the NN scores at 0.35 (near threshold) at an unpredicted position gets attenuated to 0.25 (below threshold). Visual: cleaner flame without random flickers.

3. **Self-reinforcing accuracy.** Cleaner onsets → cleaner histogram → better predictions → cleaner onsets. Bounded by the safety mechanisms above.

## Risks and Accuracy Goals

### Risks

| Risk | Severity | Mitigation | Detection |
|------|----------|------------|-----------|
| Phantom onsets in silence | High | Mic level gate fires before modulation | A/B: play track with quiet intro, count false pulses |
| Pattern lock-in (wrong pattern persists) | Medium | 22s histogram decay, cache restore separate | Monitor confidence during section changes (must drop < 0.3 within 5 bars) |
| Suppressing real onsets at unexpected positions | Medium | Suppress only when confidence > 0.5, conservative gain (0.3) | Fill tolerance test: min pc ≥ 0.2 during fills |
| Runaway positive feedback | Low | Multiplicative (zero stays zero), baseline adaptation | Onset density must stay 1-4/bar with no upward drift over 60s |

### Accuracy Goals (must-hit before deploying)

| Metric | Threshold | Rationale |
|--------|-----------|-----------|
| No false onsets in silence | 0 false pulses in 10s gap | #1 visual artifact per VISUALIZER_GOALS.md |
| Onset density stable | 1-4 events/bar, no drift over 60s | Runaway feedback indicator |
| Fill tolerance preserved | min pc ≥ 0.2 during fills | Suppression must not kill real onsets |
| Cold start unaffected | Identical onset behavior first 8 bars with/without modulation | patternConfidence=0 during cold start → modulation is no-op |
| Section change recovery | Confidence < 0.3 within 5 bars of change | Old pattern must not suppress new section |
| A/B visual quality | Blind preference: modulated ≥ unmodulated on 6/8 tracks | Must look better, not just measure better |

### Prerequisites

1. **v9 model deployed and evaluated.** Must produce peaked histograms on hi-hat-heavy tracks (techno-minimal-emotion). If the model can't distinguish kicks from hi-hats, modulation amplifies the wrong signals.
2. **Pattern param sweep analyzed.** `confidenceRise`/`confidenceDecay`/`histogramMinStrength` values settled before adding another layer.
3. **Baseline metrics established.** Full 8-track pattern memory test suite with v9 model + optimized params. All 6 metrics documented.

## Implementation Scope

| File | Change |
|------|--------|
| `AudioTracker.h` | Add `patternModGain` (default 0.5), `patternSuppressGain` (default 0.3) |
| `AudioTracker.cpp` | Pre-pulse modulation in `poll()`, remove post-pulse `patternGain` boost |
| `ConfigStorage.h/cpp` | Persist new params, bump SETTINGS_VERSION |
| `SerialConsole.cpp` | Register `patmodgain`, `patsuppress` in pattern category |

Estimated: ~30 lines of logic, ~50 lines of plumbing (config/registry).

## Non-Goals

- **Not driving BPM.** Tempo estimation is NN-independent (spectral flux ACF). Pattern memory does not influence it.
- **Not implementing multi-hypothesis.** One histogram, one confidence score, gated activation. The removed multi-agent/HMM/particle approaches solved a different problem (tempo ambiguity).
- **Not making the NN model itself pattern-aware.** The Conv1D W16 model has a 144ms receptive field — too narrow for bar context. Bar-phase conditioning (feeding phase as a model input) would require wider context or recurrence. Separate future work.

## Related Documents

- `docs/PATTERN_HISTOGRAM_DESIGN.md` — Current v77-78 pattern memory architecture
- `docs/VISUALIZER_GOALS.md` — Design philosophy (visual quality over metrics)
- `docs/AUDIO_ARCHITECTURE.md` — Full audio pipeline and AudioTracker design
