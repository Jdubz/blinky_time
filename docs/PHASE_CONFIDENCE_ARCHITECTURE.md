# Phase-Aware Onset Confidence Modulation (v76 Proposal)

*Drafted: March 18, 2026*

## Problem Statement

The NN onset detector and DSP phase tracker have a circular reliability problem:

1. **NN detects onsets** (kicks/snares) but cannot distinguish on-beat from off-beat transients (144ms receptive field, zero metrical awareness)
2. **DSP estimates BPM** via spectral flux ACF + comb bank, but has a ~135 BPM gravity well and the PLL is architecturally novel with no literature precedent
3. **PLL uses onset-gated correction**, but off-beat onsets actively corrupt the phase estimate
4. **Neither system is reliable enough to bootstrap the other**

The current pulse modulation is a crude two-threshold system: within 0.2 of beat = boost 1.3x, beyond 0.3 = suppress 0.6x, dead zone in between. It does not consider beat subdivisions and treats all onsets as either "on-beat" or "off-beat."

## Core Insight

**Don't correct onset timing. Modulate onset confidence.**

Onsets arrive when they arrive — you cannot move them forward in time, and they're likely already late due to acoustic propagation. Instead: when an onset fires, ask "how much visual weight should this get?" based on:
- Where the onset falls on the beat grid (quarter note, 8th, 16th, off-grid)
- How confident the BPM/phase estimate is (rhythmStrength)

When confidence is low, all onsets pass through at full strength (energy-driven organic mode).
When confidence is high, on-beat onsets get boosted and off-beat onsets get attenuated.

## Key Insight: Octave Errors Don't Matter

**Half-time and double-time BPM both look fine visually.** A pulse at 60 BPM (half of 120) still
aligns with the beat grid — it fires every other beat, which looks intentional. A pulse at 240 BPM
fires on every 8th note, which is still rhythmically locked. The confusion between a 1/4 note and
an 1/8 note is not a visual problem.

**What matters is phase alignment with the beat grid.** Events landing on ANY subdivision (1/4, 1/8,
1/16) of the actual tempo look musical. Events landing between subdivisions look random and break
immersion. The 135 BPM gravity well is therefore LOW priority — it causes octave errors, not phase
errors. Phase alignment is the primary bottleneck.

## Reliability Assessment

| Signal | Reliable For | Unreliable For |
|--------|-------------|----------------|
| **NN onset timing** | Detection: "a transient happened" | Classification: "was it on a beat?" |
| **DSP phase (ACF+PLL)** | Stability: smooth free-running sawtooth | Convergence: correct BPM octave (but octave errors are acceptable) |
| **rhythmStrength** | Distinguishing music from silence/ambient | Fine-grained genre classification |
| **onsetDensity** | Sparse vs busy content discrimination | Precise genre classification |

**Conclusion:** DSP phase should be advisory, not authoritative. The default is "onset fires, visual reacts." Phase confidence adds musical intelligence on top — it modulates, never gates. BPM octave accuracy is low priority — phase grid alignment is what determines visual quality.

## Architecture

### Component Responsibilities (Clarified)

| Component | Role | Input | Output |
|-----------|------|-------|--------|
| **FrameOnsetNN** | Detect acoustic transients | Mel frames | Onset activation (0-1) |
| **Spectral flux ACF** | Estimate tempo | Raw FFT magnitudes | BPM estimate |
| **CombFilterBank** | Validate tempo | Spectral flux | Independent BPM confirmation |
| **PLL** | Track phase | BPM + onset events | Phase angle (0-1 sawtooth) |
| **Confidence modulator** | Scale onset visual impact | Onset + phase + rhythmStrength | Weighted pulse + grid class |

### Data Flow

```
PDM Mic → AdaptiveMic → SharedSpectralAnalysis
                            │
                ┌───────────┼───────────┐
                ↓           ↓           ↓
          Mel Bands    Spectral Flux   Magnitudes
                │           │
                ↓           ↓
         FrameOnsetNN   ACF + Comb → BPM estimate
                │                       │
                ↓                       ↓
         Onset activation          PLL phase ramp
                │                       │
                └──────────┬────────────┘
                           ↓
                  Confidence Modulator
                  ├── subdivisionPhase = fmod(phase * subdivLevel, 1.0)
                  ├── gridClass = classifyOnset(phase, tolerance)
                  ├── confMult = f(gridDistance, rhythmStrength)
                  └── pulse = rawPulse * confMult
                           │
                           ↓
              AudioControl {energy, pulse, phase,
                           rhythmStrength, onsetDensity,
                           onsetGridClass, onsetPhaseConfidence}
                           │
                           ↓
                    Generator → Effect → LEDs
```

### Onset Grid Classification

```cpp
enum OnsetGridClass : uint8_t {
    ON_BEAT     = 0,  // Within tolerance of quarter note
    EIGHTH_NOTE = 1,  // Within tolerance of 8th note
    SIXTEENTH   = 2,  // Within tolerance of 16th note
    OFF_GRID    = 3   // Not near any subdivision
};
```

Classify where the onset falls relative to the current phase:
- Phase near 0.0 or 1.0 → ON_BEAT
- Phase near 0.5 → EIGHTH_NOTE
- Phase near 0.25 or 0.75 → SIXTEENTH
- Otherwise → OFF_GRID

Tolerance parameter (`subdivTolerance`, default 0.10) defines "near." At 120 BPM, 0.10 = 50ms window.

### Phase Confidence Multiplier

Replaces the current binary boost/suppress with a continuous, smooth multiplier:

1. **Cosine proximity curve**: 1.0 at grid center, decays smoothly to `confFloor` at maximum distance
2. **rhythmStrength gate**: blends between passthrough (no modulation) and full modulation
   - Below `confActivation` (0.3): all onsets pass through unmodulated
   - Above `confFullModulation` (0.7): full phase-based modulation
   - Between: linear interpolation
3. **On-beat boost**: when confident AND on-grid, multiply by `pulseBoostOnBeat` (1.3x)

Key properties:
- **No hard edges**: cosine taper instead of step thresholds
- **Graceful degradation**: low rhythmStrength → all onsets pass through
- **Off-beat onsets still visible**: `confFloor` (0.4) ensures off-beat transients still produce 40% visual response
- **No mode switching**: continuous blend, never jarring transitions

### PLL Density-Scaled Correction

The PLL onset-gated correction should consider onset density:

- **Low density** (< 1.5 onsets/sec): sparse content. Onsets are more likely meaningful (isolated kicks). Trust them more for phase correction → scale correction up 1.3x.
- **High density** (> 4.0 onsets/sec): busy/syncopated content. Onsets may be off-beat. Trust them less → scale correction down 0.5x.
- **Medium density**: no scaling change.

This prevents syncopated 16th-note hi-hats from corrupting PLL phase while still allowing isolated kick drums to correct it.

### Graceful Degradation Cascade

No explicit mode switching. The confidence multiplier and rhythmStrength blend create a natural cascade:

| Condition | Visual Behavior |
|-----------|----------------|
| `rhythmStrength > 0.7` + onset ON_BEAT | Full beat-sync: phase-locked pulsing, boosted sparks |
| `rhythmStrength > 0.7` + onset OFF_GRID | Reduced pulse (40% of raw), phase breathing continues |
| `rhythmStrength 0.3-0.7` | Blended: partial phase modulation + raw energy |
| `rhythmStrength < 0.3` | Organic: raw energy-driven, no phase modulation |
| Silence (>3s) | Minimal organic drift |

## New Parameters (v76)

| Parameter | Serial Name | Default | Range | Purpose |
|-----------|-------------|---------|-------|---------|
| confFloor | `conffloor` | 0.4 | 0.0-1.0 | Minimum confidence for off-grid onsets |
| confActivation | `confactivation` | 0.3 | 0.0-1.0 | rhythmStrength below this: no modulation |
| confFullModulation | `conffullmod` | 0.7 | 0.3-1.0 | rhythmStrength above this: full modulation |
| subdivTolerance | `subdivtol` | 0.10 | 0.02-0.20 | Phase distance for "near subdivision" |
| subdivLevel | `subdivlevel` | 2 | 1-4 | Max subdivision (1=quarter, 2=8th, 4=16th) |
| pllDensityThreshLow | `plldenslow` | 1.5 | 0.5-3.0 | Below: trust onsets more for PLL |
| pllDensityThreshHigh | `plldenshigh` | 4.0 | 2.0-8.0 | Above: trust onsets less for PLL |
| pllDensityScaleLow | `plldscalelow` | 1.3 | 1.0-2.0 | PLL correction boost for sparse content |
| pllDensityScaleHigh | `plldscalehigh` | 0.5 | 0.1-1.0 | PLL correction reduction for busy content |

**Replaced:** `pulseNearBeatThreshold` (0.2) and `pulseFarFromBeatThreshold` (0.3) — superseded by continuous subdivision-aware modulation.

## Future: Phase-Aware NN Training

**NOT for v76.** Requires reliable phase tracking first.

**Concept:** Feed phase context as additional NN input features:
- `sin(2*pi*phase)`, `cos(2*pi*phase)` — circular encoding (avoids 0/1 discontinuity)
- `bpmNormalized` — BPM mapped to 0-1
- `rhythmStrength` — DSP confidence

Changes NN input from 26 mel bands to 30 features per frame.

**Training with noisy phase** is critical:
- Augment with random BPM offset (5-20%), phase drift, and zero-out (p=0.2)
- Model learns to USE phase when it agrees with acoustic evidence, IGNORE it when it doesn't
- The model must still function as pure onset detector when phase features are zero

**Prerequisite:** v76 confidence modulation must be validated AND the literature A/B tests must settle the 135 BPM gravity well before phase quality is good enough for training data.

## Implementation Plan

### Phase 1: Core confidence modulation
- Modify `AudioTracker::synthesizeOutputs()` — replace two-threshold pulse logic
- Add `computeSubdivisionPhase()`, `classifyOnset()`, `computePhaseConfidence()`
- Add `onsetGridClass` and `onsetPhaseConfidence` to AudioControl
- ~60 lines added, ~15 modified. No new files. Negligible RAM/CPU.

### Phase 2: PLL density scaling
- Modify `AudioTracker::updatePll()` — scale correction by onset density
- ~15 lines added.

### Phase 3: ConfigStorage + SerialConsole
- Add 9 new params to StoredTrackerParams, increment SETTINGS_VERSION 74→75
- Register in SerialConsole, add validation in ConfigStorage

### Phase 4: A/B testing
- Test against v75 on blinkyhost (3 devices, 18 EDM tracks)
- Sweep `confFloor` (0.2, 0.4, 0.6) and `subdivLevel` (1, 2)
- Visual assessment on actual LED devices

## Architectural Principles

1. **Onset detector: detect transients. Nothing more.** The NN answers "did something percussive happen?" not "is this a beat?"
2. **Phase tracker: estimate metrical position. Advisory only.** It provides context for interpreting onsets, not authority over them.
3. **Integration is modulation, not correction.** Phase modulates onset visual weight. It never moves onset timing or suppresses onsets entirely.
4. **Confidence cascades, never switches.** No mode transitions. Continuous blend through rhythmStrength.
5. **Default is raw energy.** Without reliable BPM, onsets drive visuals directly. This is acceptable for all content types.
6. **New fields are optional.** Generators that ignore `onsetGridClass` see only the pre-modulated pulse strength — no code changes required.
