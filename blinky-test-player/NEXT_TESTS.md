# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 8, 2026 (v60: literature review, revised priorities based on SOTA research)

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
- `fwdfilter=0` -- severe half-time bias (17/18 octave errors, known literature problem)
- `fwdphase=0` -- BPM-neutral (8 wins vs 6, phase smoothness untested on LEDs)
- `templatecheck=0` -- no net benefit (baseline 10 wins, subbeat 8)
- `subbeatcheck=0` -- no net benefit
- `noiseest=0` -- hurts BPM accuracy (baseline wins 13/18)

## SOTA Context (March 2026)

Best online/causal beat tracking systems on standard benchmarks (line-in audio):

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~28%** | NN ODF + CBSS (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends (RNN/CRNN/Transformer). Our gap is primarily in ODF quality, not the beat tracking backend. The NN ODF is the biggest lever for improvement.

**Reference tempo resolutions:** madmom uses 82 lag-domain bins (~2.4 BPM at 120 BPM), BTrack uses 41 bins (2 BPM steps), BeatNet uses 300 discrete levels. Our 20 bins (11.5 BPM at 130 BPM) is far coarser than any reference system.

## A/B Test Results Summary (March 7-8, 2026)

All tests: 18 EDM tracks, blinkyhost.local, middle-of-track seeking, `NODE_PATH=node_modules`.

| Feature | Wins | Losses | Ties | Mean Err | Octave Errs | Verdict |
|---------|:----:|:------:|:----:|:--------:|:-----------:|---------|
| NN beat (nnbeat=1) | **11** | 7 | 0 | **14.8** vs 15.6 | 7 vs 7 | Default ON |
| Forward filter (fwdfilter=1) | 13 | 5 | 0 | **9.3** vs 15.4 | **17/18** | OFF (half-time) |
| Fwd filter optimized (6-param) | -- | -- | -- | **12.5** vs 14.5 | **7/18** vs 4/18 | OFF (still worse) |
| Hybrid phase (fwdphase=1) | 8 | 6 | 4 | 14.9 vs 14.8 | same | OFF (neutral) |
| Noise subtraction (noiseest=1) | 5 | **13** | 0 | 17.1 vs 15.4 | +3 | OFF (hurts) |
| Template+subbeat (v50) | 8 | **10** | 0 | -- | -- | OFF (no benefit) |

## Current Bottlenecks

1. **~135 BPM gravity well** -- ROOT CAUSE: coarse 20-bin tempo resolution (11.5 BPM width at 130 BPM, vs madmom's 2.4 BPM). Only 2 bins cover 120-140 BPM. Secondary: training data BPM bias, Rayleigh prior, octave folding asymmetry.

2. **NN ODF quality** -- the biggest lever per SOTA research. Current v4 model has test set data leakage (F1=0.717 inflated), no time-stretch augmentation, compressed mel feature range, and wide Gaussian targets. SOTA systems use strong neural frontends (CRNN/Transformer) as the primary differentiator.

3. **Phase alignment** -- correct BPM doesn't translate to correct beat placement. CBSS derives phase indirectly. All systems achieving >60% F1 use explicit phase tracking (HMM state, PLP oscillator, or particle cloud).

## Priority 1: Increase Tempo Bins (20 -> 40+)

**Status: NOT STARTED**

Root cause fix for the gravity well. Previous v29 attempt at 40 bins failed due to BPM-space Gaussian transition matrix bug -- **fixed in v43** (now uses lag-space Gaussian). Should be re-attempted.

**Literature context:** madmom uses 82 lag-domain bins, BTrack 41 bins. Our 20 bins is the coarsest of any reference system. Consider lag-domain uniform spacing (natural ~2 BPM resolution at 120 BPM, ~41 integer lags at 60 Hz frame rate) rather than linear BPM spacing.

**Memory cost:** ~17 KB extra RAM for 40 bins (comb delay lines ~10 KB, transition matrix 40x40=6.4 KB). Fits in nRF52840's 256 KB RAM (currently ~27 KB used).

**Implementation:**
1. Change `CombFilterBank::NUM_FILTERS` from 20 to 40
2. Consider lag-domain uniform spacing instead of linear BPM
3. Recompute transition matrix (lag-space Gaussian, fixed since v43)
4. Update all arrays indexed by TEMPO_BINS
5. Test: gravity well should be substantially reduced

## Priority 2: Fix NN Training Pipeline (v6 model)

**Status: NOT STARTED**

Research confirms NN ODF quality is the biggest lever for improvement. Five issues identified:

1. **Exclude 18 EDM test tracks from training** (CRITICAL -- data leakage)
2. **Add time-stretch augmentation** to flatten BPM distribution (33.5% in 120-140 BPM). SOTA practice: Beat This! uses ±6% stretch, madmom uses ±4% pitch shift with sox. This is the most validated technique for tempo invariance.
3. **Reduce Gaussian sigma or use binary targets** (41.5% of frames wasted on tails). Literature uses narrow Gaussian (sigma=1-2 frames) or binary targets for beat activation.
4. **Improve mel normalization** (mean=0.84, only ~50 effective INT8 levels)
5. **Add ACF-based ODF quality metric** (standalone F1 doesn't predict CBSS performance)

Train v6 with fixes #1-3, evaluate, compare to v4.

## Priority 3: Visual Evaluation of fwdphase=1

**Status: BPM-neutral in A/B test, visual eval needed**

Forward filter phase tracking (`fwdphase=1`) was BPM-neutral (8 wins vs 6, mean err 14.9 vs 14.8). May give smoother LED animations. Needs eyes on hardware -- no code changes required.

## Future: Heydari 1D State Space (if tempo bins insufficient)

**Status: RESEARCH ONLY**

Heydari et al. (ICASSP 2022) showed a 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. This collapses the 2D (tempo x phase) state into a 1D phase-only representation where tempo changes are handled by "jumping back" in the state space. Could be a path to explicit phase tracking without the forward filter's octave symmetry problem. ~860 states fits our memory budget.

## Completed (v50-v60, March 2026)

- v50: Rhythmic pattern templates + subbeat alternation (A/B tested, default OFF)
- v54: NN beat activation CNN (v2 model, 5L ch32, 33.3 KB INT8)
- v55: v4 NN model (+36.6% vs v2), full augmentation + mic profile
- v56: Spectral noise subtraction (A/B tested, hurts -- default OFF)
- v56: AGC ceiling lowered 60->40, conservative AGC params
- v57: Forward filter (Krebs/Bock/Widmer 2015, ~860 states, A/B tested -- severe half-time)
- v58: Hybrid phase tracker (fwdphase=1), NN beat default ON
- v59: Forward filter Bayesian bias + asymmetric obs model
- v60: Full 6-parameter sweep of forward filter -- optimized but still 7/18 octave (vs CBSS 4/18). OFF.
- v5: Focal loss training -- identical to v4, no benefit

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | **Coarse 20-bin tempo resolution** (vs madmom 82, BTrack 41) | **Medium** -- tracks lock to ~132 BPM | Increase to 40+ bins (Priority 1) |
| NN eval inflated | Test set data leakage (18 tracks in training data) | Unknown | Exclude test tracks, retrain (Priority 2) |
| Forward filter half-time | Observation model is octave-symmetric (known literature issue) | N/A | **CLOSED** -- 6-param sweep, stays OFF |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | fwdphase=1 visual eval (Priority 3) |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs for reliable evaluation | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** -- acceptable for visuals | -- |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** -- organic mode is correct | -- |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** -- energy-reactive acceptable | -- |

## Key References (Added March 2026)

- BEAST: Streaming Transformer for online beat tracking (ICASSP 2024)
- BeatNet+: CRNN + particle filter, auxiliary training (TISMIR 2024, Heydari et al.)
- RNN-PLP-On: Real-time PLP beat tracking (TISMIR 2024, Meier/Chiu/Muller)
- Novel-1D: 1D state space with jump-back reward (ICASSP 2022, Heydari et al.)
- Percival 2014: Enhanced ACF + pulse train evaluation (IEEE/ACM TASLP)
- Krebs/Bock/Widmer 2015: Efficient state-space for joint tempo-meter (ISMIR)
- Davies 2010: Beat Critic octave error identification (ISMIR)
- Scheirer 1998: Comb filter bank tempo estimation
