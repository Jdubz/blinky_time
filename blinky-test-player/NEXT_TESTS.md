# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 2, 2026

## Current Config (SETTINGS_VERSION 40)

**Detector:** BandWeightedFlux Solo (all others disabled)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate)

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.3 (v25 inverse-lag normalized), bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, cbssTightness=8.0 (v40, raised from 5.0), beatoffset=5, onsetSnapWindow=8
- densityoctave=1, octavecheck=1 (v32 octave disambiguation)
- odfmeansub=0 (v32 — raw ODF preserves ACF structure)
- phasecheck=0, warmup=0, cbssContrast=1.0

**Particle filter** (disabled by default, A/B testable via `set particlefilter 1`)
- Hybrid mode: PF provides tempo estimate, CBSS handles beat detection and phase
- 100 particles, stratified resampling with octave injection (T/2, 2T)
- v39: madmom obs model, info gate, phase-coherent octave, PF+CBSS hybrid
- **Smoke test result:** Dramatically improves BPM accuracy (+52%) but no Beat F1 improvement

## Current Baselines

### v40 18-Track Validation (Mar 2, 2026)

| Metric | 4-Device Avg | Best-Device Avg |
|--------|:------------:|:---------------:|
| Beat F1 | 0.285 | 0.348 |

### Historical Baselines

| Version | Change | 4-Dev F1 | Best-Dev F1 |
|---------|--------|:--------:|:-----------:|
| v36 | Frame rate fix, onset snap, downward correction | 0.275 | 0.351 |
| v40 | cbssTightness 5→8 | 0.285 | 0.348 |

### Known Bottlenecks

1. **Double-time lock (~182 BPM)** on 3/4 devices — primary failure mode.
   ACF + comb provide only ~3.3x octave discrimination. v32 features help but don't fully solve it.
   ACM0 consistently avoids this (acoustic/enclosure effect).

2. **Phase alignment** — even with correct BPM (PF achieves 85%), Beat F1 doesn't improve.
   Phase (timing of beat placement) is the limiting factor, not tempo estimation.

3. **Run-to-run variance** (std 0.04-0.23) — single-run evaluations cannot detect < ~0.15 F1 changes.

## Next Steps Toward 70% Beat F1

> **Design philosophy:** See [VISUALIZER_GOALS.md](../docs/VISUALIZER_GOALS.md) — visual quality over metric perfection. Low Beat F1 on ambient/trap tracks is acceptable (organic mode fallback is correct).

### Analysis: Why Are We Stuck at ~0.28?

The v40 testing revealed two independent bottlenecks:
1. **Double-time lock (3/4 devices):** ACF harmonic ambiguity (3.3x discrimination) causes
   devices to lock at ~180-195 BPM instead of correct tempo. ACM0 avoids this consistently
   (acoustic/enclosure effect). This dominates 4-device averages.
2. **Phase alignment:** Even PF with 85% BPM accuracy produces the same Beat F1 as CBSS.
   Beat placement timing within the beat cycle is the limiting factor.

### Priority 1: Address Double-Time Lock

The single biggest F1 improvement will come from preventing 3/4 devices from locking to double time.

**Options:**
- **PF tempo → CBSS phase (hybrid, already implemented):** PF avoids double-time but doesn't
  improve F1 yet. May need PF to influence CBSS period directly, not just as observation.
- **Stronger octave penalty in Bayesian posterior:** Current density octave + shadow checker
  provide ~13% each. Stacking additional cues (e.g., spectral bass/broadband ratio).
- **BTrack-style ACF post-processing:** Comb filter on ACF output to sharpen fundamental
  peak (v33 prototype exists as Viterbi, could be simplified).

### Priority 2: Phase Alignment Architecture

Once BPM is correct, improve beat placement:
- **Joint tempo-phase HMM (BTrack-style):** Viterbi on CBSS scores across phase-period grid.
  This is the standard approach in reference implementations.
- **Phase-locked loop on CBSS peaks:** Track CBSS peak phase over time, smooth corrections.
- **Stronger onset snap:** Current window=8 frames. Could use weighted snap (prefer high-ODF frames).

### Priority 3: Evaluate Visual Quality

Before further metric optimization, evaluate whether current performance produces
acceptable visual results:
- Run `render_preview` with real music patterns
- Compare animation smoothness and beat-reactivity
- If visual quality is acceptable at current F1, focus on reliability over accuracy

## PF Evaluation — Completed (v40)

### Phase 1: Smoke Test — PASSED
- PF runs stably on 3 tracks, no crashes, produces beats
- BPM accuracy: PF 0.848 vs CBSS 0.560 (+52%)
- Beat F1: PF 0.274 vs CBSS 0.295 (within noise)
- **Conclusion:** PF solves tempo, not phase

### Phase 2-5: Deferred
- Full A/B and parameter sweeps deferred pending architectural work on phase alignment
- PF's value is in tempo estimation; without phase improvement, parameter tuning won't help F1

### CBSS Parameter Sweeps — Completed (v40)

| Parameter | Values Tested | Optimal | Effect |
|-----------|:------------:|:-------:|--------|
| beatTimingOffset | 3, 5, 7, 9 | 5 (default) | Inconclusive — device BPM variation dominates |
| cbssTightness | 2, 5, 8, 12 | **8** | +24% on same-BPM devices, +3.6% on 18-track avg |

## Completed (v28-v40, Feb-Mar 2026)

- v28: FT+IOI disabled, beat-boundary tempo, peak picking, unified ODF, 40→20 bins
- v29: Transition matrix drift investigation (reverted 40 bins)
- v32: ODF mean sub disabled (+70%), density octave penalty (+13%), shadow octave checker (+13%)
- v33: BTrack-style Viterbi max-product on comb-ACF (experimental, not default)
- v35-v37: Phase alignment experiments (onset snap +20%, phase check/warmup/HMM negative)
- v38-v39: Particle filter implementation and bar-pointer model
- v40: cbssTightness 5→8, PF smoke test, parameter sweeps

## Known Limitations

| Issue | Root Cause | Visual Impact |
|-------|-----------|---------------|
| Double-time lock (~182 BPM) | ACF harmonic ambiguity (3.3x discrimination) | **Medium** — double-time looks "busy" |
| Run-to-run variance (std 0.04-0.23) | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation |
| Per-track beat offset (-58 to +57ms) | ODF latency varies by content | **None** — invisible at LED update rates |
| deep-ambience low Beat F1 | Soft ambient onsets below threshold | **None** — organic mode is correct |
| trap-electro low Beat F1 | Syncopated kicks challenge causal tracking | **Low** — energy-reactive acceptable |
