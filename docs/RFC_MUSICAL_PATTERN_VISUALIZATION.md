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

Previous attempt with CBSS (cumulative beat strength scoring — a beat tracking system that accumulated score at positions with the most onsets) was removed in v75 for similar reasons: in syncopated music, the positions with the most onsets are not necessarily the beats.

### Conclusion

Precise beat phase alignment is not achievable with acoustic onset detection alone. The system needs a different approach to creating musically responsive visual patterns.

## Proposed Architecture

Replace the PLL with three complementary signal sources operating at different timescales:

### 1. Predominant Local Pulse (PLP) — replaces PLL

**Paper:** Meier, Chiu & Muller, "A Real-Time Beat Tracking System with Zero Latency and Enhanced Confidence Signals", TISMIR 2024. Open-source: [github.com/groupmm/real_time_plp](https://github.com/groupmm/real_time_plp)

**Concept:** Instead of maintaining a free-running oscillator with onset corrections, PLP finds the dominant repeating energy pattern in the audio signal. It uses dual-source input (spectral flux AND band energy envelopes), detects the dominant period via autocorrelation, and extracts the actual repeating pattern at that period — not a synthesized sinusoidal approximation.

**Dual-source input:**
- **Spectral flux** (HWR): Captures transient change spikes — sharp onsets, attacks. Same signal ACF already uses.
- **Band energy envelopes** (bass/mid): Captures the raw energy envelope — bass pumping, sustained swells. Tracked separately from spectral flux.

When both sources agree on the dominant period, this provides higher confidence in the detected rhythm. Each source is autocorrelated independently; period agreement is checked before extracting the pattern.

**Algorithm (causal version):**
1. At each frame, spectral flux and band energy envelopes are computed (dual-source)
2. Each source is autocorrelated independently to find its dominant period
3. If both sources agree on the dominant period (within tolerance), confidence is high
4. The actual repeating energy pattern at the detected period is extracted from the circular buffer — this is the real kick-pattern curve (sharp attack, fast decay, silence, repeat), not a synthesized sinusoid
5. The extracted pattern is normalized and phase-aligned to produce the PLP pulse output
6. The pattern is also extrapolated forward by repeating the extracted waveform for lookahead

**Why it works without phase alignment:**
- The ACF finds the dominant period regardless of which onsets are on-beat
- Dual-source agreement (spectral flux + band energy) provides robust period detection — spectral flux catches transients, band energy catches sustained rhythmic patterns (bass pumping)
- The output is the actual energy pattern, which is more visually interesting than a smooth sine approximation — real kick patterns have sharp attacks and fast decays
- BPM accuracy and octave errors don't matter: half/double time still produces a valid repeating pattern. PLP finds whatever period dominates regardless.

**Output signals:**
- `plpPulse` (0-1): The extracted dominant energy pattern, normalized. Peaks at rhythmically periodic positions with the actual waveshape of the audio (sharp kick attacks, fast decays — not a smooth sinusoid). Replaces `audio.phase` as the primary visual driver for "on-beat" effects.
- `plpConfidence` (0-1): Degree of agreement between spectral flux and band energy period estimates. Replaces `periodicityStrength_` for gating musical vs. organic mode. High when both sources agree on the same period. Low during transitions/breakdowns or when no periodic pattern is present.
- `plpLookahead` (0-1): Extrapolated future pulse value (repeating the extracted pattern forward). Enables anticipatory energy effects (the "swell before the drop").

**Resource estimate:**
- Circular buffers: 2 source buffers (spectral flux + band energy) x ~128 floats = 1024 bytes
- Extracted pattern: ~64 floats = 256 bytes (one period of the dominant pattern)
- Per-frame cost: ~0.05ms (buffer write + pattern readout)
- ACF: already computed for spectral flux; band energy ACF adds ~0.02ms

**What PLP replaces:**
- PLL free-running sawtooth + onset-gated correction (~40 lines in updatePll)
- Phase modulation in synthesizeOutputs (~30 lines)
- `pllKp`, `pllKi`, `pllOnsetFloor`, `pllNearBeatWindow`, `pllIntegralDecay`, `pllSilenceDecay` parameters (6 parameters removed)
- Dependence on NN onset accuracy for the phase/pulse path — PLP uses spectral flux and band energies, both of which are reliable audio-domain signals (unlike NN onsets at ~60% accuracy / F1=0.681)

**What PLP does NOT replace:**
- ACF tempo estimation (PLP uses the detected period, though octave errors are non-issues — half/double time still produces valid repeating patterns)
- Comb filter bank (still validates tempo)
- NN onset detection (still drives visual pulse/sparks independently — raw transient trigger)
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
    float plpPulse;          // PLP dominant pattern (was: phase — PLL sawtooth). Actual waveshape, not sinusoidal.
    float plpConfidence;     // PLP dual-source agreement (was: rhythmStrength — periodicity + comb blend)

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
1. Implement dual-source PLP in AudioTracker: circular buffers for spectral flux and band energy envelopes
2. Independent autocorrelation on each source to find dominant period
3. Extract the actual repeating pattern at the dominant period (not sinusoidal synthesis)
4. Compute `plpConfidence` from dual-source period agreement
5. Output `plpPulse` and `plpConfidence` to AudioControl
6. Update generators to use `plpPulse` instead of `phase`
7. Remove PLL code and associated parameters

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
| PLP pulse | `ph` | PLP pattern readout | Reuse key for backward compat; semantics change from sawtooth to extracted rhythmic pattern (0-1, peaks at periodic positions, actual waveshape not sinusoidal) |
| PLP confidence | `str` | Dual-source period agreement | Reuse key; semantics change from periodicity blend to spectral flux / band energy period agreement |
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

Current: 12 seconds (OSS buffer 5.5s + ACF convergence 3-5s + margin). PLP convergence depends on the circular buffers accumulating enough data for reliable autocorrelation and pattern extraction — likely similar to ACF convergence (a few beat periods). Start with 12 seconds and adjust if PLP converges faster.

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
| `plpbufsize` | PLP buffer frames | 128 | 64-256 | Circular buffer length per source (spectral flux + band energy) | No (set once based on BPM range) |
| `plpnovgain` | Novelty scaling | 1.0 | 0.1-5.0 | Spectral flux gain into PLP buffer | Yes |
| `plpconfalpha` | Confidence EMA | 0.2 | 0.05-0.5 | Smoothing for plpConfidence (dual-source agreement) | Maybe |
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

1. **PLP buffer size:** Pattern extraction needs enough history to autocorrelate and extract at least 2 full periods of the dominant pattern. Minimum for 60 BPM (slowest tempo): 2 beat periods = 2 seconds = 125 frames. Two source buffers (spectral flux + band energy) x 128 floats = 1024 bytes, plus ~256 bytes for the extracted pattern. Start with 128 frames per source and tune. RAM budget: current arena 3404/32768 bytes, plus ~13 KB globals. ~1.3 KB for PLP buffers is feasible.
2. **~~Spectral flux vs. NN onset as PLP input~~** RESOLVED: Use both spectral flux AND band energy envelopes as dual-source input — NOT NN onset. NN onsets are only ~60% accurate (F1=0.681 for v1 model) and cannot distinguish on-beat from off-beat. Spectral flux captures transient change spikes; band energies capture raw energy envelopes (bass pumping, etc.). Both are reliable audio-domain signals. Each source is autocorrelated independently; when both agree on the dominant period, confidence is high.
3. **PLP lookahead:** The extracted pattern can be repeated forward to provide ~1 beat period of lookahead. At 120 BPM that's 500ms. At 200 BPM that's 300ms. This is a stretch goal for Phase 1 — the core pulse output works without it. Anticipatory effects can be added later.
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
