# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 8, 2026 (v60: asymmetric obs model, Bayesian bias, sweep-optimal defaults)

## Current Config (v60, SETTINGS_VERSION 60)

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

1. **~135 BPM gravity well** -- ROOT CAUSE IDENTIFIED (Mar 8): coarse 20-bin tempo resolution. Only 2 bins cover 120-140 BPM (bin 4=132.0, bin 5=123.8). Bin 4 catches 128-139 BPM (11.5 BPM width). Full-resolution comb-on-ACF is computed but sampled at only 20 bin-center lags. Secondary: training data 33.5% in 120-140 BPM, Bayesian prior at 128 BPM, octave folding asymmetry.

2. **NN training data issues** -- test set leakage (F1=0.717 inflated), no time-stretch augmentation, Gaussian targets waste capacity, compressed mel feature range.

3. **Phase alignment** -- correct BPM doesn't translate to correct beat placement. CBSS derives phase indirectly.

## Priority 1: Increase Tempo Bins (20 -> 40)

**Status: NOT STARTED**

Root cause fix for the gravity well. Doubling tempo bins gives ~5 BPM resolution across the entire range instead of ~11 BPM at 130 BPM. Memory cost: ~17 KB extra RAM (double comb delay lines ~10 KB, transition matrix 40x40=6.4 KB). Fits in nRF52840's 256 KB RAM (currently using ~27 KB).

**Implementation:**
1. Change `CombFilterBank::NUM_FILTERS` from 20 to 40
2. Recompute transition matrix for 40 bins
3. Update all arrays indexed by TEMPO_BINS
4. Test: gravity well should be substantially reduced

## Priority 2: Fix NN Training Pipeline

**Status: NOT STARTED**

Five issues identified in training pipeline analysis (Mar 8):

1. **Exclude 18 EDM test tracks from training** (CRITICAL -- data leakage)
2. **Add time-stretch augmentation** to flatten BPM distribution (33.5% in 120-140 BPM)
3. **Reduce Gaussian sigma** or use binary targets (41.5% of frames wasted on tails)
4. **Improve mel normalization** (mean=0.84, only ~50 effective INT8 levels)
5. **Add ODF quality metric** to evaluation (standalone F1 doesn't predict CBSS performance)

Train v6 with fixes #1-3, evaluate, compare to v4.

## Priority 3: Visual Evaluation of fwdphase=1

**Status: BPM-neutral in A/B test, visual eval needed**

Forward filter phase tracking (`fwdphase=1`) was BPM-neutral (8 wins vs 6, mean err 14.9 vs 14.8). May give smoother LED animations. Needs eyes on hardware -- no code changes required.

## Completed (v50-v59, March 2026)

- v50: Rhythmic pattern templates + subbeat alternation (A/B tested, default OFF)
- v54: NN beat activation CNN (v2 model, 5L ch32, 33.3 KB INT8)
- v55: v4 NN model (+36.6% vs v2), full augmentation + mic profile
- v56: Spectral noise subtraction (A/B tested, hurts -- default OFF)
- v56: AGC ceiling lowered 60->40, conservative AGC params
- v57: Forward filter (Krebs/Bock/Widmer 2015, ~860 states, A/B tested -- severe half-time)
- v58: Hybrid phase tracker (fwdphase=1), NN beat default ON
- v59: Forward filter density penalty + Bayesian bias + asymmetric obs model
- v59: Full 6-parameter sweep of forward filter -- optimized but still 7/18 octave (vs CBSS 4/18). OFF.
- v5: Focal loss training -- identical to v4, no benefit

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | **Coarse 20-bin tempo resolution** (only 2 bins in 120-140 BPM range) | **Medium** -- tracks lock to ~132 BPM | Increase to 40 bins (Priority 1) |
| NN eval inflated | Test set data leakage (18 tracks in training data) | Unknown | Exclude test tracks, retrain (Priority 2) |
| Forward filter half-time | Observation model is octave-symmetric | N/A | **CLOSED** -- 6-param sweep, stays OFF |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | fwdphase=1 visual eval (Priority 3) |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** -- acceptable for visuals | -- |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** -- organic mode is correct | -- |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** -- energy-reactive acceptable | -- |
