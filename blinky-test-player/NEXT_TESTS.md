# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 12, 2026 (Frame-level FC NN deployed on all devices, BandFlux obsolete)

## Current Config (v65+, SETTINGS_VERSION 64)

**ODF source:** FrameBeatNN (frame-level FC, 56.8 KB INT8, per-tensor quantization, ~3ms inference)
- FC(832→64→32→2), 55K params, 32-frame window (0.5s at 62.5 Hz)
- Beat + downbeat activation, deployed on all 3 devices (March 12)
- Current activation range: 0.07-0.26 (weak — mel level calibration needed)

**BandFlux:** OBSOLETE — scheduled for removal. Code and ~15 parameters to be deleted once NN mel calibration is complete.

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.8, bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, cbssTightness=8.0, cbsscontrast=2.0 (v66, A/B tested 10-6 win)
- beatoffset=5, onsetSnapWindow=8
- densityoctave=1, octavecheck=1 (v32 octave disambiguation)
- odfmeansub=0 (v32 -- raw ODF preserves ACF structure)
- pll=1, pllkp=0.15, pllki=0.005 (v45 PLL phase correction)

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
| Forward filter (fwdfilter=1) | 13 | 5 | 0 | **9.3** vs 15.4 | **17/18** | **REMOVED v64** |
| Fwd filter optimized (6-param) | -- | -- | -- | **12.5** vs 14.5 | **7/18** vs 4/18 | **REMOVED v64** |
| Hybrid phase (fwdphase=1) | 8 | 6 | 4 | 14.9 vs 14.8 | same | **REMOVED v64** |
| Noise subtraction (noiseest=1) | 5 | **13** | 0 | 17.1 vs 15.4 | +3 | Default OFF (kept in v64) |
| Template+subbeat (v50) | 8 | **10** | 0 | -- | -- | **REMOVED v64** |

## Current Bottlenecks

1. **Mel level mismatch (ACTIVE)** — Firmware mel values (mean ~0.52) are lower than training data (mean ~0.86). Causes weak NN activations on device (max ~0.26 at 120 BPM). Fix: retrain with corrected `target_rms_db` or capture real firmware mel streams for calibration.

2. **NN activation quality** — With correct mel calibration, NN should produce sharper, more discriminative beat activations. This is the biggest lever for improving overall beat tracking F1.

3. **~135 BPM gravity well** — Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks. Stronger NN ODF should help.

4. **Phase alignment** — correct BPM doesn't translate to correct beat placement. CBSS derives phase indirectly. Sharper NN activations help.

## Priority 1: NN Mel Calibration

**Status: NEEDED (March 12, 2026)**

The FC model is deployed and producing real output, but activations are weak. Root cause: firmware AGC produces lower mel levels than training normalization.

**Approaches to investigate:**
1. Lower `target_rms_db` in training config (e.g. -45 dB instead of -35 dB) to match firmware AGC levels
2. Capture real firmware mel streams via `stream nn` and use as calibration reference
3. Add mel normalization layer in firmware before NN input (mean/variance normalization)

## Priority 2: Conv1D Wide Model Evaluation

**Status: TRAINING COMPLETE (March 12, 2026)**

Conv1D wide model finished training (val_loss=0.4756, epoch 28). Needs export, evaluation, and comparison against FC model. FrameBeatNN.h auto-detects FC vs Conv1D from TFLite input shape.

## Priority 3: BandFlux Removal

Remove obsolete BandFlux code and ~15 associated parameters once NN mel calibration is complete. See `IMPROVEMENT_PLAN.md` Priority 2.

## ~~Priority 2: CBSS ODF Contrast~~ — COMPLETED (v66)

**Status: COMPLETED — cbssContrast=2.0 is now the default**

A/B tested cbssContrast=1.0 vs 2.0 (BTrack-style ODF squaring): 10 wins, 6 losses, 2 ties across 3 devices × 18 tracks. Mean BPM error 12.4 vs 12.6. Octave errors 9 vs 9 (unchanged). Default updated to 2.0 in v66.

## Future: Heydari 1D State Space

**Status: RESEARCH ONLY**

Heydari et al. (ICASSP 2022) showed a 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. Could be a path to explicit phase tracking without the forward filter's octave symmetry problem. ~860 states fits our memory budget.

## Closed Investigations

- **Tempo bins 20→47** (v61): Tested, no improvement. Gravity well is not a bin count issue.
- **Forward filter** (v57-v60): 6-param sweep, optimized but still 7/18 octave errors (vs CBSS 4/18). Half-time bias is fundamental. **Removed from firmware in v64.**
- **Spectral noise subtraction** (v56): A/B tested, hurts BPM accuracy (baseline wins 13/18). **Removed from firmware in v64.**
- **Focal loss** (v5): Identical to v4, no benefit.
- **Template+subbeat** (v50): No net benefit (baseline 10 wins, subbeat 8). **Removed from firmware in v64.**
- **Adaptive tightness, Percival harmonic, bidirectional snap, HMM, particle filter, PLP phase** — all removed as dead code in v64.

## Completed (v50-v64, March 2026)

- v50: Rhythmic pattern templates + subbeat alternation (A/B tested, default OFF)
- v54: NN beat activation CNN (v2 model, 5L ch32, 33.3 KB INT8)
- v55: v4 NN model (+36.6% vs v2), full augmentation + mic profile
- v56: Spectral noise subtraction (A/B tested, hurts -- default OFF)
- v56: AGC ceiling lowered 60->40, conservative AGC params
- v57: Forward filter (Krebs/Bock/Widmer 2015, ~860 states, A/B tested -- severe half-time)
- v58: Hybrid phase tracker (fwdphase=1), NN beat default ON
- v59: Forward filter Bayesian bias + asymmetric obs model
- v60: Full 6-parameter sweep of forward filter -- optimized but still 7/18 octave (vs CBSS 4/18). OFF.
- v61: Tempo bins 20→47 -- tested, no improvement. Reverted to 20 bins.
- v62: Firmware simplification (removed 6 disabled detectors, -19.5 KB flash, -5.4 KB RAM)
- v62: v2 consensus labels, training pipeline fixes (binary targets, time-stretch, test exclusion)
- v64: Dead code removal (forward filter, noise estimation, HMM, particle filter, PLP, adaptive tightness, percival, bisnap, template/subbeat checks, etc.). ConfigStorage 408->296 bytes.
- v66: cbssContrast=2.0 default (A/B tested 10 wins, 6 losses, 2 ties vs 1.0. Mean error 12.4 vs 12.6)
- v5: Focal loss training -- identical to v4, no benefit

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Multi-factorial (data bias, prior, comb harmonics) | **Medium** -- tracks lock to ~132 BPM | Improved NN ODF may help (training) |
| NN eval inflated | Test set data leakage (18 tracks in training data) | Unknown | Fixed in v6+; v9 DS-TCN in progress |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | Open research question |
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
