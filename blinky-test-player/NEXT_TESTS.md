# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 7, 2026 (v58+NN: nnbeat default ON, hybrid phase tracker, forward filter density penalty)

## Current Config (v58, SETTINGS_VERSION 58)

**Detector:** BandWeightedFlux Solo (all others disabled)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate)

**ODF source:** NN beat activation (default since v58, `nnbeat=1`)
- Causal 1D CNN, 5L ch32, 33.3 KB INT8 (v4 model)
- Requires `NN=1` build flag. Toggle: `nnbeat=0/1`
- A/B tested: 11/18 track wins vs BandFlux, -0.8 mean error

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.8, bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, cbssTightness=8.0, beatoffset=5, onsetSnapWindow=8
- densityoctave=1, octavecheck=1 (v32 octave disambiguation)
- odfmeansub=0 (v32 -- raw ODF preserves ACF structure)
- bisnap=1 (v44 bidirectional onset snap)
- pll=1, pllkp=0.15, pllki=0.005 (v45 PLL phase correction)
- adaptight=1 (v45 adaptive CBSS tightness)
- percival=1 (v45 ACF harmonic pre-enhancement)

**Default OFF (A/B tested, no net benefit):**
- `fwdfilter=0` -- severe half-time bias (17/18 octave errors)
- `fwdphase=0` -- BPM-neutral (8 wins vs 6, phase smoothness untested on LEDs)
- `templatecheck=0` -- no net benefit (baseline 10 wins, subbeat 8)
- `subbeatcheck=0` -- no net benefit
- `noiseest=0` -- hurts BPM accuracy (baseline wins 13/18)

## A/B Test Results Summary (March 7, 2026)

All tests: 18 EDM tracks, blinkyhost.local, middle-of-track seeking, `NODE_PATH=node_modules`.

| Feature | Wins | Losses | Ties | Mean Err | Octave Errs | Verdict |
|---------|:----:|:------:|:----:|:--------:|:-----------:|---------|
| NN beat (nnbeat=1) | **11** | 7 | 0 | **14.8** vs 15.6 | 7 vs 7 | Default ON |
| Forward filter (fwdfilter=1) | 13 | 5 | 0 | **9.3** vs 15.4 | **17/18** | OFF (half-time) |
| Hybrid phase (fwdphase=1) | 8 | 6 | 4 | 14.9 vs 14.8 | same | OFF (neutral) |
| Noise subtraction (noiseest=1) | 5 | **13** | 0 | 17.1 vs 15.4 | +3 | OFF (hurts) |
| Template+subbeat (v50) | 8 | **10** | 0 | -- | -- | OFF (no benefit) |

## Current Bottlenecks

1. **~135 BPM gravity well** -- nearly all tracks converge to ~135 BPM regardless of ODF source (affects both BandFlux and NN). Root cause: Rayleigh prior + comb harmonic resonance + ACF sub-harmonic bias.

2. **Forward filter half-time bias** -- 17/18 octave errors. Onset-density penalty just ported to forward filter (pending A/B test). If density penalty fixes it, forward filter's lower mean error (9.3 vs 15.4) and higher responsiveness (std 13.5 vs 5.1) could be significant.

3. **Phase alignment** -- correct BPM doesn't translate to correct beat placement. CBSS derives phase indirectly. Forward filter tracks phase explicitly but needs octave fix first.

## Priority 1: A/B Test Forward Filter + Density Penalty

**Status: IMPLEMENTED, awaiting A/B test**

Onset-density octave discriminator ported from Bayesian posterior into `updateForwardFilter()`. Penalizes tempo bins where `60 * onsetDensity / BPM` falls outside [0.5, 5.0] transients/beat.

**Hypothesis:** The density penalty should eliminate the half-time bias (where transients/beat < 0.5) while preserving the forward filter's lower mean error and higher tempo responsiveness.

**Test plan:**
1. Flash v59 firmware to blinkyhost devices (all 3)
2. Run batch A/B test: `fwdfilter=0` (baseline CBSS) vs `fwdfilter=1` (forward filter + density penalty)
3. Compare: octave errors, mean BPM error, track wins
4. If octave errors drop from 17/18 to < 5/18 AND mean error stays < 12: enable as default

**A/B test command:**
```bash
cd /home/blinkytime/blinky_time/blinky-test-player
NODE_PATH=node_modules node tools/ab_test_batch.cjs --port /dev/ttyACM0 --music-dir music/edm
```
(Modify script to toggle `fwdfilter` instead of `subbeatcheck`)

## Priority 2: Evaluate v5 Focal Loss Model

**Status: TRAINING (v5-focal-gamma2, ~8 hours)**

Focal loss `(1-p_t)^gamma * BCE` with gamma=2.0 down-weights easy negatives. Same architecture as v4 (5L ch32).

**Test plan:**
1. Wait for training to complete (~100 epochs)
2. Run evaluation: `make eval RUN_NAME=v5-focal-gamma2`
3. Compare Beat F1 and DB F1 vs v4 (Beat F1=0.717, DB F1=0.362)
4. If improved: export TFLite INT8, flash to devices, A/B test on hardware

## Priority 3: Asymmetric Observation Model (Forward Filter)

**Status: NOT STARTED -- contingent on Priority 1 results**

Scale `obsNonBeat` penalty by expected beats-per-bar at each tempo. Slower tempos should more strongly penalize high ODF at non-beat positions, breaking the octave symmetry.

Only pursue if density penalty alone doesn't fully fix half-time bias.

## Priority 4: Ensemble Simplification

**Status: IMPLEMENTED (solo detector fast path)**

`EnsembleFusion::fuse()` now short-circuits when exactly 1 detector is enabled, bypassing agreement scaling and weighted averaging. Multi-detector path retained for future experimentation.

**Test plan:** No regression expected (same output, less computation). Verify on next firmware flash.

## Completed (v50-v58, March 2026)

- v50: Rhythmic pattern templates + subbeat alternation (implemented, A/B tested, default OFF)
- v54: NN beat activation CNN (v2 model, 5L ch32, 33.3 KB INT8)
- v56: Spectral noise subtraction (A/B tested, hurts -- default OFF)
- v56: AGC ceiling lowered 60->40, conservative AGC params
- v57: Forward filter (Krebs/Bock/Widmer 2015, ~860 states, A/B tested -- severe half-time)
- v58: Hybrid phase tracker (fwdphase=1), NN beat default ON, v4 model (+36.6% vs v2)

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Rayleigh prior + comb harmonic resonance | **Medium** -- slow tracks lock to harmonics | Forward filter + density penalty (Priority 1) |
| Forward filter half-time | Observation model is octave-symmetric | **Blocked** | Density penalty implemented, needs A/B test |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | Forward filter (if octave fix works) |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** -- acceptable for visuals | -- |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** -- organic mode is correct | -- |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** -- energy-reactive acceptable | -- |
