# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 3, 2026

## Current Config (v43, SETTINGS_VERSION 42)

**Detector:** BandWeightedFlux Solo (all others disabled)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate)

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.3, bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, cbssTightness=8.0, beatoffset=5, onsetSnapWindow=8
- densityoctave=1, octavecheck=1 (v32 octave disambiguation)
- odfmeansub=0 (v32 — raw ODF preserves ACF structure)
- phasecheck=0, warmup=0, cbssContrast=1.0

**v43 algorithmic fixes (no new settings):**
- Removed double inverse-lag normalization (balanced ACF is sufficient)
- Full-resolution comb-on-ACF (47 lags, not just 20 bin centers)
- Octave folding: each bin sums evidence from lag L + lag L/2
- Lag-space Gaussian transition matrix (fixed sigma, symmetric bandwidth)

## Current Baselines

### v43 18-Track Validation (Mar 3, 2026, 3 bare boards)

| Metric | 3-Device Avg | Best-Device Avg |
|--------|:------------:|:---------------:|
| Beat F1 | 0.284 | 0.355 |
| BPM Accuracy | **0.877** | — |

### Historical Baselines

| Version | Change | N-Dev F1 | Best-Dev F1 | BPM Acc |
|---------|--------|:--------:|:-----------:|:-------:|
| v36 | Frame rate fix, onset snap, downward correction | 0.275 | 0.351 | 0.825 |
| v40 | cbssTightness 5→8 | 0.285 | 0.348 | — |
| v43-baseline | Pre-fix bare boards | 0.313 | — | **0.33** |
| **v43** | **4 Bayesian bug fixes** | **0.284** | **0.355** | **0.877** |

### Known Bottlenecks

1. **~128 BPM gravity well** — system gravitates to 125-135 BPM regardless of actual tempo.
   Tracks in-range (garage, minimal, trance) get 97-100% accuracy. Slow tracks (86-96 BPM)
   lock to 3:2 harmonic (~128 BPM). Rayleigh prior peaked at ~120 BPM likely contributes.

2. **Phase alignment** — even with 88% BPM accuracy, Beat F1 averages only 0.28.
   CBSS derives phase indirectly from beat counter. Onset snap helps but doesn't fully solve it.
   Garage-uk-2step: 99% BPM accuracy but F1 = 0.05-0.32 (pure phase problem).

3. **Run-to-run variance** (std 0.04-0.23) — single-run evaluations cannot detect < ~0.15 F1 changes.

## Next Steps Toward 70% Beat F1

> **Design philosophy:** See [VISUALIZER_GOALS.md](../docs/VISUALIZER_GOALS.md) — visual quality over metric perfection. Low Beat F1 on ambient/trap tracks is acceptable (organic mode fallback is correct).

### Analysis: Why Are We Stuck at ~0.28?

v43 testing on 3 identical bare boards revealed:
1. **Double-time lock SOLVED** (v43): Was caused by 4 compounding bugs in Bayesian tempo
   estimation, not acoustic/enclosure effects. BPM accuracy 33%→88%.
2. **~128 BPM gravity well**: Slow tracks (86-96 BPM) lock to 3:2 harmonic. Rayleigh prior
   + comb filter harmonic resonance pull toward the 120-130 BPM range.
3. **Phase alignment is the F1 bottleneck**: Correct BPM doesn't translate to correct beat
   placement. CBSS phase derivation is inherently indirect.

### Priority 1: Fix ~128 BPM Gravity Well

Slow tracks (breakbeat 86 BPM, reggaeton 92 BPM) lock to ~128 BPM (3:2 harmonic).

**Options:**
- **Widen Rayleigh prior** or flatten it below 100 BPM to stop pulling slow tempos upward
- **Stronger onset-density octave penalty** for 3:2 ratios (not just 2:1)
- **Extend octave folding to 3:2**: currently only folds L + L/2; could also fold L + 2L/3

### Priority 2: Phase Alignment Architecture

Once BPM is correct, improve beat placement:
- **Joint tempo-phase HMM (BTrack-style):** Viterbi on CBSS scores across phase-period grid.
  This is the standard approach in reference implementations.
- **Lower cbssTightness** (8→5): gives onsets more influence on phase correction per cycle.
  BTrack uses tightness=5.
- **Bidirectional onset snap:** Currently snaps forward only. Bidirectional could catch beats
  that fall slightly before the predicted position.

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

## Completed (v28-v43, Feb-Mar 2026)

- v28: FT+IOI disabled, beat-boundary tempo, peak picking, unified ODF, 40→20 bins
- v29: Transition matrix drift investigation (reverted 40 bins)
- v32: ODF mean sub disabled (+70%), density octave penalty (+13%), shadow octave checker (+13%)
- v33: BTrack-style Viterbi max-product on comb-ACF (experimental, not default)
- v35-v37: Phase alignment experiments (onset snap +20%, phase check/warmup/HMM negative)
- v38-v39: Particle filter implementation and bar-pointer model
- v40: cbssTightness 5→8, PF smoke test, parameter sweeps
- v42: PLP phase extraction (tested, no effect — disabled by default)
- v43: 4 Bayesian tempo bug fixes — BPM accuracy 33%→88%, double-time lock eliminated

## Known Limitations

| Issue | Root Cause | Visual Impact |
|-------|-----------|---------------|
| ~128 BPM gravity well | Rayleigh prior + comb harmonic resonance | **Medium** — slow tracks lock to 3:2 harmonic |
| Phase alignment limits F1 | CBSS derives phase indirectly from beat counter | **Medium** — correct BPM but misplaced beats |
| Run-to-run variance (std 0.04-0.23) | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation |
| Per-track beat offset (-58 to +57ms) | ODF latency varies by content | **None** — invisible at LED update rates |
| deep-ambience low Beat F1 | Soft ambient onsets below threshold | **None** — organic mode is correct |
| trap-electro low Beat F1 | Syncopated kicks challenge causal tracking | **Low** — energy-reactive acceptable |
