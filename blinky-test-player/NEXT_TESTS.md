# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** March 10, 2026 (v64: dead code removal, v9 DS-TCN training in progress)

## Current Config (v64, SETTINGS_VERSION 64)

**Detector:** BandWeightedFlux Solo (6 disabled detectors removed in v62)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate)

**ODF source:** NN beat activation (default since v58, `nnbeat=1`)
- v9 DS-TCN 24ch committed to staging (26.5 KB INT8, commit 10711d2 "mead model"). Training in progress (24ch and 32ch variants — F1 and on-device inference time not yet measured)
- Previous standard conv models (5L/7L) take 79ms+ inference (~12 Hz), unacceptable framerate. DS-TCN targets ~25-30ms (~33-40 Hz)
- Requires `NN=1` build flag. Toggle: `nnbeat=0/1`
- A/B tested (v4 model vs BandFlux): 11/18 track wins, -0.8 mean error

**Beat tracking:** CBSS with Bayesian tempo fusion
- bayesacf=0.8, bayescomb=0.7, bayesft=0, bayesioi=0
- cbssthresh=1.0, cbssTightness=8.0, beatoffset=5, onsetSnapWindow=8
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

1. **~135 BPM gravity well** -- Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks, octave folding asymmetry. Tested 47 bins in v61 — no improvement. NOT a bin count issue.

2. **NN ODF quality** -- the biggest lever per SOTA research. v9 DS-TCN 24ch committed (staging), training in progress (24ch and 32ch). Standard conv models too slow (79ms+).

3. **Phase alignment** -- correct BPM doesn't translate to correct beat placement. CBSS derives phase indirectly. All systems achieving >60% F1 use explicit phase tracking (HMM state, PLP oscillator, or particle cloud).

## Priority 1: NN Model Training (v9 DS-TCN)

**Status: TRAINING IN PROGRESS (March 10, 2026)**

v9 DS-TCN 24ch committed to staging (26.5 KB INT8, commit 10711d2). Training in progress for 24ch and 32ch variants. **Critical constraint:** standard conv models (5L/7L) take 79ms+ inference → ~12 Hz framerate. Only DS-TCN architecture can achieve acceptable inference times (~25-30ms target).

**Previous models (completed):**
- v6-restart (5L ch32): Beat F1=0.727. 79ms inference (BN-fused). Too slow.
- v7-melfixed (7L ch32): Beat F1=0.787. Best offline F1 but >79ms inference. Too slow.
- v8 (7L ch48): Beat F1=0.821. Heap exhaustion on device.

**Outstanding (not yet addressed):**
- Compressed mel feature range (mean=0.84, ~50 INT8 levels) — inherent to -35 dB RMS + INT8
- ACF-based ODF quality metric — standalone F1 doesn't predict CBSS performance

**Changes for next training run (v10+):**
- `beat_cnn.py`: Set `bias=False` on `DSConvBlock.pw_conv` and `DSTCNBeatCNN.input_conv` (redundant before BatchNorm). Saves 120 params. Also update `export_tflite.py` TF model builder (`use_bias=False`) and weight transfer (`set_weights([tf_w])` without bias in unfused path). See TODO comments in code.

## Priority 2: CBSS ODF Contrast (cbssContrast=2.0)

**Status: NOT TESTED**

BTrack applies power-law contrast (squaring) to the ODF before feeding it into CBSS. This sharpens beat peaks relative to non-beat frames, making the cumulative score more discriminative. Our `cbssContrast` parameter exists (AudioController.h:315) but defaults to 1.0 (linear, no contrast).

**Rationale:** Code review against BTrack source confirms this is an intentional design choice in BTrack, not an accident. Squaring the ODF suppresses low-level noise while amplifying genuine onset peaks, which should improve CBSS beat/non-beat discrimination.

**Test plan:**
1. Single-parameter A/B test: `cbssContrast=1.0` (baseline) vs `cbssContrast=2.0` (BTrack-style)
2. Standard 18-track EDM test set, 3 devices, track manifest seeking
3. Duration 35s, settle 12s
4. Metric: BPM error + octave error count (same as previous A/B tests)
5. If 2.0 wins, optionally sweep [1.5, 2.0, 2.5, 3.0] for optimal value

**Command:**
```bash
cd blinky_time/blinky-test-player && NODE_PATH=node_modules node ../ml-training/tools/ab_test_multidev.cjs \
  --baseline "cbsscontrast=1.0" --candidate "cbsscontrast=2.0" \
  --tracks ../music/edm/track_manifest.json --duration 35 --settle 12
```

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
