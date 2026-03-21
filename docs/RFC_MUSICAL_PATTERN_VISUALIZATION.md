# RFC: Musical Pattern Visualization — Beyond Phase Tracking

**Date:** 2026-03-20
**Status:** Proposed
**Author:** jdubz + Claude

## Problem Statement

The current audio system uses a PLL (phase-locked loop) to align a sawtooth phase signal with the musical beat. On-device testing across all onset models (v1, v3, v7, v8) shows phase consistency of ~0.035-0.042 on a 0-1 scale (where 1.0 = perfectly locked, 0.0 = random). This means the PLL is effectively free-running and not locked to the beat.

### Root Cause Analysis

The PLL requires onset corrections to align phase. Two fundamental barriers prevent this:

1. **The NN onset detector cannot distinguish on-beat from off-beat transients.** A kick on beat 1 and a syncopated kick on the "and" of beat 2 produce identical activations. The PLL's `pllNearBeatWindow` only corrects when onsets land near expected beat positions, but if the phase estimate is already wrong, real onsets fall outside the correction window. Chicken-and-egg: the PLL needs to be roughly correct to get corrected.

2. **The ODF information gate was starving the PLL.** The gate (threshold 0.20) set `odf = 0.02` for weak onsets, but the PLL floor was 0.10. This has been fixed (PLL now receives raw ODF), but phase consistency only improved from 0.035 to 0.042 — confirming that the gate was a secondary issue, not the root cause.

Previous attempt with CBSS (cumulative beat strength scoring) was removed in v75 for similar reasons: CBSS accumulates score at positions with the most onsets, which in syncopated music is not necessarily the beat.

### Conclusion

Precise beat phase alignment is not achievable with acoustic onset detection alone. The system needs a different approach to creating musically responsive visual patterns.

## Proposed Architecture

Replace the PLL with three complementary signal sources operating at different timescales:

### 1. Predominant Local Pulse (PLP) — replaces PLL

**Paper:** Meier, Chiu & Muller, "A Real-Time Beat Tracking System with Zero Latency and Enhanced Confidence Signals", TISMIR 2024. Open-source: [github.com/groupmm/real_time_plp](https://github.com/groupmm/real_time_plp)

**Concept:** Instead of maintaining a free-running oscillator with onset corrections, PLP creates a smooth pulse signal by convolving the onset/flux signal with half-wave sinusoidal kernels at the detected tempo and overlap-adding them into a buffer.

**Algorithm (causal version):**
1. At each frame, the current spectral flux feeds an onset novelty function
2. The ACF (already computed every 150ms) provides the local tempo estimate
3. A half-wave rectified sinusoidal kernel is synthesized at the detected tempo
4. The kernel is centered at the current frame position in a circular buffer
5. The left half (past) matches observed data; the right half (future) extrapolates
6. All overlapping kernels accumulate via addition in the buffer
7. The buffer value at the current position is the PLP pulse output

**Why it works without phase alignment:**
- The ACF finds the dominant period regardless of which onsets are on-beat
- When many consecutive frames agree on the same tempo, their kernels reinforce at consistent positions (the natural beat phase emerges from constructive interference)
- Off-beat onsets partially cancel because their kernels don't consistently align
- The output is inherently smooth (sum of sinusoids) — no jitter, no phase discontinuities

**Output signals:**
- `plpPulse` (0-1): Smooth wave peaking at musically periodic positions. Replaces `audio.phase` as the primary visual driver for "on-beat" effects.
- `plpConfidence` (0-1): Amplitude of PLP peaks. Replaces `periodicityStrength_` for gating musical vs. organic mode. High when tempo is stable and onsets are periodic. Low during transitions/breakdowns.
- `plpLookahead` (0-1): Extrapolated future pulse value. Enables anticipatory energy effects (the "swell before the drop").

**Resource estimate:**
- Buffer: ~128 floats = 512 bytes (2-beat window at minimum BPM)
- Kernel synthesis: sine lookup table (existing or ~256 bytes)
- Per-frame cost: ~0.05ms (one sine eval + one buffer write + one buffer read)
- ACF: already computed, no additional cost

**What PLP replaces:**
- PLL free-running sawtooth + onset-gated correction (~40 lines in updatePll)
- Phase modulation in synthesizeOutputs (~30 lines)
- `pllKp`, `pllKi`, `pllOnsetFloor`, `pllNearBeatWindow`, `pllIntegralDecay`, `pllSilenceDecay` parameters (6 parameters removed)

**What PLP does NOT replace:**
- ACF tempo estimation (PLP uses this as input)
- Comb filter bank (still validates tempo)
- NN onset detection (still drives visual pulse/sparks independently)
- Pattern memory (histogram can accumulate PLP-phase-relative positions instead of PLL-phase)

### 2. Multi-Band Energy Envelopes — independent frequency channel visuals

**Concept:** Group the 26 mel bands into 3-4 frequency regions and track independent energy envelopes. Each drives a different visual parameter.

**Band grouping:**
| Group | Mel Bands | Freq Range | Musical Content |
|-------|-----------|------------|-----------------|
| Bass | 1-6 | 60-300 Hz | Kicks, bass lines |
| Low-mid | 7-13 | 300-1200 Hz | Snares, vocals |
| Mid-high | 14-20 | 1200-4000 Hz | Leads, harmonics |
| High | 21-26 | 4000-8000 Hz | Cymbals, hi-hats, air |

**Per-band signals:**
- `bandEnergy[4]`: EMA-smoothed energy per group (~4 floats, fast alpha ~0.3)
- `bandFlux[4]`: Spectral flux per group (sum of per-bin HWR within each group — computed from data already in SharedSpectralAnalysis)

**Visual mapping:**
| Band | Fast Signal (flux) | Slow Signal (energy) |
|------|-------------------|---------------------|
| Bass | Spark/pulse intensity | Flame height, brightness |
| Low-mid | Accent flashes | Color saturation |
| Mid-high | (unused or subtle) | Hue drift speed |
| High | Shimmer/sparkle overlay | (unused — prevents hi-hat visual dominance) |

This solves the hi-hat problem architecturally: hi-hat energy drives a *different visual layer* (subtle shimmer) instead of the same pulse as kicks. No model-level suppression needed.

**Resource estimate:**
- State: 8 floats (4 energy + 4 flux EMA) = 32 bytes
- Per-frame: ~0.1ms (sum mel bands within each group, already computed)

### 3. Onset Pattern Regularity (nPVI) — rhythm characterization

**Concept:** Quantify "how regular is the rhythm right now?" using the Normalized Pairwise Variability Index computed from the existing onset timestamp buffer.

**Formula:**
```
nPVI = (100 / (n-1)) * SUM(|IOI_k - IOI_{k+1}| / ((IOI_k + IOI_{k+1}) / 2))
```
where IOI_k is the k-th inter-onset interval.

**Scale:**
| nPVI Range | Musical Feel | Visual Mode |
|------------|-------------|-------------|
| 0-25 | Metronomic (four-on-the-floor techno) | Smooth, locked pulsing |
| 25-60 | Regular with variation (house, pop) | Moderate reactivity |
| 60-100 | Syncopated (funk, breakbeat) | High energy, variable |
| 100+ | Irregular (fills, transitions, free-form) | Organic/chaotic mode |

**Visual application:** nPVI directly modulates the blend between PLP-driven pulsing (low nPVI = regular music) and raw onset-driven sparks (high nPVI = irregular patterns). Replaces the current `rhythmStrength` gating which depends on periodicity strength alone.

**Resource estimate:**
- Computed from existing `onsetTimes_[64]` circular buffer
- ~16 divisions per computation, run every ~500ms
- Cost: ~0.01ms, 0 bytes additional state

### 4. Spectral Centroid (bonus, nearly free) — slow timbral evolution

**Concept:** Track the amplitude-weighted mean frequency of the spectrum. Bright sounds (cymbals, synth leads) have high centroid; dark sounds (bass, pads) have low centroid.

**Formula:** `centroid = SUM(f_k * |X_k|) / SUM(|X_k|)` over FFT bins.

**Visual application:** EMA-smoothed centroid (slow alpha ~0.01, updating ~1 Hz) drives color temperature. Low centroid (bassy section) = warm reds/oranges. High centroid (bright section) = cool blues/whites. This provides visual variation on the timescale of musical sections (8-32 bars) without any beat tracking.

**Resource estimate:** 1 float state, ~0.05ms per computation (128 bins).

## AudioControl Struct Changes

```cpp
struct AudioControl {
    // Existing (unchanged)
    float energy;            // Hybrid energy (mic + bass mel + onset peak-hold)
    float pulse;             // NN onset strength (raw transient trigger, no phase modulation)

    // Replaced
    float plpPulse;          // PLP smooth pulse (was: phase — PLL sawtooth)
    float plpConfidence;     // PLP confidence (was: rhythmStrength — periodicity + comb blend)

    // New
    float onsetRegularity;   // nPVI-based regularity (0=metronomic, 1=chaotic)
    float bassEnergy;        // Bass band (60-300 Hz) envelope
    float midEnergy;         // Mid band (300-4000 Hz) envelope
    float brightness;        // Spectral centroid, normalized (0=dark, 1=bright)

    // Removed
    // float phase;          // PLL sawtooth — replaced by plpPulse
    // float rhythmStrength; // replaced by plpConfidence
    // float onsetDensity;   // subsumed by onset regularity + band energies
    // float downbeat;       // was always 0
    // uint8_t beatInMeasure; // was always 0
};
```

Note: The exact struct layout will be determined during implementation.

### Generator Migration Table

| Generator | Old Field | Old Usage | New Field | New Usage |
|-----------|-----------|-----------|-----------|-----------|
| HeatFire | `phase` | Breathing effect (cosine of phase) | `plpPulse` | Breathing driven by PLP wave |
| HeatFire | `pulse` | Spark burst intensity | `pulse` | Unchanged (raw NN onset) |
| HeatFire | `rhythmStrength` | Blend music/organic mode | `plpConfidence` | Same role, PLP-derived |
| HeatFire | `energy` | Baseline flame height | `energy` | Unchanged |
| HeatFire | `onsetDensity` | Content classification | `onsetRegularity` | nPVI replaces density for mode selection |
| Water | `phase` | Wave phase offset | `plpPulse` | Wave driven by PLP |
| Water | `pulse` | Ripple trigger | `pulse` | Unchanged |
| Lightning | `phase` | Bolt timing | `plpPulse` | Bolt timing from PLP |
| Lightning | `pulse` | Bolt trigger | `pulse` | Unchanged |
| All | (none) | (none) | `bassEnergy` | New: bass-specific visual drive |
| All | (none) | (none) | `brightness` | New: color temperature control |

Generator migration is estimated at 60-70% of total implementation effort (Phase 4).

## Implementation Plan

### Phase 1: PLP Core (replaces PLL)
1. Implement PLP overlap-add buffer in AudioTracker
2. Feed spectral flux as the novelty function (same signal ACF uses)
3. Output `plpPulse` and `plpConfidence` to AudioControl
4. Update generators to use `plpPulse` instead of `phase`
5. Remove PLL code and associated parameters

### Phase 2: Multi-Band Energy
1. Add band grouping computation in SharedSpectralAnalysis (or AudioTracker)
2. Track per-band energy envelopes (EMA)
3. Expose `bassEnergy`, `midEnergy` on AudioControl
4. Update Fire generator to use `bassEnergy` for flame height, bass flux for sparks

### Phase 3: Onset Regularity + Spectral Centroid
1. Add nPVI computation from onset timestamp buffer
2. Add spectral centroid computation from FFT bins
3. Expose on AudioControl
4. Use nPVI to modulate PLP-vs-organic visual blend

### Phase 4: Generator Updates
1. Update HeatFire, Water, Lightning to use new AudioControl fields
2. Map band energies to distinct visual parameters per generator
3. Tune visual response curves

## Serial Stream Changes

### Current music stream format (`{"m":{...}}` at ~20 Hz)

| Field | Key | Source | PLP Impact |
|-------|-----|--------|------------|
| Rhythm active | `a` | periodicityStrength > threshold | Unchanged |
| BPM | `bpm` | ACF + comb bank | Unchanged |
| Phase | `ph` | PLL sawtooth 0→1 | **Replace with plpPulse** |
| Rhythm strength | `str` | 60% periodicity + 40% comb | **Replace with plpConfidence** |
| Periodicity | `conf` | ACF peak ratio | Unchanged |
| Beat count | `bc` | PLL phase wrap counter | **Remove** (PLP has no discrete wraps) |
| Beat event | `q` | Phase wrap >0.8→<0.2 | **Replace** (see below) |
| Energy | `e` | mic + bass mel + onset peak-hold | Unchanged |
| Pulse | `p` | NN onset strength | Unchanged |
| Onset strength | `oss` | Raw NN activation | Unchanged |
| Onset density | `od` | Onsets/sec rolling window | **Replace with nPVI** |
| Downbeat | `db` | Always 0 | Remove |
| Beat in measure | `bm` | Always 0 | Remove |

### New/changed fields

| Field | Key | Source | Purpose |
|-------|-----|--------|---------|
| PLP pulse | `ph` | PLP buffer readout | Reuse key for backward compat; semantics change from sawtooth to smooth sinusoidal wave (0-1, peaks at periodic positions) |
| PLP confidence | `str` | PLP peak amplitude | Reuse key; semantics change from periodicity blend to PLP-derived confidence |
| Beat event | `q` | PLP peak detection | 1 when plpPulse crosses threshold downward (peak just passed); replaces phase-wrap detection |
| Onset regularity | `nPVI` | nPVI computation | New field. 0=metronomic, 100+=irregular |
| Bass energy | `eBass` | Mel bands 1-6 EMA | New field. 0-1 |
| Mid energy | `eMid` | Mel bands 7-20 EMA | New field. 0-1 |
| Brightness | `bright` | Spectral centroid EMA | New field (Phase 3 stretch). 0-1 |

### Backward compatibility strategy

Reuse `ph` and `str` keys with changed semantics rather than adding new keys. Test scripts that read these fields will get PLP values instead of PLL values — which is the intent. Scripts that depend on sawtooth-specific behavior (phase wrap detection at >0.8→<0.2) need updating.

### Diagnostic commands

| Command | Current | After PLP |
|---------|---------|-----------|
| `show beat` | PLL state (phase, integral, beat count) | Alias to `show plp`; show plpPulse, plpConfidence, buffer fill |
| `show plp` | (new) | PLP buffer state, confidence, nPVI, band energies |
| `json rhythm` | BPM, phase, periodicity, combBpm | Replace phase with plpPulse, add plpConfidence, nPVI |
| `json plp` | (new) | Compact: plpPulse, plpConfidence, nPVI, eBass, eMid, brightness |
| `show bands` | (new) | Per-band energy envelopes (4 bands) |

## Testing Infrastructure Changes

### Ground truth — no changes needed

The `.beats.json` files contain beat times, strengths, and downbeat flags from 7-system consensus. These are audio-content labels, not system-dependent. The `track_manifest.json` provides seek offsets, BPM, and quality metadata. All remain valid for PLP testing.

The `kick_weighted/*.kick_weighted.json` files provide per-instrument onset labels. These are also audio-content labels and remain valid.

### Test script impact

| Script | Stream Fields Used | PLP Impact | Required Changes |
|--------|-------------------|------------|-----------------|
| `ab_test_multidev.cjs` | `m.bpm` only | None | None (BPM path unchanged) |
| `model_compare_multidev.cjs` | `m.bpm` only | None | None |
| `param_sweep_multidev.cjs` | `m.bpm`, `m.ph`, `m.p`, `m.str`, `m.e` | Phase alignment metric changes | Update phase alignment formula (PLP peaks near 1.0, not sawtooth wrap) |
| `phase_downbeat_eval.cjs` | `m.ph`, `m.bpm`, `m.str`, `m.db`, `m.q` | Major: phase error and beat event semantics change | Rewrite beat detection (PLP peak detection replaces phase wrap); disable downbeat metrics |

### New test metrics

**1. PLP pulse periodicity** (replaces phase consistency)

Autocorrelate the streamed `ph` (plpPulse) signal at the detected BPM lag. The ACF peak height measures how periodic the PLP output is. Target: > 0.5 for rhythmic music. This is the primary success metric — it directly measures whether the visualization pulses in time with the music's dominant rhythm.

```
plpPeriodicity = ACF(plpPulse, lag=framerate*60/bpm) / ACF(plpPulse, lag=0)
```

**2. Visual onset accuracy** (unchanged)

Do `audio.pulse` spikes coincide with audible kick/snare transients? Measured via kick-weighted onset F1 (existing metric, unchanged by PLP).

**3. Band energy responsiveness** (new)

Cross-correlate streamed `eBass` with ground-truth bass amplitude envelope (extracted offline from test tracks via bandpass filter + RMS). Peak correlation and lag measure responsiveness. Target: peak correlation > 0.7, lag < 50ms.

**4. nPVI accuracy** (new, calibration only)

Compare streamed `nPVI` values across the 18 EDM test tracks. Four-on-the-floor tracks (techno-minimal-01, techno-deep-ambience) should show low nPVI (<30). Breakbeat/syncopated tracks (breakbeat-drive, garage-uk-2step) should show high nPVI (>60). This calibrates the nPVI thresholds for electronic music.

### Settle time

Current: 12 seconds (OSS buffer 5.5s + ACF convergence 3-5s + margin). PLP convergence depends on the overlap-add buffer filling with enough periodic kernels — likely similar to ACF convergence (a few beat periods). Start with 12 seconds and adjust if PLP converges faster.

## Parameters

### Removed (12 PLL/phase parameters)

| Serial Name | Parameter | Reason |
|-------------|-----------|--------|
| `pllkp` | PLL proportional gain | PLL removed |
| `pllki` | PLL integral gain | PLL removed |
| `pllonsetfloor` | PLL onset floor | PLL removed |
| `pllnearbeat` | PLL correction window | PLL removed |
| `pllintdecay` | PLL integral decay | PLL removed |
| `pllsildecay` | PLL silence decay | PLL removed |
| `pulseboost` | On-beat pulse boost | Phase modulation removed |
| `conffloor` | Off-beat confidence floor | Phase modulation removed |
| `energyboost` | On-beat energy boost | Phase modulation removed |
| `confactivation` | Rhythm activation threshold | Replaced by plpConfidence |
| `conffullmod` | Full modulation threshold | Replaced by plpConfidence |
| `subdivtol` | Subdivision tolerance | Phase modulation removed |
| `odfgate` | ODF information gate | Already deprecated (v76) |

### New (estimated 8-12 PLP/band/nPVI parameters)

| Serial Name | Parameter | Default | Range | Purpose | Needs Sweep? |
|-------------|-----------|---------|-------|---------|-------------|
| `plpbufsize` | PLP buffer frames | 256 | 128-512 | Overlap-add buffer length | No (set once based on BPM range) |
| `plpnovgain` | Novelty scaling | 1.0 | 0.1-5.0 | Spectral flux gain into PLP kernels | Yes |
| `plpconfalpha` | Confidence EMA | 0.2 | 0.05-0.5 | Smoothing for plpConfidence | Maybe |
| `plpactivation` | Music mode threshold | 0.3 | 0.0-1.0 | plpConfidence below this → organic mode | Yes |
| `bassalpha` | Bass energy EMA | 0.3 | 0.1-0.5 | Smoothing for 60-300 Hz band | Maybe |
| `midalpha` | Mid energy EMA | 0.3 | 0.1-0.5 | Smoothing for 300-4k Hz band | Maybe |
| `npvirate` | nPVI update interval | 500 | 250-1000 | Milliseconds between nPVI updates | No |
| `npvimin` | nPVI min intervals | 4 | 2-16 | Minimum IOIs for stable estimate | No |
| `centroidalpha` | Brightness EMA | 0.01 | 0.001-0.1 | Slow timbral evolution smoothing | No |

Parameters marked "Needs Sweep" should be tested with `param_sweep_multidev.cjs` using the PLP periodicity metric as the optimization target.

## Success Metrics

Instead of phase consistency (which measures lock to beat grid — something we've shown is unachievable), measure:

1. **PLP pulse periodicity:** autocorrelation of the PLP output at the detected BPM lag. Should be > 0.5 for rhythmic music (vs. current phase consistency of 0.04).
2. **Visual onset accuracy:** do `audio.pulse` spikes coincide with audible kick/snare transients? Measured via the existing kick-weighted onset F1 metric.
3. **Band energy responsiveness:** do bass/mid/high envelopes track their respective frequency content with < 50ms latency? Measured by cross-correlating band energy with ground-truth amplitude envelopes.
4. **Subjective visual quality:** does the visualization look musical? Evaluated by watching the devices with music. This is ultimately what matters.

## Open Questions

1. **PLP buffer size:** The Meier 2024 paper uses a 4-8 second window. At 62.5 Hz that's 250-500 frames. Minimum for 60 BPM (slowest tempo): 2 beat periods = 2 seconds = 125 frames = 500 bytes. Maximum for full stability: 500 frames = 2 KB. Start with 256 frames (1 KB) and tune. RAM budget: current arena 3404/32768 bytes, plus ~13 KB globals. 1-2 KB for PLP buffer is feasible.
2. **Spectral flux vs. NN onset as PLP input:** PLP can use either. Spectral flux is NN-independent and broadband. NN onset is instrument-aware (especially with v8's 3-channel output). May want to try both. Start with spectral flux (same signal ACF already uses) to avoid adding an NN dependency to the phase path.
3. **PLP lookahead:** The extrapolated right half of the kernel provides ~1 beat period of lookahead. At 120 BPM that's 500ms. At 200 BPM that's 300ms. This is a stretch goal for Phase 1 — the core pulse output works without it. Anticipatory effects can be added later.
4. **nPVI stability:** nPVI requires at least 4-8 inter-onset intervals for a stable estimate. At 2-4 onsets/second (typical for EDM), that's 2-4 seconds of onset history. The existing 64-slot onset timestamp buffer covers ~16-32 seconds at typical density — more than sufficient. nPVI should be updated every ~500ms (not every frame) to smooth the output.
5. **nPVI threshold calibration:** The proposed ranges (0-25=metronomic, 25-60=regular, etc.) are from speech prosody literature. Electronic music may cluster differently. Treat these as starting points and calibrate with on-device testing across the 18 EDM test tracks.
6. **Settings version bump:** New AudioControl layout requires SETTINGS_VERSION increment (v75 → v76) and factory reset on all 7 devices. The `odfGateThreshold` parameter should be fully removed at that time.
7. **Stream format versioning:** Reusing `ph` and `str` keys with changed semantics will silently break consumers (blinky-console, test scripts). Options: (a) add a `"v":2` field to the stream JSON so consumers can detect the format, (b) use new key names with a deprecation period, or (c) accept the break since all consumers are internal and updated in the same PR. Recommend (a) — minimal cost, prevents silent breakage.

## References

- Grosche & Muller, "Extracting Predominant Local Pulse Information from Music Recordings", IEEE TASLP 2011 (original PLP algorithm)
- Meier, Chiu & Muller, "A Real-Time Beat Tracking System with Zero Latency and Enhanced Confidence Signals", TISMIR 2024 (causal real-time PLP with confidence signals; open-source: github.com/groupmm/real_time_plp)
- Patel & Daniele, "An empirical comparison of rhythm in language and music", Cognition 2003 (nPVI for rhythm regularity)
- Scheirer, "Tempo and Beat Analysis of Acoustic Musical Signals", JASA 1998 (multi-band envelope model)
- Thul & Toussaint, "A Comparative Evaluation of Rhythm Complexity Measures", ISMIR 2008 (comparison of 32 complexity measures including nPVI)
