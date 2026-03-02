# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 2, 2026

## Current Config (SETTINGS_VERSION 38)

**Detector:** BandWeightedFlux Solo (all others disabled)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate)

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.3 (v25 inverse-lag normalized), bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, beatoffset=5, onsetSnapWindow=4
- densityoctave=1, octavecheck=1 (v32 octave disambiguation)
- odfmeansub=0 (v32 — raw ODF preserves ACF structure)
- phasecheck=0, warmup=0, cbssContrast=1.0

**NEW (v38):** Particle filter beat tracker (disabled by default, A/B testable)
- `set particlefilter 1` to enable
- Replaces CBSS beat detection when active (Bayesian tempo + ACF still run)
- 100 particles, stratified resampling with octave injection (T/2, 2T)

## Current Baselines

### v36 18-Track Validation (Feb 28, 2026)

| Metric | 4-Device Avg | Best-Device Avg |
|--------|:------------:|:---------------:|
| Beat F1 | 0.280 | 0.364 |
| BPM Accuracy | 0.846 | 0.872 |

### v37 Onset Snap Validation (Mar 1, 2026)

5-run repeated test on trance-party (ACM0):
- **With onset snap (window=4):** mean F1=0.588, std=0.099
- **Without onset snap:** mean F1=0.491, std=0.173
- **+20% mean, -43% variance** — improvement is statistically significant

### Known Bottleneck

**Double-time lock at ~182 BPM** remains the primary failure mode.
ACF + comb provide only ~3.3x octave discrimination (inverse-lag 2x × Rayleigh 1.67x).
v32 features (density penalty + shadow octave checker) help but don't fully solve it.
The particle filter is designed to address this via competing tempo/phase hypotheses.

## v38 Particle Filter — Test Plan

### Phase 1: Smoke Test (Quick Validation)

Verify PF runs without crashes and produces beats on 3 representative tracks.

```
# Enable PF on ACM0, leave CBSS on ACM1 as control
run_music_test_multi(
  ports: ["/dev/ttyACM0", "/dev/ttyACM1"],
  port_commands: {"/dev/ttyACM0": ["set particlefilter 1"]},
  audio_file: "<track>",
  ground_truth: "<annotations>"
)
```

Tracks: trance-party (stable 138 BPM), techno-minimal-emotion (minimal), breakbeat-drive (complex)

**Pass criteria:** PF produces beats (Beat F1 > 0), no device crashes, BPM estimate present.

### Phase 2: Full 18-Track A/B Comparison

Run all 18 tracks with PF (ACM0) vs CBSS control (ACM1).
Compare Beat F1, BPM accuracy, and per-track results.

**Key questions:**
1. Does PF reduce double-time errors? (Check BPM accuracy on tracks where CBSS locks at ~182)
2. Does PF maintain performance on tracks where CBSS already works well?
3. What's the Beat F1 distribution like? (PF may have different failure modes)

### Phase 3: Parameter Sensitivity Sweep

If Phase 2 shows promise, sweep key PF parameters using multi-device testing:

| Parameter | Serial cmd | Default | Sweep range | Rationale |
|-----------|:----------:|:-------:|:-----------:|-----------|
| pfnoise | `pfnoise` | 0.02 | 0.005-0.08 | Period diffusion — too low = stuck, too high = jittery |
| pfbeatsigma | `pfbeatsigma` | 0.05 | 0.02-0.15 | Beat kernel width — narrow = precise but brittle |
| pfoctaveinject | `pfoctaveinject` | 0.10 | 0.0-0.25 | Octave injection ratio — key for octave disambiguation |
| pfbeatthresh | `pfbeatthresh` | 0.25 | 0.1-0.5 | Beat detection threshold — affects precision/recall tradeoff |
| pfcontrast | `pfcontrast` | 1.0 | 0.5-3.0 | ODF power-law — higher = peakier likelihood |

**Method:** 3 devices × 3 values per parameter × 3 tracks = 9 runs per parameter.
Use `run_music_test_multi` with `port_commands` to set different values per device.

### Phase 4: Repeated-Run Validation

Once optimal parameters are found, run 5× repeated tests on 4-5 tracks to verify
improvement is statistically significant (given run-to-run variance std=0.04-0.23).

### Phase 5: Full Suite Validation

Final 18-track validation with optimized PF parameters on all 4 devices.
Document results in PARAMETER_TUNING_HISTORY.md.

## Next Priorities (After PF Evaluation)

> **Design philosophy:** See [VISUALIZER_GOALS.md](../docs/VISUALIZER_GOALS.md) — visual quality over metric perfection. Low Beat F1 on ambient/trap tracks is acceptable (organic mode fallback is correct).

1. **If PF works:** Tune parameters, make default, remove/simplify Bayesian fusion
2. **If PF doesn't help:** Consider BTrack-style Viterbi on ACF (v33 prototype exists), or TinyML ODF
3. **Visual quality evaluation** — Run render_preview with PF-tracked beats, compare animation smoothness
4. **Generator tuning** — Adjust Fire/Water/Lightning response curves for PF beat characteristics

## Completed (v28-v37, Feb-Mar 2026)

- v28: FT+IOI disabled, beat-boundary tempo, peak picking, unified ODF, 40→20 bins
- v29: Transition matrix drift investigation (reverted 40 bins)
- v32: ODF mean sub disabled (+70%), density octave penalty (+13%), shadow octave checker (+13%)
- v33: BTrack-style Viterbi max-product on comb-ACF (experimental, not default)
- v35-v37: Phase alignment experiments (onset snap +20%, phase check/warmup/HMM negative)
- v38: Particle filter implementation (pending testing)

## Known Limitations

| Issue | Root Cause | Visual Impact |
|-------|-----------|---------------|
| Double-time lock (~182 BPM) | ACF harmonic ambiguity (3.3x discrimination) | **Medium** — double-time looks "busy" |
| Run-to-run variance (std 0.04-0.23) | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation |
| Per-track beat offset (-58 to +57ms) | ODF latency varies by content | **None** — invisible at LED update rates |
| deep-ambience low Beat F1 | Soft ambient onsets below threshold | **None** — organic mode is correct |
| trap-electro low Beat F1 | Syncopated kicks challenge causal tracking | **Low** — energy-reactive acceptable |
