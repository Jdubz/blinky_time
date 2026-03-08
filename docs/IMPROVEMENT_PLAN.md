# Blinky Time - Improvement Plan

*Last Updated: March 8, 2026 (SOTA alignment review, gravity well root cause, revised priorities)*

## Current Status

### Completed: Mic Calibration & Gain Optimization (March 6, 2026)

Gain sweep completed on all 3 devices (ACM0, ACM1, ACM2) with pink noise reference audio. Calibration data stored at `ml-training/data/calibration/`.

**Per-Device Results:**

| Device | Best SNR Gain | Peak SNR | Max Gain (noise < 0.5) |
|--------|:---:|:---:|:---:|
| ACM0 | 25 | 24.9 dB | 20 |
| ACM1 | 35 | 24.8 dB | 20 |
| ACM2 | 30 | 24.1 dB | 15 |

**Dynamic Range vs Gain (median across 3 devices, bands 3-25):**

| Gain | Avg Noise Floor | Avg Dynamic Range |
|------|:-:|:-:|
| 0 | 0.372 | 38 dB |
| 5 | 0.354 | 39 dB |
| 15 | 0.387 | 37 dB |
| 30 | 0.431 | 34 dB |
| 50 | 0.513 | 29 dB |
| 80 | 0.633 | 22 dB |

**Gain-aware mic profile built** (`mic_profile.npz`): 17 gain levels × 26 bands, with recommended AGC range 0-15. Profile includes per-band gain (0.91-1.00, nearly flat) and gain-indexed noise floor for training augmentation.

**Firmware AGC ceiling set to 40** (`hwGainMaxSignal=40` in AdaptiveMic.h, `hw_gain_max: 40` in default.yaml, v56). Training pipeline must agree — `prepare_dataset.py` caps gain-aware noise augmentation at this value.

**Gain×Volume discriminability sweep completed** (`gain_volume_sweep.py`): 8 gains × 4 volumes × 4 tracks × 3 devices. Measured ODF AUC-ROC and Beat SNR with latency-corrected alignment (~560ms audio pipeline latency).

| Gain | ODF Beat SNR | AUC-ROC | Status |
|------|:---:|:---:|--------|
| 10 | 1.60x | 0.686 | Best discriminability |
| 20-40 | 1.55x | 0.675 | Excellent |
| 50 | 1.52x | 0.664 | Good |
| **60** | **1.42x** | **0.650** | **AGC ceiling — acceptable** |
| 70 | 1.33x | 0.642 | Degraded (17% SNR loss) |
| 80 | 1.28x | 0.620 | Poor (20% SNR loss) |

**Key finding: Speaker volume has negligible effect on discriminability.** AUC is nearly identical at 25% vs 100% volume because the AGC compensates. The noise floor at a given gain is the dominant factor, not signal amplitude. This means the AGC ceiling is the critical parameter — once gain is capped, the noise floor is bounded regardless of source loudness.

### Completed: Signal Chain Treatment (v56, March 6, 2026)

Gain sweep analysis revealed the beat detection path (BandFlux + NN) only sees hardware gain — software AGC, compressor, and whitening are all bypassed. Higher gain adds more noise than signal above gain 30-40, degrading ODF discriminability by 12-20%.

**Three-part treatment implemented:**

**1. Conservative hardware AGC (AdaptiveMic.h):**
- Gain ceiling: 60 → 40 (sweep shows SNR degrades above 35-40)
- Target raw level: 0.35 → 0.20 (accept quieter signal, rely on BandFlux `log(1+γ·mag)` for level mapping)
- Tracking tau: 30s → 60s (slow AGC preserves within-song dynamics, per Kates 2008 hearing aid literature)
- Calibration period: 30s → 60s (matches tracking tau)

**2. Spectral noise floor estimation (SharedSpectralAnalysis):**
- Minimum statistics noise estimation (Martin 2001, simplified for real-time)
- Per-bin smoothed power tracking → running minimum with slow release → noise floor estimate
- Spectral subtraction: `cleanMag = max(mag - α·√noiseFloor, floor·mag)`
- Inserted after FFT, before `preWhitenMagnitudes_` → benefits both BandFlux and NN paths
- Parameters: `noiseest=1`, `noisesmooth=0.92`, `noiserelease=0.999`, `noiseover=1.5`, `noisefloor=0.02`
- ~1 KB RAM (2×128 floats), ~0.1% CPU

**3. Training pipeline alignment:**
- `hw_gain_max: 60 → 40` in default.yaml (done)
- Spectral subtraction in training pipeline (done): `apply_spectral_noise_subtraction()` in `scripts/audio.py`, mirrors firmware algorithm exactly (per-bin smoothed power → running minimum → oversubtraction). Applied to FFT magnitudes before mel projection in `firmware_mel_spectrogram_torch()`. Config in `base.yaml` under `audio.noise_subtraction` (enabled=true, params match firmware defaults).
- Re-prepare dataset and retrain after firmware validation confirms improvement

**Literature:**
- Boll 1979: Spectral subtraction (noise estimation during silence)
- Martin 2001: Minimum statistics (continuous noise floor tracking without VAD)
- Cohen 2003 (IMCRA): Improved minima-controlled recursive averaging
- Kates 2008: Hearing aid AGC (slow-acting >30s preserves dynamics)
- Böck & Widmer 2013: Spectral flux onset detection with adaptive whitening (complementary to subtraction)

**Hardware A/B Test: Spectral Noise Subtraction (March 6, 2026):**

18-track EDM sweep on ACM0 (25s per track × 2 configs, seeking to middle of track):

| Metric | Baseline (noiseest OFF) | Noise Estimation ON |
|--------|:---:|:---:|
| Track wins | **13** | 5 |
| Mean BPM error | **15.4** | 17.1 |
| Octave errors | **4/18** | 7/18 |

**Finding: Noise estimation HURTS.** Higher BPM std on most tracks (instability), +3 octave errors, baseline wins 13/18. The minimum statistics estimator subtracts useful spectral content in the speaker-to-mic path, not just noise. Notable regressions: techno-minimal-01 (17.2 vs 6.1 err), trance-party (12.6 vs 3.8), edm-trap-electro (12.3 vs 4.7).

**Decision: Keep `noiseest=0` as default.** The conservative AGC ceiling (part 1) is the effective noise treatment. Spectral subtraction may help in noisier environments (outdoors, crowds) but hurts in controlled speaker-to-mic testing.

**Outstanding:**
- ~~Validate on hardware: A/B test~~ **DONE** — noise estimation regresses BPM accuracy
- ~~Implement spectral subtraction in training pipeline~~ **DONE** — `scripts/audio.py` + `configs/base.yaml`
- Training pipeline: consider disabling noise subtraction in feature extraction since firmware default is OFF

### Completed: Neural Network Beat Activation (v54, March 5-6, 2026)

Training a small causal CNN to replace BandFlux ODF with a learned beat activation. See [ML_TRAINING_PLAN.md](ML_TRAINING_PLAN.md) for full details.

**Completed:**
- Full `ml-training/` pipeline: feature extraction, model definition, training script, export, evaluation
- Causal 1D CNN: configurable dilated conv layers (3L baseline or 5L wider). ~9K-15K params, ~20-33 KB INT8
- Multi-output: beat activation (channel 0) + downbeat activation (channel 1)
- Acoustic environment augmentation (volume, noise, reverb, bass boost, RIR convolution)
- Spectral conditioning augmentation (static compressor + whitening approximation)
- Firmware integration: `BeatActivationNN.h` (multi-output TFLite Micro), `SharedSpectralAnalysis::getRawMelBands()`, `AudioControl.downbeat`, `AudioController` NN path
- Compiles on nRF52840: NN build 426 KB flash (52%) / 22 KB RAM (9%), non-NN 301 KB / 22 KB
- Labeling tool research: Beat This! (primary, SOTA), essentia (cross-validation), BeatNet (needs Python 3.11)
- Determinism verified: Beat This!, librosa, and essentia all produce bit-identical results across runs
- Cross-tool comparison on 18 EDM tracks: 94% BPM agreement, BT-essentia F1=0.948
- Multi-system consensus labeling (4 systems: Beat This!, essentia, librosa, madmom)
- Mic calibration pipeline: `calibrate_mic.py` with generate/capture/capture-all/gain-sweep/analyze
- Fixed consensus labels: Beat This! file naming bug excluded it from merge (stem.beats.beat_this → stem.beat_this). Re-merged with all 4 systems → 616K consensus beats (39% 2-sys, 35% 3-sys, 26% 4-sys agreement), 31.6% downbeat ratio
- Fixed config labels_dir: was pointing to Beat This!-only labels, now points to 4-system consensus
- Training v2 kicked off (overnight): 6993 tracks, 4-system consensus labels, augmentation ON, 100 epochs

**Training v1 (consensus-4sys-v1) — FAILED:**
- pos_weight was wrong: 10.0 (configured) vs 4.7 (measured from data). Downbeat pos_weight: 40.0 vs 10.5 actual.
- Model accuracy stuck at 61.7% (= negative class ratio — model learned nothing useful)
- Beat F1 = 0.473 mean on test set, but 2.09x over-detection ratio
- Downbeat F1 = 0.003 (broken — test labels were missing isDownbeat flags)

**Training v2 (consensus-4sys-v2) — COMPLETED, FUNCTIONAL:**
- pos_weight corrected: beat=4.7, downbeat=10.5
- Best val_loss=1.0445 at epoch 55/70 (early stopped, patience=15)
- **Mean Beat F1=0.525** @ threshold 0.50 (18 EDM test tracks, ±70ms mir_eval tolerance)
- **Mean Downbeat F1=0.256** (functional, up from 0.003)
- Detection ratio: 1.03x (near-perfect, v1 was 2.09x over-detecting)
- Best tracks: techno-minimal-01 (0.798), trance-party (0.722), trance-goa-mantra (0.698)
- Worst tracks: techno-dub-groove (0.230), dubstep-edm-halftime (0.402)
- Failure mode: model hedges on complex rhythms (peaks ~0.5-0.7 not 0.9+, valleys ~0.2 not 0.0). 240ms receptive field too narrow for slow/halftime patterns.
- Model exported to `beat_model_data.h` (20.4 KB INT8, hash c8e4c40c)

**Training v3 (consensus-4sys-v3-wider) — COMPLETED:**
- Wider receptive field: dilations [1,2,4,8,16], RF=63 frames=1008ms (vs 15 frames=240ms)
- 15,330 params, 33.3 KB INT8 (fits nRF52840: ~700 KB flash free, tensor arena bumped 8→16 KB)
- Config: `configs/wider_rf.yaml`
- Trained on clean data only (no augmentation, no mic profile)
- Not deployed — superseded by v4

**Training v4 (consensus-4sys-v4-augmented) — COMPLETED:**
- Same architecture as v3: 5L ch32, dilations [1,2,4,8,16], 15,330 params
- Full augmentation pipeline: clean + 4 gain levels + 3 noise floors + lowpass + bass-boost + 3 RIR = ~13 variants per track
- Mic profile augmentation: gain-aware noise floor from calibration data (17 gain levels × 26 bands)
- 4-system consensus labels (Beat This! + essentia + librosa + madmom)
- Dataset: 3M+ training chunks, 545K val chunks (6993 tracks × ~13 augmented variants)
- Best val_loss=0.9692 at epoch 53/68 (early stopped, patience=15; v2 best was 1.0445)
- **Final eval (18-track EDM test set):**
  - **Mean Beat F1=0.717** (+36.6% vs v2's 0.525)
  - **Mean Downbeat F1=0.362** (+41.4% vs v2's 0.256)
  - Best tracks: trance-infected-vibes 0.953, techno-minimal-01 0.947, techno-deep-ambience 0.884
  - Worst tracks: breakbeat-drive 0.449, reggaeton-fuego-lento 0.451
- Model exported to `beat_model_data.h` (33.3 KB INT8, hash 28b2dfd5)
- ~5 min/epoch on RTX 3080. Output: `/mnt/storage/blinky-ml-data/outputs/v4-wider-ch32-augmented/`

**Hardware A/B Test: NN Beat ODF vs BandFlux (March 6, 2026):**

v4 model deployed to all 3 devices (v57+NN firmware, `ENABLE_NN_BEAT_ACTIVATION`). TFLite library fix: `precompiled=full` → `precompiled=false` + cache clear. 426 KB flash (52%), 22 KB RAM (9%).

18-track EDM sweep on ACM0 (25s per track × 2 configs, seeking to middle of track):

| Metric | BandFlux (Baseline) | NN Beat |
|--------|:---:|:---:|
| Track wins | 6 | **11** |
| Ties | 1 | 1 |
| Mean BPM error | 15.6 | **14.8** |
| Octave errors | 4/18 | 5/18 |

**Best NN improvements:** techno-minimal-01 (0.6 vs 7.5 err), garage-uk-2step (3.3 vs 8.2), amapiano-vibez (18.2 vs 23.2).

**Finding: Modest but consistent improvement.** NN ODF wins 11/18 tracks with -0.8 lower mean error. Not transformative — both systems are dominated by the ~135-138 BPM gravity well in the Bayesian tempo estimator. The NN's advantage is in tracks where BandFlux locks to the wrong tempo region. Octave error rate is comparable (4 vs 5).

**Outstanding:**
- ~~Consider enabling `nnbeat=1` as default~~ **DONE (v58)** — 11/18 wins justifies it
- ~~Consider focal loss variant~~ **DONE (v5)** — identical results to v4, focal loss doesn't help
- ~~Deploy v4 model to devices → A/B test~~ **DONE**
- ~~Gain × Volume characterization sweep~~ **DONE** — see results in Mic Calibration section above. AGC ceiling of 40 validated.

**Training Pipeline Issues Identified (March 8, 2026):**

Analysis of training pipeline revealed several issues that may limit model quality:

1. **Test set data leakage (CRITICAL):** All 18 EDM evaluation tracks exist in the training data directory (`/mnt/storage/blinky-ml-data/audio/combined/`). Random train/val split likely included them in training. Beat F1=0.717 is inflated — true generalization unknown. Fix: exclude test tracks from `prepare_dataset.py`.

2. **Compressed mel feature range:** Mean mel value 0.84, IQR [0.78, 0.95]. Only ~0.2 units of dynamic range in [0,1] space. After INT8 quantization, only ~50 effective levels. The -35 dB RMS normalization helped but didn't fully solve the problem.

3. **Gaussian targets waste capacity:** sigma_frames=2 produces 41.5% frames in Gaussian tails (0 < target ≤ 0.5), more than the 16.6% at beat centers (target > 0.5). Model spends capacity fitting the tail shape that CBSS doesn't use. Fix: reduce sigma or use binary targets.

4. **No time-stretch augmentation:** Training data is 33.5% in 120-140 BPM range. Model has 3x more exposure to ~120 BPM beat patterns than 80 or 160 BPM. With RF=63 frames (1008ms, covering 2+ beats), model can learn tempo-correlated features. Fix: add time-stretch augmentation.

5. **Evaluation metric mismatch:** evaluate.py measures standalone beat detection F1, but model is used as ODF source for CBSS. A model with slightly smeared but consistent activations could score lower on F1 but produce better ACF peaks. Consider adding ACF-based ODF quality metric.

### Completed (March 4, 2026)

**v50 Rhythmic Pattern Templates + Subbeat Alternation — VALIDATED, default OFF, NO NET BENEFIT (SETTINGS_VERSION 50):**
- Rhythmic pattern templates (Krebs/Böck/Widmer ISMIR 2013): Pearson correlation of CBSS history against 3 precomputed zero-mean EDM bar templates (16 slots/bar). Compares T vs T/2 and T vs 2T every 4 beats. Calls `switchTempo()` if alternative wins by `templateScoreRatio` (1.3). Settings: `templatecheck=0`, `templatescoreratio=1.3`, `templatecheckbeats=4`.
- Beat Critic subbeat alternation (Davies ISMIR 2010): Bins CBSS into 8 subbeat slots, computes odd/even energy ratio. Strong alternation at T indicates double-time → switches to T/2. Only downward switching (upward branch removed as weak signal). Settings: `subbeatcheck=0`, `alternationthresh=1.2`, `subbeatcheckbeats=4`.
- **A/B tested (Mar 6, 2026):** 18-track EDM sweep on hardware. Results:
  - Baseline wins 10, subbeatcheck wins 8. Mean BPM error: baseline 16.3, subbeat 15.8 (within noise).
  - templatecheck alone: slightly worse than baseline (per-track A/B).
  - subbeatcheck alone: best per-track result on techno-minimal-01 (3.0 vs 15.5 error) but regression on techno-dub-groove (11.5 vs 6.6).
  - Both ON: best on earlier techno-minimal-01 test (129.1 vs 129.0 true) but inconsistent across genres.
  - **Dominant issue: ~140 BPM gravity well** — nearly all tracks converge to 130-148 BPM regardless of true tempo. Octave disambiguation features cannot fix this observation-side bias.
- Both features retained as OFF defaults. 300KB flash (37%), 22KB RAM (9%). 6 new settings.

**v49 Continuous ODF Observation Model in CBSS Phase Tracker (SETTINGS_VERSION 49):**
- Replaced Bernoulli observation model with continuous ODF observation in phase tracker. Addresses the key finding from v46 failure analysis.

### Completed (March 3, 2026)

**v47 Signal Chain Decompression — NEUTRAL, retained as default (SETTINGS_VERSION 47):**
- Implemented raw FFT magnitude path for BandFlux (`bfprewhiten=1`): bypasses both compressor and per-bin whitening. Matches how reference systems (SuperFlux, BTrack) feed their ODF.
- Implemented band-selective whitening bypass (`whitenbassbypass`): preserves kick drum contrast in bins 1-6.
- **Tested across 11 tracks + compressor/gamma parameter sweeps: NO measurable improvement.**
- BandFlux's `log(1+20*mag)` + adaptive threshold self-normalizes, compensating for any upstream processing.
- Transient detection identical across all configs (~200 detections, 0.83 recall on techno-minimal-01).
- Run-to-run Beat F1 variance (0.24-0.52) dwarfs any config effect.
- **Conclusion**: Signal chain is NOT the F1 bottleneck. Phase alignment remains the dominant limitation.
- Retained as default (`bfprewhiten=1`) since it matches reference architecture. +512B RAM.

**v46 HMM Phase Tracker Experiment — FAILED, CBSS retained (SETTINGS_VERSION 45, no version bump):**
- Attempted to replace CBSS beat detection with explicit phase tracking via Bernoulli observation model.
- Three approaches tested, all regressed vs CBSS baseline:

| Approach | Avg Beat F1 | Beat Count vs Expected | Root Cause |
|----------|:-----------:|:---------------------:|------------|
| Bernoulli argmax wrap | 0.241 | ~50-60% (under-fire) | Observation model requires onset at every beat; beats without transients missed |
| Simple countdown | 0.265 | ~72% (under-fire) | Onset snap + PLL extend effective period cumulatively |
| CBSS baseline | 0.366 | ~100% (correct) | Threshold-based detection adapts to signal, not reliant on onset at every beat |

- **Key finding**: The Bernoulli observation model `P(obs|pos=0) = ODF, P(obs|pos≠0) = 1-ODF` fundamentally requires a transient at every beat position. Real music has beats without strong transients (e.g., sustained bass notes, syncopated patterns). CBSS's threshold-based detection avoids this by using cumulative beat strength history.
- **v37 HMM analysis confirmed**: Both v37 (joint tempo-phase, 855 states) and v46 (phase-only, single period) HMM approaches fail for the same reason — the observation model conflates "onset" with "beat."
- **Conclusion**: CBSS `detectBeat()` is architecturally better-suited for noisy mic-in-room audio than probabilistic phase trackers. Future improvements should enhance CBSS phase alignment, not replace the detection mechanism.

**Signal chain audit — 6 adaptive systems identified that compound to reduce transient contrast:**

| System | Timescale | Risk | Mechanism |
|--------|-----------|------|-----------|
| **Compressor release** | 2.0s | HIGH | 3:1 ratio with 2s release dampens next transient after loud peak |
| **Per-bin whitening** | ~1s (decay 0.997) | HIGH | Running max normalization flattens sustained rhythmic content |
| **Hardware AGC** | 5-30s | HIGH | Gain adaptation lag during quiet→loud transitions |
| **BandFlux threshold** | ~0.8s (α=0.02) | MEDIUM | Additive threshold drift after loud sections misses quieter kicks |
| **ODF 5-point MA** | 80ms | MEDIUM | Phase lag + transient broadening |
| **Bayesian transition** | 2-5s | MEDIUM | Slow tempo lock |

- These interact multiplicatively: a kick triggers AGC gain reduction + compressor gain reduction + whitening running-max increase + threshold elevation = 3-4 independent attenuation stages on subsequent transients.
- **Next priority**: Investigate mitigation approaches (dual-path ODF, modified compressor parameters, band-selective whitening).

**v45 Percival Harmonic Enhancement + PLL Phase Correction + Adaptive Tightness (SETTINGS_VERSION 45):**
- **Percival ACF harmonic pre-enhancement** (`percival=1, percivalw2=0.5, percivalw4=0.25`): Folds ACF[2L]+ACF[4L] into ACF[L] before comb-on-ACF evaluation. Gives fundamental a unique ~2-3x advantage over sub-harmonic through the combined Percival+comb pipeline. Forward iteration safe. Targets 128 BPM gravity well.
- **PLL proportional phase correction** (`pll=1, pllkp=0.15, pllki=0.005`): Measures IBI error against expected period T at each beat fire, applies proportional + leaky integral correction (decay=0.95). Max shift capped at T/4. Targets slow phase drift.
- **Adaptive CBSS tightness** (`adaptight=1, tightlowmult=0.7, tighthighmult=1.3, tightconfhi=3.0, tightconflo=1.5`): Modulates log-Gaussian tightness based on onset confidence (OSS/cbssMean ratio). Strong onsets → looser (allow phase correction), weak → tighter (resist noise). Resolves tightness 5 vs 8 dilemma.
- All 3 features togglable for A/B testing. 11 new settings (3 bools + 8 floats).
- **TESTED** (Mar 3, 2026): 3-dev avg F1 0.317 (+11.6% vs v43 0.284), BPM accuracy 0.815 (-7.1% vs v43 0.877).
- A/B isolation: PLL +0.031 (strongest), adaptive tightness +0.012, Percival +0.010. All retained as defaults.
- Phase alignment remains the F1 bottleneck. v45 improvements are positive but confirm CBSS architectural ceiling (~0.35).
- SETTINGS_VERSION 45. 286KB flash (35%), 21.2KB RAM (8%). 188/192 settings slots used.

**v44 Bidirectional Onset Snap + 128 BPM Gravity Well Investigation (SETTINGS_VERSION 43):**
- Fixed MAX_SETTINGS overflow: 178 registered settings vs MAX_SETTINGS=128. Increased to 192.
- Bidirectional onset snap (`bisnap=1`): delays beat declaration by 3 frames (~45ms) so backward onset snap window covers frames arriving after predicted beat. **+0.005 avg F1, 9/18 track wins.** Large gains on trance tracks (+0.193 trance-goa-mantra, +0.124 trance-infected-vibes) from improved phase alignment.
- 3:2 octave folding (`fold32`, default OFF): folds comb evidence from 2L/3 into L. **No net benefit** in 18-track sweep (-0.009 avg F1, 1/18 wins). Kept as tunable toggle.
- 3:2 shadow octave check (`sesquicheck`, default OFF): tests 3T/2 and 2T/3 alternatives in CBSS octave checker. **No net benefit** alongside fold32.
- 3:2/2:3 transition matrix shortcuts (`harmonicsesqui`, default OFF): enables Bayesian posterior jumps between 3:2-related tempo bins. **Causes catastrophic regression** on fast tracks (130+ BPM pulled down to 100-110 BPM). The only feature that helps slow tracks escape 128 BPM lock, but too dangerous for production.
- Configurable Rayleigh prior peak (`rayleighbpm`, default 120). Shifting to 90-100 insufficient to escape 128 BPM well alone.
- Tunable switchTempo nudge (`temponudge`, default 0.8, was hardcoded 0.3). Stronger nudge improves octave checker responsiveness.
- **128 BPM gravity well ROOT CAUSE IDENTIFIED (Mar 8)**: coarse 20-bin tempo resolution is the primary cause. Only 2 bins cover 120-140 BPM (bin 4=132.0 BPM, bin 5=123.8 BPM). Bin 4 catches 128-139 BPM (11.5 BPM width). Full-resolution comb-on-ACF is computed at every lag but sampled only at 20 bin-center lags, throwing away fine-grained information. **Fix: increase to 40 bins (~5 BPM resolution, ~17 KB extra RAM).** Secondary causes: training data 33.5% in 120-140 BPM (no time-stretch augmentation), Bayesian prior at 128 BPM, octave folding asymmetry (bins >99 BPM cannot receive half-time bonus).
- SETTINGS_VERSION 43. 287KB flash (35%), 21KB RAM (8%).

**v43 Bayesian Tempo Bug Fixes — 4 critical fixes, BPM accuracy 33%→88%:**
- Fixed 4 compounding bugs identified by comparison with BTrack/madmom: (1) double inverse-lag normalization causing 1.65x upward bias, (2) coarse comb evaluation at 20 bins instead of full 47-lag resolution, (3) missing octave folding (L + L/2 evidence sum), (4) BPM-space transition matrix causing asymmetric bandwidth on lag-uniform grid.
- Validated on 3 identical bare boards — confirmed double-time lock is 100% algorithmic, not enclosure-related.
- BPM accuracy: 33% → 88%. Double-time ~195 BPM lock eliminated. Beat F1 unchanged (0.284 avg).
- New bottleneck: ~128 BPM gravity well (slow tracks lock to 3:2 harmonic). Phase alignment limits F1.
- Full per-track results: `blinky-test-player/PARAMETER_TUNING_HISTORY.md`

**v42 PLP Phase Extraction — TESTED, NO EFFECT (SETTINGS_VERSION 42):**
- Implemented PLP (Predominant Local Pulse) analytical phase extraction from Fourier angle of OSS at dominant tempo.
- Two approaches tested: (1) single-bin DFT phasor rotation over OSS buffer, (2) comb filter bank IIR-accumulated phase.
- Both produce garbage confidence: DFT confidence 3.2%, comb bank confidence 2.2%. The real-world OSS buffer (noisy, spiky, mic-in-room) lacks the clean periodicity that PLP requires.
- With confidence scaling, corrections round to 0 frames (`error * strength * confidence = -7 * 1.0 * 0.022 → 0`).
- Without confidence scaling, corrections are noise-driven (23 frames = 348ms!), but F1 averages to identical baseline (4-run mean: PLP OFF 0.426±0.065, PLP ON 0.426±0.045).
- **Root cause**: PLP is redundant with onset snap. Both align beats to nearby onset positions. Onset snap already does per-beat local correction within ±8 frames.
- **Conclusion**: Phase alignment is NOT the primary bottleneck as previously diagnosed. The F1 gap is dominated by tempo octave errors (double-time lock on 3/4 devices) and run-to-run variance (std=0.04-0.23).
- PLP params added but disabled by default: `plpphase=0`, `plpstrength=0.5`, `plpminconf=0.3`. SETTINGS_VERSION 42.

### Completed (March 2, 2026)

**v39 Frame Rate Fix + Onset Snap + Downward Harmonic Correction (SETTINGS_VERSION 39-41):**
- Fixed OSS_FRAME_RATE from 60 to 66 (measured ~66.4 Hz; PDM 16kHz / FFT-256 ≈ 62.5 theoretical). Was causing ~10.7% systematic BPM under-reporting.
- Onset snap window 4→8 frames (133ms vs 67ms). 119ms median phase offset couldn't be reached with 4 frames.
- Downward harmonic correction for >160 BPM (3:2 and 2:1 checks). Experimental — overcorrects 136 BPM trance to ~98 BPM. Gated behind `downwardcorrect` toggle (disabled by default, v41).
- BPM accuracy 45% → 82.5% (4-device avg). **F1 unchanged** (0.275 vs 0.280 baseline) — tempo octave errors and run-to-run variance dominate the F1 gap (see v42 analysis).
- Particle filter implemented (v38-39): 100 particles tracking (period, phase), madmom-style observation model, PF+CBSS hybrid. Achieves 85% BPM accuracy but F1 doesn't improve. Tempo octave errors and run-to-run variance dominate the F1 gap (see v42 analysis).
- cbssTightness 5→8 (v40): +24% in controlled comparison, +3.6% on full 18-track (within noise).
- Deprecated pfBeatSigma/pfBeatThreshold (unused in v39+ madmom observation model).
- SETTINGS_VERSION 41. 270KB flash (33%), 21KB RAM (9%).

**v32 Octave Disambiguation (SETTINGS_VERSION 32):**
- ODF mean subtraction disabled (`odfmeansub=0`): +70% F1. Raw ODF preserves natural ACF peak structure. Contradicts Feb 24 finding (pre-whitening era).
- Onset-density octave discriminator (`densityoctave=1`): +13% F1. Gaussian penalty on tempos where transients/beat < 0.5 or > 5.0.
- Shadow CBSS octave checker (`octavecheck=1`): +13% F1. Compares T vs T/2 every 2 beats, switches if T/2 scores 1.3x better.
- Full 18-track validation: 4-dev avg 0.265, best-dev 0.302 (vs baseline 0.148, +68-104%).

**v28 Feature Toggles + Simplification (SETTINGS_VERSION 28):**
- FT+IOI disabled by default (`bayesft=0`, `bayesioi=0`). No reference implementation (BTrack, madmom, librosa) uses these for real-time beat tracking.
- Beat-boundary tempo (`beatboundary=1`): defers period changes to beat fire, synchronizing tempo and CBSS.
- Dual-threshold peak picking (`bandflux_peakpick=1`): local-max confirmation with 1-frame look-ahead.
- Unified ODF (`unifiedodf=1`): BandFlux pre-threshold activation feeds CBSS, replacing duplicate `computeSpectralFluxBands()`.
- 40 tempo bins tested (v29), reverted — transition matrix drift 2x worse with 40 bins. **NOTE: Root cause was BPM-space Gaussian transition matrix on lag-uniform grid, fixed in v43.** 40 bins should now work correctly.

### Completed (February 25, 2026)

**Bayesian Weight Tuning (SETTINGS_VERSION 21-22, refined in 24):**
- Multi-device parallel sweep of all 6 Bayesian parameters using beat event scoring
- Root cause analysis: ACF/FT/IOI observations have fundamental implementation issues (compared to BTrack, madmom, librosa)
- v21: ACF/FT/IOI=0 (disabled), comb=0.7, lambda=0.15, cbssthresh=1.0 → F1 0.590
- v22: Combined validation found bayesacf=0.3 needed to prevent half-time lock → F1 0.519
- v24: Post-spectral re-tuning re-enabled FT=2.0 and IOI=2.0 (spectral processing fixes normalization issues)

**Multi-Device Testing Infrastructure:**
- 3 devices (Long Tube) connected simultaneously via `/dev/ttyACM0,1,2`
- Replaced Playwright browser-based audio playback with `ffplay` — works headless on Raspberry Pi, no X server needed
- Fixed serial port leak in MCP server: `sendCommand('stream fast')` left `this.streaming=false` while firmware was actively streaming, causing `disconnect()` to skip `stream off` and lock the port. Fix: track streaming commands in `sendCommand()` + always send `stream off` in `disconnect()` as safety net
- Multi-device variation test: all 3 devices capture simultaneously from single audio playback. F1 spread 0.014 on techno-minimal-01 — devices are highly consistent
- Multi-device parallel sweep: batches parameter values across N devices (3x speedup with 3 devices)
- Audio routed to USB speakers (JBL Pebbles) via `.asoundrc`
- Music test files (`.mp3` + `.beats.json` ground truth) auto-discovered from `music/` directory

### Completed (February 23-24, 2026)

**Bayesian Tempo Fusion + CBSS Adaptive Threshold (SETTINGS_VERSION 20):**
- Replaced sequential override chain (~400 lines) with unified Bayesian posterior estimation over 20 tempo bins (60-180 BPM)
- Four observation signals (autocorrelation, Fourier tempogram, comb filter bank, IOI histogram) contribute per-bin likelihoods multiplied with Viterbi transition prior
- Removed 17 old parameters (HPS, pulse train, harmonic thresholds, cross-validation thresholds), added 9 Bayesian/beat params
- Per-sample ACF harmonic disambiguation: after MAP extraction, checks raw autocorrelation at lag/2 (2x BPM, >50% thresh) and lag*2/3 (1.5x BPM, >60% thresh) — fixes sub-harmonic lock on minimal-01 (BPM 70→124)
- CBSS adaptive threshold (`cbssthresh=0.4`): beat fires only if CBSS > factor * running mean (EMA tau ~2s). Prevents phantom beats during silence/breakdowns
- Optional ongoing static prior (`bayespriorw`, default off) — tested but static prior hurts tracks far from center
- Removed dead code: `evaluatePulseTrains()`, `generateAndCorrelate()`, old sequential override chain
- Fixed pre-existing bug: IOI onset ring buffer not shifted during sampleCounter_ wrap (~4.6 hour edge case)

### Completed (February 21-22, 2026)

**Detector & Beat Tracking Optimization:**
- BandWeightedFluxDetector set as sole default detector (all others disabled)
- BandFlux parameters confirmed near-optimal via sweep: gamma=20, bassWeight=2.0, threshold=0.5
- beatoffset recalibrated 9→5, doubling avg Beat F1 (0.216→0.452 across 9 tracks)
- Onset delta filter (minOnsetDelta=0.3): rejects slow-rising pads/swells, improved avg Beat F1 0.452→0.472
- IOI histogram, Fourier tempogram, ODF mean subtraction implemented as independent features (later unified into Bayesian fusion)

### Completed (February 2026)

**Rhythm Tracking:**
- CBSS beat tracking with counter-based beat detection (replaced multi-hypothesis v3)
- Deterministic phase derivation from beat counter
- Adaptive cooldown — tempo-aware, scales with beat period
- BandWeightedFluxDetector Solo (replaced Drummer+Complex ensemble)

**Particle System & Visuals:**
- Frame-rate independent physics (centiseconds, not frames)
- Continuous mode blending (replaced binary `hasRhythm()` threshold)
- Particle variety system (FAST_SPARK, SLOW_EMBER, BURST_SPARK types)
- Smooth 6-stop color gradient for fire (eliminated banding)
- Hardware AGC full range in loud mode (0-80, was 10-80)
- Multi-octave SimplexNoise turbulence wind (replaced sine wave)
- Runtime device configuration (safe mode, JSON registry, serial upload)

### Completed (December 2025)

**Architecture:** Generator → Effect → Renderer, AudioController v3, ensemble detection (6 algorithms), agreement-based fusion, comprehensive testing infrastructure (MCP + param-tuner), calibration completed.

---

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

### Current Priorities (March 2026)

| Priority | Task | Expected Impact | Status |
|----------|------|----------------|--------|
| **1** | Increase tempo bins (20→40+) | Fix gravity well (root cause) | Not started |
| **2** | Fix NN training pipeline (v6 model) | Biggest lever per SOTA research | Not started |
| **3** | Visual eval of fwdphase=1 | May improve LED smoothness | BPM-neutral, needs eyes on hardware |

**Priority 1: Increase Tempo Bins (20→40+)**
Root cause fix for the ~135 BPM gravity well. Previous v29 attempt at 40 bins failed due to BPM-space Gaussian transition matrix bug — **fixed in v43** (now uses lag-space Gaussian). Should be re-attempted. Consider lag-domain uniform spacing (natural ~2 BPM resolution at 120 BPM, ~41 integer lags at 60 Hz frame rate) rather than linear BPM spacing. ~17 KB extra RAM (fits nRF52840's 256 KB).

**Priority 2: Fix NN Training Pipeline (v6 model)**
Research confirms NN ODF quality is the biggest lever. Five issues: (1) exclude 18 test tracks from training (data leakage), (2) add time-stretch augmentation (33.5% training data at 120-140 BPM), (3) reduce Gaussian sigma or use binary targets (41.5% of frames wasted on tails), (4) improve mel normalization (compressed range, ~50 effective INT8 levels), (5) add ACF-based ODF quality metric.

**Priority 3: Visual Eval of fwdphase=1**
Forward filter phase tracking was BPM-neutral in A/B test (8 wins vs 6). May give smoother LED animations. No code changes required.

**Future: Heydari 1D State Space**
Heydari et al. (ICASSP 2022) showed a 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. Collapses 2D (tempo × phase) into 1D phase-only where tempo changes are "jumps back" in the state space. ~860 states fits our memory budget. Could replace CBSS if tempo bins + NN training improvements are insufficient.

## Design Philosophy

See **[VISUALIZER_GOALS.md](VISUALIZER_GOALS.md)** for the guiding design philosophy. Key principle: the goal is visual quality, not metric perfection. Low Beat F1 on ambient, trap, or machine-drum tracks may represent correct visual behavior (organic mode fallback). False positives are the #1 visual problem.

## Outstanding Issues

### Priority 1: False Positive Elimination — RESOLVED (Feb 22, 2026)

**Status:** Closed. Remaining ~16-22 pad FPs on synthetic pattern are **visually acceptable** — timing analysis shows they are on-beat, not random.

**Timing analysis of pad-rejection FPs (Feb 22):**
Analysis of 22 FPs from pad-rejection pattern (80 BPM, 750ms beat period):
- **4 FPs** are direct pad triggers — precisely on the 80 BPM beat grid (within 6-102ms of pad onset)
- **14 FPs** are reverb/echo tails — within 200-700ms of a real event (kick, snare, or pad)
- **4 FPs** are pre-kick room reflections — 230-590ms before a kick
- **0 FPs** are truly random or unrelated to musical events

**Visual impact:** Since all FPs cluster around actual musical events on the beat grid, they appear as "slightly extra busy but still rhythmic" rather than "random sparks with no musical cause." The pad-rejection pattern is a worst case (isolated pads with no other content). In real music, pads are mixed with kicks/snares and the extra triggers are masked.

**Approaches tested and rejected (Feb 22):**
1. **Bass-ratio gate** — Fails: snares are mid-dominant like pads
2. **Band-dominance gate** (max band / total) — Fails: room acoustics smear spectral content
3. **Post-onset decay gate** (decayRatio + decayFrames) — Discriminates kicks vs pads, but synth stabs share sustained envelope → regresses synth-stabs 1.000→0.59
4. **Min-flux decay variant** — Same result as above
5. **Spectral crest factor gate** (crestGate) — Eliminates all pad FPs but kills kicks through room resonances (recall 0.31 on strong-beats)

**Active gates (production config):**
1. Hi-hat rejection gate (high-only flux suppression)
2. Onset delta filter (minOnsetDelta=0.3, rejects slow-rising signals)

**Available but disabled gates** (for future experimentation):
- `bfdominance` — band-dominance gate (0.0=disabled)
- `bfdecayratio` + `bfconfirmframes` — post-onset decay gate (0.0=disabled)
- `bfcrestgate` — spectral crest factor gate (0.0=disabled)

### Priority 2: CBSS Beat Tracking + Bayesian Tempo Fusion — VALIDATED, PHASE BOTTLENECK IDENTIFIED (v41)

BTrack-style predict+countdown CBSS beat detection with Bayesian tempo fusion. Tempo estimated via unified posterior over 20 bins (60-180 BPM). Comb filter bank + harmonic-enhanced ACF (weight 0.8, v25). FT and IOI disabled (v28). CBSS adaptive threshold (1.0) prevents phantom beats. Frame rate corrected to 66 Hz (v39). Onset snap window 8 frames (v39). cbssTightness 8.0 (v40). Particle filter hybrid (v38-39, BPM accuracy 82.5%).

**Current bottleneck: phase alignment + ~128 BPM gravity well.** v43 fixed 4 Bayesian tempo bugs (BPM accuracy 33%→88%), eliminating double-time lock. Despite correct BPM, Beat F1 is unchanged at ~0.28. Phase alignment (beat placement timing) is the primary F1 bottleneck. Secondary: slow tracks (86-96 BPM) lock to ~128 BPM via 3:2 harmonic. Run-to-run variance (std=0.04-0.23) limits measurement precision.

#### Literature Review — Root Cause Analysis (March 2026)

Research into BTrack, madmom, Essentia/Percival, librosa, and recent papers identified root causes and untried techniques:

**128 BPM gravity well — three compounding forces:**
1. **Additive comb-on-ACF** (lines 912-948): Sums ACF at L, 2L, 3L, 4L — boosts harmonics just as much as fundamentals. This is the core problem. A comb tuned to lag 31 (128 BPM) accumulates nearly as much energy from a 86 BPM signal as the lag-46 (86 BPM) comb itself.
2. **Rayleigh prior** (lines 812-828): Peaked at lag 33 (120 BPM), gives bins at 86 BPM ~35% less multiplicative weight.
3. **IIR comb bank resonance** (lines 3193-3209): No harmonic inhibition. Resonator at 128 BPM accumulates energy from 64 BPM signals indistinguishably.

BTrack has the same three mechanisms and the same fundamental limitation — it just works well enough because its test corpus doesn't emphasize slow tempos. No system using additive comb evaluation has solved 3:2 harmonic ambiguity.

**Phase alignment — CBSS is inherently limited:**
- Alpha=0.9: 90% of CBSS comes from history → self-reinforcing phase lock-in
- Phase is emergent (from a counter), not an explicit state variable
- Asymmetric correction: onsets before prediction found by backward search, onsets after prediction arrive too late
- Small tempo errors accumulate as phase drift (1 frame/beat with 3% error → half-period drift after 17 beats)
- **All systems achieving >60% F1 track phase explicitly** (HMM state variable, Fourier angle, or particle cloud)

**Tightness 8 vs BTrack's 5 — explained:**
BTrack's tightness=5 assumes clean line-in audio. Our reverberant mic setup has noisier onsets, so higher tightness prevents noise from pulling phase incorrectly. The correct solution is **adaptive tightness** (lower when onset confidence high, higher when low), not a fixed value.

#### Untried Techniques from Literature (Ranked by Expected Impact)

**For phase alignment (PRIMARY bottleneck):**

| # | Technique | Source | Effort | Expected Impact | Status |
|---|-----------|--------|--------|----------------|--------|
| **A** | **Forward filter with continuous ODF observation** | madmom obs model (Krebs/Böck 2015) | ~5.1 KB RAM, ~1% CPU | **HIGH** — addresses root cause; all systems >60% F1 use this | **v57 — IMPLEMENTED, full 6-param sweep: best 7/18 oct err vs CBSS 4/18. Default OFF.** |
| 2a | **PLL-style proportional correction** | PLL beat tracking (Kim 2007) | ~20 bytes, trivial CPU | +0.031 F1, addresses slow phase drift | **v45 — DONE, retained** |
| 2b | **Adaptive tightness** | Novel (noise-vs-correction tradeoff) | ~10 bytes, trivial CPU | +0.012 F1, resolves 5 vs 8 dilemma | **v45 — DONE, retained** |
| 2c | **Off-beat suppression in CBSS** | Davies & Plumbley (2007) | ~100 bytes, 0.1% CPU | Low — minor refinement per literature | Deprioritized |

**For 128 BPM gravity well (SECONDARY bottleneck):**

| # | Technique | Source | Effort | Expected Impact | Status |
|---|-----------|--------|--------|----------------|--------|
| **B** | **Rhythmic pattern templates** | Krebs/Böck/Widmer (ISMIR 2013) | ~512 bytes, negligible CPU | **MEDIUM** — "drastically reduces octave errors" per paper | **v50 — IMPLEMENTED, default OFF, awaiting validation** |
| **C** | **Beat Critic subbeat alternation** | Davies (ISMIR 2010) | ~256 bytes, negligible CPU | **MEDIUM** — different discriminative signal from metricalcheck | **v50 — IMPLEMENTED, default OFF, awaiting validation** |
| 1a | **Percival ACF harmonic pre-enhancement** | Essentia `percivalenhanceharmonics.cpp` | ~50 ops, 0 memory | +0.010 F1, marginal | **v45 — DONE, retained** |
| 1b | **Anti-harmonic comb (percivalw3)** | Speech F0 estimation | ~50 ops, 0 memory | Marginal, default OFF | **v48 — tested, marginal** |
| 1c | **Metrical contrast check** | Beat Critic (ISMIR 2010) | ~20 ops/beat | Negative on full validation | **v48 — tested, default OFF** |

**Technique A — Forward Filter with Continuous ODF Observation (v57, IMPLEMENTED):**

Joint tempo-phase forward filter (Krebs/Böck/Widmer 2015). Tracks tempo and phase jointly via forward algorithm. 20 tempo bins × variable phase positions (~700 states). Continuous ODF observation model. Toggle: `fwdfilter=1` (default OFF for A/B testing).

**How it works:** Each state (tempo_i, phase_j) receives a likelihood at every frame based on the current ODF value. At beat-zone positions (first 1/λ of period), high ODF = high likelihood. At non-beat positions, low ODF = high likelihood (confirms "no onset here, as expected"). The state with the highest accumulated probability determines both tempo and phase. Beat detected when the argmax position wraps from near period-1 to near 0.

**Why this differs from v46 (which failed):**

| | v46 Bernoulli (FAILED) | v57 Continuous ODF |
|---|---|---|
| At beat, ODF=0.1 | P → 0.1 (near-catastrophic) | P = 0.1 (low but survivable) |
| At beat, ODF=0.0 | P → 0 (tracker crashes) | P ≈ floor (other states fill in) |
| Between beats, ODF=0.0 | P = 1.0 (binary certainty) | P = 1/λ ≈ 0.125 (proportional) |
| Missing an onset | Probability collapses | Probability reduced gracefully |

**Implementation (actual, v57):**
- State space: 20 tempo bins × variable phase per bin (period in samples) = ~700 total states (880 max)
- Per frame: shift all phase positions forward by 1, apply observation likelihood
- Observation: `obsBeat = max(λ * ODF^contrast, floor)`, `obsNonBeat = max((1 - ODF^contrast) / (λ-1), floor)`
- Beat zone: first `period/λ` positions of each tempo bin
- Transition: Gaussian `exp(-lagDiff² / 2σ²)` for tempo changes, applied only at position 0 (beat boundary)
- Beat detection: argmax position wraps from near period-1 (last 25%) to near 0 (first 25%) + cooldown + silence gate
- Onset snap + PLL correction reused from CBSS path
- CBSS still updated for `sampleCounter_++` and `cbssMean_` (silence gate)
- Settings: `fwdtranssigma=3.0`, `fwdfiltcontrast=2.0`, `fwdfiltlambda=8.0`, `fwdfiltfloor=0.01`
- RAM: ~5.1 KB (3.5 KB alpha + 1.6 KB transition matrix)
- CPU: ~700 multiplications per frame at 66 Hz = negligible on 64 MHz
- SETTINGS_VERSION 57. When enabled, bypasses Bayesian tempo fusion for tempo setting (guard added to `externalTempoActive`)

**Technique A — A/B Test Results (March 6, 2026):**

18-track EDM sweep on ACM0 (v57 firmware, 25s per track × 2 configs).

| Metric | CBSS Baseline | Forward Filter |
|--------|:---:|:---:|
| Mean BPM error (octave-aware) | 15.4 | **9.3** |
| Track wins | 5 | **13** |
| Octave errors | 7/18 | **17/18** |
| Mean std dev (BPM stability) | **5.1** | 13.5 |

**Finding: Severe half-time bias.** The forward filter consistently tracks at ~½ the true tempo (65-95 BPM for 120-175 BPM tracks). It gets lower octave-aware error because half-time is often closer than baseline's ~135 BPM gravity well, but **17/18 tracks are octave errors** vs 7/18 for baseline.

**Root cause analysis:**
1. **Rayleigh prior favors slow tempos**: The prior peaks at 120 BPM lag, which maps to ~30 samples. Half-tempo (~60 BPM, lag ~60) gets moderate prior weight, and once the filter locks to half-time, the observation model reinforces it because every other beat aligns with the slow tempo.
2. **Narrow beat zone (λ=8, 12.5%)**: At half-tempo, beats land in the beat zone every other real beat. The missed real beats produce moderate ODF values at non-beat positions, which are only weakly penalized by `(1-ODF)/(λ-1)`.
3. **No tempo lower bound enforcement**: The forward filter tracks tempos down to 60 BPM (the OSS buffer limit), while the baseline's Bayesian posterior + comb bank have stronger upward pressure from the gravity well.

**Follow-up parameter sweep (lambda=4, 6, 8):**
- λ=4: Swings to double-time (152-169 BPM for 118-129 BPM tracks). Too wide a beat zone.
- λ=6: Still half-time biased (4/5 tracks at 73-106 BPM for 118-154 BPM true). Marginally better.
- λ=8 (default): Consistent half-time (17/18 octave errors in full sweep).
- **Conclusion: lambda tuning cannot fix the octave problem.** The observation model is fundamentally octave-symmetric — at half-time, the filter sees a beat every other cycle and reinforces it.

**Full 6-parameter sweep (Mar 8, 2026):**

Systematic sweep of all 6 forward filter parameters using 3-device batched testing (3 values per audio pass, ~23 min per sweep). Each sweep used optimized values from previous sweeps.

| Parameter | Range | Best | Score | Oct Err | Significance |
|-----------|-------|------|-------|---------|-------------|
| `fwdtranssigma` | 0.3-5.0 | 0.6 | 82.9 | 7/18 | **HIGH** — most impactful (13->7 octave errors) |
| `fwdbayesbias` | 0-1 | 0.2 | 82.5 | 7/18 | **HIGH** — 0 = catastrophic (12/18 octave) |
| `fwdasymmetry` | 0-4 | 0.8 | 92.2 | 8/18 | Moderate — helps at low values, hurts >3.0 |
| `fwdfiltcontrast` | 1-5 | 1.0 | 83.5 | 7/18 | Moderate — higher = more octave errors |
| `fwdfiltlambda` | 4-16 | 10 | 93.3 | 8/18 | Low — noisy, no clear trend |
| `fwdfiltfloor` | 0.001-0.05 | ~0.01 | 82.5 | 7-8 | **None** — no measurable effect |

Optimal config: `sigma=0.6, bias=0.2, lambda=10, asym=0.8, contrast=1, floor=0.01`
Best achievable: **mean err 12.5, 7/18 octave errors** (score ~82.5)

**CBSS baseline: mean err 14.5, 4/18 octave errors**

**Conclusion: Forward filter CANNOT beat CBSS baseline on octave errors.** The optimized forward filter has lower mean BPM error (12.5 vs 14.5) but nearly double the octave errors (7 vs 4). The observation model is fundamentally octave-symmetric — at half-time, every other beat aligns perfectly, and no amount of parameter tuning can break this symmetry. 6 tracks (breakbeat-background, breakbeat-drive, dnb-energetic-breakbeat, dnb-liquid-jungle, dubstep-edm-halftime, reggaeton-fuego-lento) always have octave errors regardless of any parameter combination.

**Key findings from sweeps:**
- `fwdTransSigma`: Literature was right — our default 3.0 was 5x too loose. Tightening to 0.6 reduced octave errors from 13 to 7.
- `fwdBayesBias`: NOT just a workaround for loose sigma — essential even with tight sigma. Without it, forward filter reverts to full half-time bias.
- `fwdAsymmetry`: Sweet spot at 0.8-1.6. Default 2.0 was close. Above 3.0, pushes tracks to double-time.
- `fwdFilterFloor`: Despite being 10-100x higher than literature values, has NO measurable effect. Our observation model formula differs enough that floor is irrelevant.

**Status: Forward filter remains OFF as default.** Use `fwdphase=1` for phase-only tracking if smoother phase is desired (BPM-neutral).

**Technique B — Rhythmic Pattern Templates (DETAILED):**

Pre-compute 2-3 EDM bar templates as onset distributions across one bar (32 time slots). Compare observed ODF pattern (accumulated over 4-8 beats) against templates at each candidate tempo. The tempo where the observed pattern best matches a template is preferred.

**Templates:**
- Template 1: Standard 4/4 kick (emphasis on 1,3)
- Template 2: Four-on-the-floor (equal kicks on all 4 beats)
- Template 3: Sparse/breakbeat (kick on 1, snare on 3)

**Why this helps octave ambiguity:** At 96 BPM, the kick pattern fills a template one way. At 128 BPM (3:2), the same audio misaligns with all templates. Template correlation provides metrical-level evidence that ACF/comb cannot.

**Technique C — Beat Critic Subbeat Alternation (DETAILED):**

Divide each beat period into 16 bins, measure ODF energy in each (phase histogram). Compute alternation measure: ratio of even-bin to odd-bin energy. Strong alternation at period T but weak at T/2 indicates double-time tracking. Different discriminative signal from existing `checkOctaveAlternative` (CBSS score comparison) and `metricalcheck` (beat/midpoint ratio).

Split into 2-4 spectral subbands for the histogram (kick energy vs hi-hat energy provide independent evidence).

**Eliminated by research/testing:**
- ~~Widen Rayleigh prior~~ — weak lever, doesn't overcome comb filter harmonics
- ~~Stronger density penalty for 3:2~~ — fold32/sesquicheck tested in v44, no net benefit
- ~~Lower cbssTightness (8→5)~~ — tested in v40, wrong for noisy mic audio
- ~~PLP phase extraction~~ — tested in v42, OSS too noisy for analytical phase
- ~~Bidirectional onset snap~~ — implemented in v44 as bisnap (+0.005 F1, near theoretical limit)
- ~~Bernoulli observation model HMM~~ — tested in v46, all approaches regress vs CBSS. Binary onset/no-onset observation requires onset at every beat; fails on noisy mic audio. **NOTE: The continuous ODF observation model (technique A above) is fundamentally different and has NOT been tested.**
- ~~Phase-only Bernoulli tracker~~ — tested in v46, argmax (F1=0.241), countdown (F1=0.265), deterministic counter (F1=0.195). All worse than CBSS (F1=0.366)
- ~~Multi-agent beat tracking~~ — tested in v48, -4% on full 18-track validation. Strong on kick-prominent tracks but weak on sparse/ambient
- ~~Anti-harmonic comb (percivalw3)~~ — tested in v48, marginal BPM improvement but no F1 improvement
- ~~Metrical contrast check (metricalcheck)~~ — tested in v48, negative on full 18-track validation
- ~~Signal chain mitigation (bfprewhiten, whitenbassbypass)~~ — tested in v47, BandFlux self-normalizes. Signal chain is NOT the F1 bottleneck
- ~~Complex Spectral Difference for rhythm ODF~~ — eliminated by research. Phase too noisy via microphone in reverberant room (Dixon 2006)

**Key references:**
- Krebs, Böck & Widmer 2013 — Rhythmic Pattern Modeling for Beat and Downbeat Tracking (ISMIR)
- Krebs, Böck & Widmer 2015 — Efficient State-Space Model for Joint Tempo and Meter Tracking (ISMIR)
- Heydari et al. 2022 — Novel 1D State Space for Efficient Music Rhythmic Analysis (ICASSP)
- Davies & Plumbley 2007 — Context-Dependent Beat Tracking (IEEE TASLP)
- Davies 2010 — Beat Critic: Beat Tracking Octave Error Identification (ISMIR)
- Kim et al. 2007 — On-Line Musical Beat Tracking with PLL Technique
- Percival & Tzanetakis 2014 — Streamlined Tempo Estimation (IEEE/ACM TASLP)
- Heydari et al. 2024 — BeatNet+: Real-Time Rhythm Analysis for Diverse Music Audio (TISMIR)
- Meier, Chiu & Müller 2024 — Real-Time PLP Beat Tracking (TISMIR)

**Pre-Bayesian baseline (sequential override chain, Feb 21):** avg Beat F1 **0.472** on 9 tracks.
**Bayesian v20 (all observations on, cbssthresh=0.4, Feb 24):** avg Beat F1 **0.421**.
**Bayesian v21 (comb-only, cbssthresh=1.0, Feb 25):** avg Beat F1 **0.590** (best independent cbssthresh sweep).
**Combined validation (comb+ACF 0.3, cbssthresh=1.0, Feb 25):** avg Beat F1 **0.519** (4-device validated).

**Why v21 independent sweep was misleading:** Each parameter was swept independently against device-saved defaults (which had bayesacf=1). The cbssthresh=1.0 result (F1 0.590) was achieved WITH ACF at weight 1.0, not 0. When all v21 changes were applied together (ACF=0), half-time lock occurred on most tracks (avg F1 0.410). A 4-device bayesacf sweep with v21 base params found 0.3 optimal.

**Bayesian tunable parameters (9 total, SETTINGS_VERSION 41 defaults):**

| Param | Serial cmd | Default | Controls |
|-------|-----------|---------|----------|
| Lambda | `bayeslambda` | 0.07 | Transition tightness (tightened v25 to prevent octave jumps) |
| Prior center | `bayesprior` | 128 | Static prior Gaussian center BPM |
| Prior width | `priorwidth` | 50 | Static prior Gaussian sigma |
| Prior weight | `bayespriorw` | 0.0 | Ongoing static prior strength (off by default) |
| ACF weight | `bayesacf` | 0.8 | Harmonic-enhanced ACF (4-harmonic comb + Rayleigh prior, v25) |
| FT weight | `bayesft` | **0.0** | Fourier tempogram — **disabled v28** (no reference system uses FT for real-time) |
| Comb weight | `bayescomb` | 0.7 | Comb filter bank observation weight |
| IOI weight | `bayesioi` | **0.0** | IOI histogram — **disabled v28** (no reference system uses IOI for polyphonic) |
| CBSS threshold | `cbssthresh` | 1.0 | Adaptive beat threshold (CBSS > factor * mean, 0=off) |

**4-device validation results (Feb 25, bayesacf sweep with v21 base params):**

| Track | v20 baseline | acf=0 | acf=0.3 | acf=0.7 | acf=1.0 |
|-------|:-----------:|:-----:|:-------:|:-------:|:-------:|
| trance-party | 0.775 | 0.583 | 0.596 | 0.326 | 0.409 |
| minimal-01 | 0.695 | 0.467 | **0.679** | 0.540 | 0.557 |
| infected-vibes | 0.691 | 0.563 | **0.764** | 0.574 | 0.615 |
| goa-mantra | 0.605 | 0.596 | **0.769** | 0.540 | 0.318 |
| minimal-emotion | 0.486 | 0.484 | **0.678** | 0.588 | 0.571 |
| deep-ambience | 0.404 | 0.437 | **0.636** | 0.489 | 0.263 |
| machine-drum | 0.224 | 0.000 | 0.032 | 0.000 | 0.000 |
| trap-electro | 0.190 | 0.159 | 0.065 | 0.269 | 0.242 |
| dub-groove | 0.176 | 0.405 | **0.452** | 0.118 | 0.031 |
| **Average** | **0.472** | **0.410** | **0.519** | **0.383** | **0.334** |

#### FT and IOI Observation History (Reference Implementation Comparison)

**Originally disabled (v21-22, Feb 25):** Comparison with BTrack, madmom, and librosa identified normalization issues.

**Re-enabled (v24, Feb 26):** Spectral compressor + whitening (v23) fixed the normalization problems. Both re-enabled at weight 2.0 with +49% avg Beat F1 vs control.

**Disabled again (v28, Feb 27):** No reference implementation (BTrack, madmom, librosa) uses Fourier tempogram or IOI histograms for real-time beat tracking. The +49% improvement attributed to v24 re-enablement was likely confounded by simultaneous spectral processing changes. FT produces near-flat observation vectors; IOI has O(n²) complexity and unnormalized counts. Both disabled by default (`bayesft=0`, `bayesioi=0`).

**ACF (bayesacf=0.8, v25):** Harmonic-enhanced ACF with 4-harmonic comb summation and Rayleigh tempo prior. BTrack-style approach: for each candidate period T, sum ACF at 1T, 2T, 3T, 4T with spread windows — fundamental gets 4x advantage over sub-harmonics. Rayleigh weighting peaked at ~120 BPM.

**Current fusion (v41):** `posterior = prediction × combObs × acfObs`. Two well-understood signals (comb filter bank + harmonic-enhanced ACF). BTrack uses a similar approach (ACF → comb → Rayleigh).

**Known limitations:**
- DnB half-time detection — both librosa and firmware detect ~117 BPM instead of ~170 BPM. Acceptable for visual purposes
- trap/syncopated low F1 — energy-reactive mode is the correct visual response
- deep-ambience low F1 — organic mode fallback is correct for ambient content

**Ongoing static prior — tested and disabled (Feb 23):**
Static Gaussian prior (centered at `bayesprior`, sigma `priorwidth`) multiplied into posterior each frame. Helps tracks near 128 BPM (minimal-01: 69.8→97.3 BPM) but actively hurts tracks far from center (trap-electro: 112→131 BPM, machine-drum: 144→124 BPM). Fundamental limitation — can't distinguish correct off-center tempo from sub-harmonic. Per-sample ACF harmonic disambiguation is the proper fix.

**What NOT to do (tested and rejected):**
- PLP phase extraction (plpphase): OSS too noisy for Fourier angle extraction, redundant with onset snap (v42)
- Phase correction (phasecorr): Destroys BPM on syncopated tracks
- ODF width > 5: Variable delay destroys beatoffset calibration
- Ensemble transient input to CBSS: Only works for 4-on-the-floor
- Ongoing static prior as sole sub-harmonic fix: Helps near 128 BPM, hurts far from it (Feb 23)
- Posterior-based harmonic disambig: Posterior already sub-harmonic dominated, check never triggers (Feb 24)
- Soft bin-level ACF penalty: Helped trance-party (0.856) but destroyed goa-mantra (0.246), collateral damage (Feb 24)
- HPS (both ratio-based and additive): Penalizes correct peaks or boosts wrong sub-harmonics (Feb 22). Note: **log-domain HPS** (geometric mean) not yet tested — may avoid the zero-product problem.
- Pulse train cross-correlation: Sub-harmonics produce similar onset alignment (Feb 22)
- Lower cbssTightness (8→5): Tested v40. BTrack's value assumes clean line-in; noisy mic audio needs higher tightness. Adaptive tightness is the correct reformulation (Mar 2026 research)
- Widen/flatten Rayleigh prior alone: Insufficient — doesn't overcome comb filter harmonic resonance (v44 testing + Mar 2026 research)
- fold32 (3:2 octave folding): -0.009 avg F1, 1/18 wins (v44)
- sesquicheck (3:2 shadow CBSS check): No benefit alongside fold32 (v44)
- harmonicsesqui (3:2 transition shortcuts): Catastrophic on 130+ BPM tracks (v44)
- v37 HMM attempt: Failed due to too-few bins (20) and wrong observation model (flat across all states). (Mar 2026 research)
- v46 HMM phase tracker with **Bernoulli obs model** (3 approaches): Bernoulli argmax wrap (F1=0.241), countdown (F1=0.265), deterministic counter (F1=0.195). All worse than CBSS (F1=0.366). Root cause: **Bernoulli observation model** (binary onset/no-onset) requires onset at every beat position. **NOTE:** This does NOT rule out probabilistic phase tracking with a **continuous ODF observation model** (madmom-style), which is fundamentally different and has not been tested. See untried technique A.
- Sequential override chain: Features interact negatively, combined avg F1 dropped 0.472→0.381 (Feb 23)
- Raw ACF observation in Bayesian: Sub-harmonic bias without inverse-lag normalization (Feb 25) — **fixed**: inverse-lag normalization added, then harmonic comb + Rayleigh prior added (v25), ACF weight raised to 0.8
- Fourier tempogram at full weight without spectral processing: Mean normalization destroys discriminability (Feb 25) — **fixed**: spectral compressor+whitening (v23) enabled re-activation at weight 2.0 (v24)
- IOI histogram at full weight without spectral processing: Unnormalized counts dominate posterior (Feb 25) — **fixed**: spectral whitening stabilized onset detection, re-enabled at weight 2.0 (v24)

### Priority 2: BandWeightedFluxDetector — COMPLETE (Feb 21, 2026)

**Status:** Default detector. All parameters confirmed near-optimal. beatoffset recalibrated.

**Algorithm:** Log-compress FFT magnitudes (`log(1 + 20 * mag[k])`), 3-bin max-filter (SuperFlux vibrato suppression), band-weighted half-wave rectified flux (bass 2.0x, mid 1.5x, high 0.1x), additive threshold (`mean + delta`), asymmetric threshold update, hi-hat rejection gate.

**Parameter sweep results (Feb 21):** All defaults confirmed optimal:
- gamma: 10 (-0.06), 20 (baseline), 30 (-0.01) — 20 is sweet spot
- bassWeight: 1.5 (worse), 2.0 (baseline), 3.0 (-0.02) — 2.0 is sweet spot
- threshold: 0.3 (-0.03), 0.5 (baseline), 0.7 (-0.15) — 0.5 is sweet spot

**Beat tracking with beatoffset=5 (9 tracks, Feb 21):**

| Metric | Value |
|--------|:-----:|
| Avg Beat F1 | 0.452 |
| Best track | minimal-01 (0.704) |
| Avg BPM accuracy | 0.942 |
| Avg transient F1 | 0.440 |

### Priority 2b: False Positive Results — COMPLETE (Feb 21, 2026)

BandFlux synthetic pattern evaluation completed. Major improvement on lead-melody, regression on pad-rejection:

| Pattern | BandFlux F1 | Old (HFC+D) F1 | Delta |
|---------|:-:|:-:|:-:|
| lead-melody | 0.785 | 0.286 | **+0.499** |
| bass-line | 0.891 | 0.630 | +0.261 |
| chord-rejection | 0.727 | 0.698 | +0.029 |
| synth-stabs | 0.600 | 1.000 | -0.400 |
| pad-rejection | 0.314 | 0.696 | **-0.382** |

### Priority 3: Microphone Sensitivity — RESOLVED (Feb 2026)

**Problem:** Raw ADC level is 0.01-0.02 at maximum hardware gain (80 = +20 dB) with music playing from speakers. This is ~60 dB SPL at the mic — conversation level, not music level.

**Solution: Spectral compressor + per-bin adaptive whitening (v23+, SETTINGS_VERSION 23-24)**

Instead of the originally proposed raw-sample pre-amplification, a spectral-domain approach was implemented that addresses the root cause more effectively:

1. **Soft-knee compressor** (Giannoulis/Massberg/Reiss 2012) — Frame-level RMS-based compression applied to FFT magnitudes. Threshold -30dB, ratio 3:1, soft knee 15dB, **6dB makeup gain**, 1ms attack, 2s release. Boosts quiet signals more than loud ones, normalizing gross signal level differences caused by mic placement and room acoustics.

2. **Per-bin adaptive whitening** (Stowell & Plumbley 2007) — Each FFT bin normalized by its running maximum (decay 0.997, ~5s memory). Makes change-based detectors (BandFlux) invariant to sustained spectral content regardless of absolute signal level.

**Why spectral-domain beats raw pre-amp:**
- Raw pre-amp (multiplying int16 samples by 2-4x) amplifies noise equally with signal — no SNR improvement
- Spectral compressor applies frequency-selective gain based on frame energy — quiet frames get more boost
- Per-bin whitening auto-scales each frequency band independently — works at any mic level
- Makeup gain (+6dB) provides a fixed boost in the spectral domain where it matters for detection

**Impact:** Spectral processing fixed FT and IOI normalization issues, enabling their re-activation at weight 2.0 in v24 (avg Beat F1 +49% vs control). The pipeline also improved BandFlux onset detection by normalizing spectral dynamics across different acoustic environments.

**Hardware findings (for reference):**
- Mic: MSM261D3526H1CPM, -26 dBFS sensitivity, 64 dB SNR — industry standard
- PDM clock: nRF52840 uses 1.28 MHz; mic's optimal is 2.4 MHz. Below optimal but within spec. May cost 1-2 dB SNR.
- Hardware gain: 0-80 range (±20 dB in 0.5 dB steps), firmware uses full range with AGC
- Physical mic placement remains the single highest-impact variable for raw signal level

**Spectral pipeline parameters (10 total, all tunable via serial):**

| Param | Serial cmd | Default | Controls |
|-------|-----------|---------|----------|
| Whitening enabled | `whitenenabled` | true | Per-bin adaptive whitening |
| Whitening decay | `whitendecay` | 0.997 | Running max decay (~5s memory) |
| Whitening floor | `whitenfloor` | 0.001 | Noise floor for normalization |
| Compressor enabled | `compenabled` | true | Soft-knee compressor |
| Threshold | `compthresh` | -30.0 dB | Compression onset level |
| Ratio | `compratio` | 3.0 | Compression ratio (3:1) |
| Knee | `compknee` | 15.0 dB | Soft knee width |
| Makeup gain | `compmakeup` | 6.0 dB | Post-compression gain boost |
| Attack | `compattack` | 0.001s | Attack time constant |
| Release | `comprelease` | 2.0s | Release time constant |

### Priority 4: Startup Latency — IMPLEMENTED (Feb 22, 2026)

**Was:** AudioController required ~3s (180 samples @ 60Hz) before first autocorrelation.
**Now:** Progressive startup — autocorrelation begins after 1s (60 samples). The existing `maxLag = ossCount_ / 2` clamp naturally limits detectable tempo range during warmup:
- At 1s (60 samples): minimum detectable BPM is ~120 (maxLag=30); upper bound is bpmMax (200)
- At 2s (120 samples): full 60-200 BPM range available
- `periodicityStrength_` smoothing (0.7/0.3 EMA) handles early estimate noise

Band autocorrelation (adaptive weighting) also lowered from 120 to 60 minimum samples.

### Priority 5: Music Content Classification (Long-term)

The existing `rhythmStrength` blend works well but could be enhanced with additional content descriptors. Research (Feb 22) identified three cheap features that would improve organic/music mode transitions:

1. **Onset density** — ✅ Implemented (Feb 2026). Windowed counter with EMA smoothing, exposed as `AudioControl::onsetDensity` and `"od"` in streaming JSON. Modulates `rhythmStrength` by ±0.1 centered at 3 onsets/s.
2. **Spectral centroid variability** — variance of spectral centroid over 2-4s window. High variance=dynamic/percussive, low=sustained/ambient. Already have centroid computation.
3. **Energy crest factor** — peak/mean energy ratio over 2-4s. High=percussive with quiet periods, low=continuous drone.

These would modulate the existing `rhythmStrength` for smoother, more appropriate visual responses without hard mode switching.

### Priority 6: Tempo Estimation — Bayesian Fusion (Feb 23, 2026)

**Architecture:** Replaced sequential override chain with Bayesian posterior estimation. All tempo signals (autocorrelation, Fourier tempogram, comb filter bank, IOI histogram) contribute per-bin observation likelihoods multiplied together with a Viterbi transition prior. MAP estimate with quadratic interpolation becomes the tempo. Post-posterior harmonic disambiguation checks 2x and 1.5x BPM bins.

**What the Bayesian refactor eliminated:**
- Sequential override chain (~400 lines of cascading cross-validation)
- HPS code (ratio-based and additive, both rejected Feb 22)
- Pulse train cross-correlation (Percival 2014, rejected Feb 22)
- 17 old tuning parameters (harmonic thresholds, cross-validation thresholds, etc.)
- Negative feature interactions (combined features scored 0.381, worse than 0.472 baseline)

**What it preserved:**
- Autocorrelation computation (core signal, unchanged)
- CBSS beat tracking (receives tempo from Bayesian state)
- Comb filter bank, Fourier tempogram, IOI histogram (now per-bin observations)
- ODF smoothing, beat timing offset (unchanged)

#### 6a. ODF Mean Subtraction — REVERSED (v32)

**Feb 24 (pre-whitening):** Turning OFF caused major regressions (minimal-01: 0.610→0.266). ODF mean subtraction was essential for ACF discriminability.

**Feb 28 (post-whitening, v32):** Turning OFF now gives +70% F1. Per-bin spectral whitening (v23+) removes the DC bias that previously corrupted the ACF. Raw ODF preserves natural ACF peak structure. **Disabled by default (`odfmeansub=0`).**

#### 6b. CBSS Adaptive Threshold — TUNED (Feb 25, 2026)

**Status:** Implemented as `cbssThresholdFactor` (default **1.0**, raised from 0.4 in SETTINGS_VERSION 21). Beat fires only if `CBSS > factor * cbssMean_` where cbssMean_ is an EMA with tau ~120 frames (~2s). Setting to 0 disables the threshold (countdown-only, old behavior).

**Impact:** Multi-device sweep (Feb 25) found thresh=1.0 is optimal (F1 0.590 vs 0.209 at 0.4). Higher threshold = fewer phantom beats during low-energy sections = more stable BPM tracking = paradoxically better recall.

#### 6c. Lightweight Particle Filter — Future Alternative (Research, Feb 22)

Inspired by BeatNet (Heydari et al., ISMIR 2021). 100-200 particles tracking (beat_period, beat_position). Octave investigator injects particles at 2x/0.5x median tempo at resampling. Could replace or complement Bayesian fusion if it proves insufficient.

- **CPU:** ~1% (100 particles × weight update per frame + periodic resampling)
- **Memory:** ~2KB
- **Complexity:** ~100-150 lines C++

#### 6d. Multi-Agent Beat Tracking — Future Alternative (Research, Feb 22)

5-10 competing tempo/phase agents. Each scores onset events against predicted beats. Best-scoring agent determines output. Agents at different metrical levels compete naturally.

- **CPU:** <1% | **Memory:** ~500 bytes | **Complexity:** ~150-250 lines

#### Approaches tested and rejected

| Approach | Why Not | Tested |
|----------|---------|--------|
| Sequential override chain | Features interact negatively, combined F1 0.381 < 0.472 baseline | Feb 23 |
| Ongoing static prior (Bayesian) | Helps near 128 BPM, hurts tracks at 112/144 BPM | Feb 23 |
| HPS (ratio-based + additive) | Penalizes correct peaks or boosts wrong sub-harmonics | Feb 22 |
| Pulse train (Percival 2014) | Sub-harmonics produce similar onset alignment | Feb 22 |
| Comb bank cross-validation | Both ACF and comb lock to same wrong tempo | Feb 22 |
| IOI bidirectional override | Repackages bad transient data | Feb 22 |
| ODF mean subtraction OFF | Destroys BPM on sparse tracks (minimal-01 BPM→89) | Feb 24 |
| Per-band thresholds ON | Helps strong tracks, destroys weak ones (avg 0.421→0.354) | Feb 24 |
| Multi-frame diffframes=2 | Too many transients, hurts phase (avg -0.098) | Feb 24 |
| Deep learning/CNN/Transformer | Not feasible on nRF52840 (64 MHz, no matrix acceleration) | Research |

### Priority 7: Onset Detection — Gap Analysis vs State of the Art (Feb 27, 2026)

Comprehensive comparison against SuperFlux, BTrack (ComplexSpectralDifferenceHWR), madmom, aubio, and CNN-based detectors. Our BandWeightedFlux detector is well-aligned with established best practices but has specific gaps in peak picking, frequency resolution, and ODF design.

#### Onset Detection Performance Context

| Method | F1 (50ms) | F1 (25ms) | Type | Embedded |
|--------|-----------|-----------|------|----------|
| CNN ensemble (Schlüter 2014) | ~0.90 | ~0.87 | Offline neural | No |
| SuperFlux (Böck 2013) | ~0.85-0.88 | ~0.84-0.85 | Offline DSP | Yes |
| ComplexFlux | ~0.86-0.89 | — | Offline DSP | Yes |
| Complex domain (Duxbury 2003) | ~0.78-0.82 | — | DSP | Yes |
| Standard spectral flux | ~0.80-0.83 | ~0.78-0.80 | DSP | Yes |
| **BandWeightedFlux (ours)** | **~0.44** | — | **Real-time DSP** | **Yes** |

Our lower scores vs literature benchmarks are primarily due to: (1) room acoustics via microphone vs studio recordings, (2) 16 kHz / FFT-256 vs 44.1 kHz / FFT-2048, (3) single-threshold peak picking without local-max confirmation. Percussion onset detection on clean recordings is considered "solved" (F1 > 0.85) — our challenge is the degraded real-world acoustic environment.

#### What BandWeightedFlux Does Right (Validated by Literature)

1. **Log-compressed spectral flux** — Standard in SuperFlux, madmom, librosa. Any compression improves F1 by 5-10+ points.
2. **Per-bin adaptive whitening** (Stowell & Plumbley 2007) — Exactly the aubio algorithm. 10+ F1 point improvement in literature.
3. **Soft-knee spectral compressor** (Giannoulis 2012) — More sophisticated than most onset detectors (typically just log compression).
4. **SuperFlux 3-bin max filter** — Reduces false positives up to 60% on vibrato-heavy material (Böck & Widmer 2013).
5. **Half-wave rectified spectral flux** — Standard in all top non-neural systems.
6. **Additive threshold** (`mean + delta`) — BTrack, SuperFlux, and madmom all use additive/subtractive thresholds.
7. **Asymmetric threshold update** (skip update on detection frames) — Prevents loud onsets from inflating noise floor. Novel but sound engineering.
8. **Band weighting** (bass=2.0, mid=1.5, high=0.1) — Aligned with literature on drum-focused onset detection (Scheirer 1998).
9. **Onset delta filter** (minOnsetDelta=0.3) — Addresses the "gradual onset" failure mode identified in the literature as a key challenge.

#### 7a. Per-Band Independent Thresholds — TESTED, KEEP OFF (Feb 24, 2026)

Independent adaptive thresholds per band (bass/mid/high). Detection fires if ANY band exceeds its own threshold × multiplier. **Disabled by default** (`bfperbandthresh=0`).

- **Full 9-track regression:** avg F1 0.421→0.354 (**-0.067**) — major regressions on quiet/sparse tracks
- **Verdict:** Keep disabled. Literature supports this finding — splitting detection by band without per-band calibration increases false positives.

#### 7b. Multi-Frame Temporal Reference — TESTED, KEEP AT 1 (Feb 24, 2026)

Configurable `diffframes` (1-3). Default remains 1. diffframes=2 generates too many transients (avg -0.098 F1).

#### 7c. Dual-Threshold Peak Picking — ✅ DONE (v28)

**The biggest gap in our onset detection.** SuperFlux, madmom, and librosa all use dual-threshold peak picking requiring BOTH:
1. ODF sample is a **local maximum** within a window
2. ODF sample **exceeds local mean + delta**

We use threshold-only (`combinedFlux > averageFlux + threshold`). No local maximum check. This means:
- We fire on the **rising edge** of flux peaks rather than the true peak (imprecise timing)
- Consecutive frames above threshold all fire (suppressed only by cooldown, not peak detection)
- The cooldown does double duty: rate-limiting AND peak-selecting

**SuperFlux default parameters:**
```
pre_max=10ms, post_max=50ms   // local max window (post_max provides look-ahead)
pre_avg=150ms, post_avg=0ms   // local mean window (causal)
combine=30ms                   // minimum inter-onset interval
delta=1.1                      // threshold above local mean
```

**Causal adaptation for our system:**
- Local max: `ODF[t] >= ODF[t-1] && ODF[t] >= ODF[t+1]` with 1-frame look-ahead (16ms)
- Local mean: existing `averageFlux` EMA (causal, no look-ahead needed)
- Combine: existing adaptive cooldown
- Even 1-frame look-ahead (16ms) is imperceptible for visualization

**Implementation:** Buffer 1-2 frames of ODF output. Only report detection when the buffered frame is confirmed as a local maximum. The current detection fires at frame N; the new logic would fire at frame N+1 after confirming N is a peak.

- **Effort:** Low (~40 lines). 1-2 frame ring buffer + local max check before emitting detection.
- **Expected impact:** Medium-High. Literature shows dual-threshold adds ~2-5% F1 over threshold-only. Improves timing precision of every detection.
- **References:** SuperFlux (Böck & Widmer 2013), madmom `peak_picking`, librosa `peak_pick`

#### 7d. Hi-Res Bass via Goertzel — ✅ TESTED, KEEP OFF (v32, -9% F1)

The hi-res bass path (`hiResBassEnabled`) is already coded in BandWeightedFluxDetector. It uses 512-sample Goertzel for 12 bass bins at 31.25 Hz/bin (vs 6 FFT bins at 62.5 Hz/bin). This doubles bass frequency resolution, giving 2-4 bins for kick drum fundamental (40-80 Hz) vs 1-2 bins currently.

At FFT-256/16kHz, kick drum fundamental (40-80 Hz) and bass guitar (80-250 Hz) can share the same 1-2 bins. The kick's attack is masked by the bass's sustain. Hi-res bass separates them.

- **Effort:** Trivial (set `hiResBassEnabled=true`, run 9-track sweep)
- **Expected impact:** Medium for kick-specific detection
- **References:** Multi-resolution spectral flux (Bello 2005), Böck CNN multi-scale input (ICASSP 2014)

#### 7e. Complex Spectral Difference for Rhythm ODF — ELIMINATED (Mar 2026 research)

BTrack's default ODF uses both magnitude AND phase (ComplexSpectralDifferenceHWR). Phase is extremely sensitive to noise — through a microphone in a reverberant room, phase coherence degrades rapidly. Dixon (2006) found CSD only slightly outperforms spectral flux on clean recordings and performs **worse** on noisy signals. Not worth implementing for our mic-in-room signal path.

#### 7f. Log-Spaced Sub-Band Filterbank — MEDIUM PRIORITY

SuperFlux uses 24 bands/octave (~216 total). madmom CNN uses 80 mel bands. We use 3 bands (bass/mid/high) with raw FFT bins averaged per band. This is very coarse — a kick at 60 Hz and a bass note at 300 Hz share the same "bass" band.

With 128 FFT bins at 62.5 Hz/bin, we could create 12-24 log-spaced bands. At low frequencies, each band maps to 1-2 FFT bins (limited by resolution); at high frequencies, bands span many bins. The finer grouping would:
- Separate kick fundamental from bass guitar
- Separate snare crack from vocal energy
- Give per-band flux more discriminative power

**However:** Our 3-band approach works well for the specific kick/snare visual use case. The coarse grouping is a deliberate simplification. The literature filterbanks are for general-purpose onset detection across all instrument types.

- **Effort:** Medium (~80 lines, new band definition table + per-band flux loop)
- **Expected impact:** Low-Medium for kick/snare use case. Higher for general onset detection.
- **Risk:** Requires per-band threshold calibration. May interact with existing `bassWeight/midWeight/highWeight`.

#### 7g. Knowledge-Distilled TinyML Onset Detector — LONG TERM

Train CNN on desktop with labeled EDM onset data, distill to tiny student model (~1K-5K params), quantize to INT8, deploy via TensorFlow Lite Micro. The only approach that can learn complex spectral patterns (kicks vs bass vs pads vs room modes).

Performance gap between DSP and neural onset detection: ~10-15 F1 points on standard benchmarks. CNN F1 ~0.90 vs SuperFlux ~0.85-0.88. This gap is the performance ceiling that DSP improvements cannot cross.

- **Memory:** ~5-15KB (model + activations)
- **CPU:** ~200-500us/frame (with CMSIS-NN on Cortex-M4F)
- **Flash:** ~5-20KB model weights
- **Risk:** High — requires training data, model development, TFLM integration
- **References:** Böck & Schlüter (ICASSP 2014), efficient CNN (81K params, 2018), TinyML distillation (Nature 2025)

#### Onset Detection: Approaches Tested and Rejected

| Approach | Why Not | Tested |
|----------|---------|--------|
| Per-band independent thresholds | Increases FPs on sparse tracks (-0.067 avg F1) | Feb 24 |
| Multi-frame diffframes=2 | Too many transients, hurts phase (-0.098 avg F1) | Feb 24 |
| Post-onset decay confirmation | Adds latency, rejects synth stabs | Feb 22 |
| Band-dominance gate | Redundant with high-weight suppression | Feb 22 |
| Spectral crest factor gate | Kills kicks through room resonances | Feb 22 |
| CNN/RNN on nRF52840 | Not feasible (64 MHz, no matrix acceleration) | Research |
| Complex domain for visual transients | Phase too noisy via microphone in room | Research |
| Overlapping FFT windows (125 fps) | Doubles FFT CPU cost for marginal timing gain | Research |

#### Onset Detection: What NOT to Change (Validated)

| Feature | Rationale |
|---------|-----------|
| BandFlux Solo | Single detector outperforms ensemble (+14% Beat F1). Literature confirms cleaner signal > multi-detector voting |
| Additive threshold | Correct for low-signal environments. Used by BTrack, SuperFlux, madmom |
| High-band suppression (0.1) | Correct for kick/snare visual use case |
| Onset delta filter (0.3) | Valid solution for gradual onset rejection |
| Asymmetric threshold update | Prevents onset self-inflation. Sound engineering |
| Adaptive cooldown | Maps to SuperFlux's `combine` parameter. Tempo-awareness correct for visualization |
| Log compression (gamma=20) | Aggressive but appropriate for low-SNR mic input |
| Per-bin whitening (decay=0.997) | Faster than aubio default (250s) but appropriate for live music |

---

## Calibration Status (March 3, 2026)

**SETTINGS_VERSION 45.** Bayesian tempo fusion (Comb+ACF) + CBSS beat tracking. v43 algorithmic fixes: removed double inverse-lag normalization, full-resolution comb-on-ACF, octave folding, lag-space Gaussian transition matrix. Frame rate 66 Hz. FT+IOI disabled. PLP disabled (no effect). v44: bidirectional onset snap. v45: Percival harmonic pre-enhancement, PLL phase correction, adaptive CBSS tightness.

**Current performance (v43, 18-track validation on 3 bare boards):**
- 3-device avg Beat F1: **0.284**
- Best-device avg Beat F1: **0.355**
- BPM accuracy: **87.7%** (3-device avg, up from 33% pre-fix)
- **Primary bottleneck: phase alignment** (correct BPM does not translate to correct beat placement)
- **Secondary: ~128 BPM gravity well** (slow tracks lock to 3:2 harmonic)
- Run-to-run variance is large (std=0.04-0.23 per track). Single-run validations cannot detect improvements < ~0.15 F1.

**Multi-device sweep capability:** 4 devices sweep parameters in parallel (4x speedup). Uses real music files with ground truth annotations, ffplay for headless audio playback.

| Feature | Parameters | Status |
|---------|:----------:|--------|
| **Spectral pipeline** | compenabled, compthresh=-30, compratio=3, compknee=15, compmakeup=6, whitenenabled, whitendecay=0.997 | **Validated** (v23-24) |
| **Bayesian weights** | bayesacf=0.8, bayesft=0.0, bayescomb=0.7, bayesioi=0.0, bayeslambda=0.07, bayespriorw=0 | **Validated** (v28) — FT+IOI disabled, Comb+ACF only |
| **CBSS parameters** | cbssthresh=1.0, cbssTightness=8.0, cbssAlpha=0.9, onsetsnap=8 | **Validated** (v40-41) |
| **Octave disambiguation** | odfmeansub=off, densityoctave=1, octavecheck=1 | **Validated** (v32) |
| **Beat-boundary tempo** | beatboundary=1 | **Enabled** (v28) |
| **Unified ODF** | unifiedodf=1 | **Enabled** (v28) |
| **Dual-threshold peak picking** | bandflux_peakpick=1 | **Enabled** (v28) |
| **Particle filter** | pfEnabled=1, pfNoise=0.08, pfObsLambda=8, pfInfoGate=0.10 | **Enabled** (v38-39) |
| **Frame rate** | OSS_FRAME_RATE=66 | **Fixed** (v39) |
| PLP phase extraction | plpphase=off, plpstrength=0.5, plpminconf=0.3 | **Tested, keep OFF** — no measurable effect, redundant with onset snap (v42) |
| Downward harmonic correction | downwardcorrect=off | **Tested, keep OFF** — overcorrects mid-tempo (v41) |
| Adaptive ODF Threshold | adaptodf=off | **Tested, keep OFF** (v32) |
| Hi-Res Bass | bfhiresbass=off | **Tested, keep OFF** — -9% F1 (v32) |
| Per-band thresholds | bfperbandthresh | **Tested, keep OFF** — -0.067 avg F1 (Feb 24) |
| Multi-frame diffframes | bfdiffframes=1 | **Tested, keep at 1** — -0.098 avg F1 (Feb 24) |
| BandFlux core params | gamma=20, bassWeight=2.0, threshold=0.5, onsetDelta=0.3 | **Calibrated** (Feb 21) |

## State-of-the-Art Gap Analysis (March 2, 2026)

Current avg Beat F1: **0.28** (4-device avg, 18 tracks, v41; best-device avg 0.35). BTrack (nearest comparable DSP-only system): **~65-75%**. The gap is primarily **phase alignment** — we achieve 82.5% BPM accuracy but F1 doesn't follow. Even with correct BPM, CBSS beat prediction fires at the wrong point in the beat cycle.

### Performance Context

| System | Type | Beat F1 | Hardware |
|--------|------|---------|----------|
| Beat This! (2024) | Offline transformer | ~89% | GPU |
| madmom DBN (offline) | Offline RNN+DBN | ~88% | CPU |
| BeatNet+ (2024) | Online CRNN+PF | ~81% | CPU |
| Real-time PLP (2024) | Online DSP (Fourier) | ~75% | CPU |
| madmom forward (online) | Online RNN+HMM | ~74% | CPU |
| BTrack (online) | Online DSP (CBSS) | ~65-75% | Embedded OK |
| **Blinky v42 (online)** | **Online DSP (CBSS+PF)** | **~28%** | **nRF52840 64MHz** |

Note: BTrack's 65-75% is measured on clean digital audio. On live microphone with room acoustics (our setup), BTrack would likely score ~40-50%. Our 28% F1 is roughly 60-70% of what BTrack would achieve on the same degraded input.

### What We're Doing Right (Validated by Literature)

1. **Continuous ODF → CBSS** — BandFlux pre-threshold feeds CBSS (unified ODF, v28). Matches BTrack architecture.
2. **Adaptive spectral whitening** (Stowell & Plumbley 2007) — Per-bin normalization. 10+ F1 point improvement in literature.
3. **Soft-knee spectral compression** (Giannoulis 2012) — Standard in all top systems.
4. **Inverse-lag ACF normalization** — Corrects sub-harmonic bias. BTrack does the same.
5. **SuperFlux-style max filtering** (Böck & Widmer 2013) — In BandFlux onset detection.
6. **Comb filter bank** (Scheirer 1998) — Best single non-neural tempo estimator.
7. **BTrack-style predict+countdown CBSS** — Standard non-neural real-time beat tracking.
8. **Band-weighted spectral flux** — Emphasizing bass/mid over high is well-supported.
9. **CBSS adaptive threshold** — Prevents phantom beats during silence/breakdowns.
10. **Beat-boundary tempo** — Tempo changes deferred to beat fire, matching BTrack.
11. **Dual-threshold peak picking** — Local-max confirmation, matching SuperFlux/madmom.

### Why Phase Alignment Fails (Diagnosis)

The gap between 28% and BTrack's 65-75% is primarily phase alignment. Specific causes:

1. **ODF quality degradation**: 16 kHz / FFT-256 (62.5 Hz/bin) vs BTrack's typical 44.1 kHz / FFT-512 (86 Hz/bin). Lower spectral resolution means broader, less precise onset peaks. CBSS forward projection argmax has poor resolution on smeared peaks.

2. **CBSS momentum dominance** (tightness=8): Higher than BTrack's 5.0. The narrow log-Gaussian window locks in initial phase errors because CBSS feedback (90% weight via alpha=0.9) strongly reinforces existing phase. Less room for onset contribution (10%) to correct bad phase.

3. **Onset snap is backward-only**: Looks at last N frames. If the actual onset occurs a few frames after countdown expiry, it's missed. BTrack's prediction projects CBSS forward to find future peaks.

4. **Live microphone vs clean audio**: Room reflections, enclosure resonances, and low sample rate degrade ODF peak sharpness. This is the single biggest uncontrollable factor.

5. **No explicit phase tracking**: CBSS derives phase indirectly from a beat counter. Systems achieving >60% F1 either track phase explicitly (HMM state variable) or extract it analytically (Fourier angle). **However**: PLP analytical phase extraction (v42) showed no improvement — onset snap already handles per-beat phase alignment. The remaining F1 gap is dominated by tempo octave errors and run-to-run variance, not systematic phase drift.

---

## Detailed Phase History

### Phase 1: Simplify — Remove Wasteful/Detrimental Features

#### 1a. Test Disabling FT and IOI Observations — ✅ DONE (v28)

**Problem:** FT and IOI have documented algorithmic issues (see root cause analysis in `blinky-test-player/PARAMETER_TUNING_HISTORY.md`). No reference implementation (BTrack, madmom, librosa) uses Fourier tempogram or IOI histograms in real-time beat tracking. The +49% improvement attributed to their v24 re-enablement may be confounded by simultaneous changes (spectral processing, cbssthresh tuning) rather than FT/IOI themselves.

**Evidence against FT:**
- Mean normalization in Goertzel produces near-flat observation vectors (all bins ≈ 1.0)
- Independent sweep found bayesft=0 optimal
- BTrack does not use Fourier tempogram — uses comb filter on ACF instead
- madmom uses RNN activations, not Fourier tempogram, for real-time

**Evidence against IOI:**
- Unnormalized counts (1-10+ range) can dominate multiplicative posterior
- O(n²) complexity with onset count (up to 48×48×20 operations)
- 2x folding biases toward fast tempos
- No reference implementation uses IOI histograms for polyphonic beat tracking

**Test plan:**
1. Set `bayesft=0, bayesioi=0` (disable observations)
2. Run 9-track beat F1 sweep vs current defaults (`bayesft=2.0, bayesioi=2.0`)
3. If F1 is equal or better: make the removal permanent, delete the code
4. If F1 is worse: keep enabled but document which tracks benefit and why

**Effort:** Trivial (parameter change). **Impact:** Removes 2 fragile signals from multiplicative fusion + ~1-2% CPU.

**Result:** Disabled in v28. No regression on 18-track validation. FT and IOI code retained but default weights set to 0.

#### 1b. Simplify Ensemble Infrastructure for Solo Detector — DONE (v59)

**Problem:** EnsembleFusion runs agreement-based confidence scaling, weighted averaging, and multi-detector cooldown logic — all designed for N detectors. With BandFlux Solo (1 detector enabled), this is pure overhead.

**Result:** Added fast path in `EnsembleFusion::fuse()`: when exactly 1 detector is enabled, bypasses agreement scaling and weighted averaging — direct strength pass-through with confidence filtering and cooldown only. Multi-detector path retained for future experimentation.

#### 1c. Evaluate Adaptive Band Weighting Cost/Benefit — OUTSTANDING

**Problem:** ~1600 lines of code for adaptive band weighting (per-band OSS buffers, cross-band correlation, peakiness crest factor, per-band autocorrelation). Consumes 2.9 KB RAM for per-band OSS buffers + ~1% CPU. The conditions for adaptive weights to activate (periodicity > 0.1, avgEffective > 0.15, bandSynchrony > 0.3) may rarely be met, causing the system to fall back to fixed defaults most of the time.

**Test plan:**
1. Set `adaptiveBandWeightEnabled=false`
2. Run 9-track beat F1 sweep vs enabled
3. If F1 is equal: remove the feature (reclaim 2.9 KB RAM + 1% CPU + ~1600 lines)
4. If F1 is worse on specific tracks: document which tracks and evaluate if the complexity is justified

**Effort:** Trivial (parameter toggle). **Impact:** Potentially major simplification.

#### 1d. Remove Dual ODF Computation (Consolidate with Phase 2.4)

**Problem:** Two parallel spectral flux computations on the same data:
- `computeSpectralFluxBands()` in AudioController — raw band-weighted flux for CBSS
- `BandWeightedFluxDetector::computeBandFlux()` — log-compressed flux with thresholding for transients

These run independently on the same spectral magnitudes, producing different views of "what just happened." This is wasteful AND architecturally harmful (see Phase 2.4).

**Action:** Defer to Phase 2.4 (Unify ODF). The fix for waste and the fix for the architectural gap are the same change.

---

### Phase 2: Improve — Close Gaps with State of the Art

Each improvement must be tested and calibrated independently before combining. Use 9-track (or 18-track) beat F1 sweep with 4-device parallel testing.

#### 2.1. Only Update beatPeriodSamples at Beat Boundaries — ✅ DONE (v28)

**Gap:** Tempo changes can happen at arbitrary times (every 250ms during autocorrelation), causing CBSS to use a beat period that changed mid-prediction. BTrack only calls `calculateTempo()` when a beat fires — tempo and beat timing are synchronized.

**What BTrack does:** `calculateTempo()` runs inside the `if (timeToNextBeat == 0)` block. The beat period used by CBSS stays constant between beats.

**What we do:** `runAutocorrelation()` runs every 250ms regardless of beat phase. The resulting BPM immediately updates `beatPeriodSamples_`, which CBSS uses for its log-Gaussian window. A mid-beat tempo change shifts the window while a prediction is in flight.

**Fix:** Continue running autocorrelation every 250ms (the Bayesian posterior needs frequent updates to track changes). But defer applying the new `beatPeriodSamples_` to CBSS until the next beat fires. Store the pending value in `pendingBeatPeriod_` and apply it in `detectBeat()` when `timeToNextBeat <= 0`.

**Test plan:**
1. Implement pending beat period
2. Run 9-track beat F1 sweep before/after
3. Expect improvement on tracks with stable tempo (fewer mid-beat period discontinuities)
4. Watch for regression on tracks with rapid tempo changes (deferred update = slower adaptation)

**Effort:** Low (~20 lines). **Impact:** High — synchronizes tempo and beat tracking.

#### 2.2. Increase Tempo Resolution (20 → 40+ Bins) — ✅ TESTED, REVERTED (v29)

**Gap:** 20 bins over 60-180 BPM = 6 BPM per bin. At 120 BPM, 1 BPM error = 4.2ms per beat — after 10 beats, 42ms cumulative drift. BTrack uses 41 bins (80-160 BPM, 2 BPM steps). madmom uses frame-level resolution (~80 distinct periods at 100fps).

**Fix:** Increase `NUM_TEMPO_BINS` from 20 to 40 (3 BPM steps) or 60 (2 BPM steps).

**Cost analysis:**
- Transition matrix: 40×40×4 = 6.4 KB (vs current 20×20×4 = 1.6 KB). Precomputed once.
- Per-update: O(N²) = 1600 multiplies at 40 bins vs 400 at 20 bins. At 250ms intervals on 64 MHz, negligible.
- Comb filter bank: 40 filters × 60 samples × 4 bytes = 9.6 KB (vs current 4.8 KB). This is the main memory cost.
- Total: ~+8 KB RAM. Within budget (14 KB used of 256 KB available).

**Test plan:**
1. Implement 40 bins, re-run 9-track sweep
2. If better: try 60 bins
3. Check memory usage stays under 25 KB total
4. Re-calibrate Bayesian weights if needed (more bins may change optimal exponents)

**Effort:** Low-Medium (~50 lines, mainly constants and array sizes). **Impact:** High — finer tempo resolution reduces cumulative phase drift.

**Result:** 40 bins tested in v29, caused transition matrix drift 2x worse than 20 bins (BPM-space Gaussian on lag-uniform grid). Reverted to 20 bins.

#### 2.3. Adaptive ODF Threshold Before ACF (BTrack-style) — ✅ TESTED, KEEP OFF (v32)

**Gap:** BTrack applies a sliding window adaptive threshold to the ODF before computing the ACF. This removes slowly-varying energy envelopes (verse/chorus dynamics, crescendos), leaving only impulsive onsets for the ACF to find periodicity in. Our ODF goes into the ACF with only 5-point causal smoothing — the ACF can find periodicity in arrangement-level dynamics rather than beat-level dynamics.

**BTrack algorithm:**
```
For each ODF sample:
    localMean = mean(ODF[i-8 : i+7])  // 16-sample window (pre=8, post=7)
    thresholded[i] = max(0, ODF[i] - localMean)
```

**Adaptation for causal (real-time) operation:** BTrack's threshold uses a post-window (looks 7 samples ahead), which isn't possible in real-time at frame rate. Options:
- Use fully causal window (pre=15, post=0) — trades look-ahead for latency
- Apply threshold only to the OSS buffer retrospectively before ACF — the buffer contains 6s of history, so a centered window is fine for the ACF computation

**Test plan:**
1. Implement local-mean subtraction on OSS buffer before autocorrelation (centered window, applied to buffer not real-time stream)
2. Run 9-track beat F1 sweep before/after
3. Expect improvement on tracks with dynamic energy (verse/chorus, breakdowns)
4. Check periodicity strength stability (should be more consistent)

**Effort:** Low (~30 lines). **Impact:** Medium — cleaner ACF input = more reliable tempo estimation.

**Result:** Tested as `adaptodf=1` in v32. Marginal benefit over raw ODF. Keep off.

#### 2.4. Unify ODF — Feed BandFlux Pre-Threshold Activation to Beat Tracker — ✅ DONE (v28)

**Gap:** The transient detector (BandWeightedFlux) and beat tracker (computeSpectralFluxBands) compute spectral flux independently with different preprocessing. The transient detector has log compression, onset delta filtering, hi-hat rejection — all tuned for visual aesthetics. The beat tracker's ODF has none of these. This means the beat tracker might lock onto energy patterns that the transient detector suppresses (or vice versa).

**What BTrack does:** Single ODF (ComplexSpectralDifferenceHWR) feeds BOTH onset detection (peak picking) and beat tracking (CBSS). One representation, two consumers.

**Fix:** Extract the continuous (pre-threshold) activation value from BandWeightedFlux and use it as the ODF for CBSS. Apply thresholding/cooldown only for the visual pulse output. This:
- Eliminates the duplicate `computeSpectralFluxBands()` computation (~100 lines)
- Unifies what the system "hears" — transient detection and beat tracking see the same signal
- The log compression in BandFlux may actually help the ACF (compresses dynamic range, standard in the literature)
- Keeps BandFlux's vibrato suppression and band weighting for the shared ODF

**Test plan:**
1. Add a `getPreThresholdFlux()` accessor to BandWeightedFluxDetector
2. Replace `computeSpectralFluxBands()` call with pre-threshold BandFlux value
3. Run 9-track beat F1 sweep
4. If BandFlux's log compression hurts ACF: add a configurable compression bypass for the CBSS path
5. Re-calibrate `beatoffset` (timing may shift due to different ODF characteristics)

**Effort:** Medium (~50 lines of plumbing, ~100 lines removed). **Impact:** High — eliminates ODF disagreement between transient detection and beat tracking.

**Result:** Implemented as `unifiedodf=1` in v28. BandFlux pre-threshold activation feeds CBSS via `getPreThresholdFlux()`.

#### 2.5. Simplify Bayesian Fusion — OUTSTANDING (Conditional on phase alignment work)

**Gap:** Multiplicative fusion of 4 independent estimators is fragile. If any estimator produces near-zero for the correct bin, the posterior collapses. BTrack uses a sequential pipeline (ACF → comb filter on ACF → Rayleigh → Viterbi). madmom uses a single observation model into a DBN. No reference system uses multiplicative fusion of 4 independent estimators.

**Options (evaluate after Phase 1a):**

**Option A — Reduce to Comb + ACF only:**
If Phase 1a confirms FT/IOI don't help, simplify to `posterior = prediction × combObs × acfObs`. Two well-understood signals. Removes ~150 lines of FT/IOI observation code.

**Option B — Switch to log-domain additive fusion:**
`logPosterior[i] = w_comb * log(combObs[i]) + w_acf * log(acfObs[i]) + ...`
Numerically more stable. Individual estimators contribute proportionally rather than multiplicatively vetoing. Equivalent to weighted geometric mean.

**Option C — BTrack-style pipeline:**
Apply comb filter to ACF values (not to raw ODF), then Rayleigh weight, then Viterbi-like DP. Each stage refines the previous one's output. More robust because failure in one stage doesn't multiply through.

**Test plan:** Implement the option that aligns with Phase 1a results. 9-track sweep. Compare stability (F1 variance across tracks) not just average.

**Effort:** Medium. **Impact:** Medium — reduces fragile interaction effects.

#### 2.6. Dual-Threshold Peak Picking for BandFlux — ✅ DONE (v28)

**Gap:** The biggest gap in our onset detection. SuperFlux, madmom, and librosa all use dual-threshold peak picking requiring BOTH a local maximum AND exceeding the threshold. We use threshold-only, which fires on rising edges rather than true peaks, with cooldown doing double-duty as rate limiter and peak selector.

**Fix:** Buffer 1-2 frames of ODF output. Only report detection when the buffered frame is confirmed as a local maximum. Add 1-frame look-ahead (16ms latency, imperceptible for visualization).

```
Detection requires ALL of:
1. combinedFlux[t] > averageFlux + threshold        (existing threshold check)
2. combinedFlux[t] >= combinedFlux[t-1]              (rising or peak)
3. combinedFlux[t] >= combinedFlux[t+1]              (confirmed peak, 1-frame delay)
4. Cooldown elapsed                                   (existing adaptive cooldown)
```

**Test plan:**
1. Implement 1-frame look-ahead ring buffer in BandWeightedFluxDetector
2. Add local max condition: detection deferred 1 frame, emitted only if confirmed as peak
3. Run 9-track transient F1 sweep (timing precision should improve)
4. Run 9-track beat F1 sweep (better timing → better beat tracking)
5. Re-calibrate `beatoffset` if timing shifts

**Effort:** Low (~40 lines). **Impact:** Medium-High — improves timing precision of every detection, reduces double-fires.

**Result:** Implemented as `bandflux_peakpick=1` in v28. Local-max confirmation with 1-frame look-ahead.

#### 2.7. Enable Hi-Res Bass — ✅ TESTED, KEEP OFF (v32)

**Already implemented** in BandWeightedFluxDetector (`hiResBassEnabled`). 512-sample Goertzel for 12 bass bins at 31.25 Hz/bin. Doubles bass resolution vs 6 FFT bins at 62.5 Hz/bin.

**Test plan:**
1. Set `hiResBassEnabled=true`
2. Run 9-track transient F1 sweep (focus on kick-heavy tracks)
3. Run 9-track beat F1 sweep
4. If improvement: make default, update SETTINGS_VERSION

**Effort:** Trivial (parameter toggle). **Impact:** Medium for kick discrimination.

**Result:** Tested as `bfhiresbass=1` in v32. Hurts Beat F1 by ~9%. Keep off.

#### 2.8. Complex Spectral Difference ODF for Rhythm Tracking — ELIMINATED

BTrack's default ODF (ComplexSpectralDifferenceHWR) uses both magnitude AND phase. We already have phase data in SharedSpectralAnalysis. CSD catches pitched onsets at constant energy that magnitude flux misses. **For CBSS rhythm ODF only** — phase is too noisy via microphone for visual transient detection.

**Test plan:**
1. Implement CSD in AudioController (or SharedSpectralAnalysis)
2. Test as standalone ODF for CBSS (replace spectral flux)
3. Test as weighted blend: `ODF = α*flux + (1-α)*CSD`
4. 9-track beat F1 sweep for each variant

**Effort:** Medium (~80 lines + 512 bytes). **Impact:** Uncertain — phase noisy via mic in room.

#### 2.9. Tempo Transition Constraints (Apply at Beat Boundaries Only) — PARTIALLY DONE (v28)

Beat-boundary tempo (`beatboundary=1`) defers `beatPeriodSamples_` application to beat fire. The full Bayesian posterior still updates every 250ms but the applied period only changes at beat boundaries. This matches BTrack's behavior (tempo calculated at beat time).

Further constraining posterior updates themselves to beat boundaries was not tested — the current approach (frequent posterior updates, deferred application) seems sound.

---

### Phase 3: Architecture — Major Changes (Future)

These require significant design work and should only be attempted after Phase 1+2 gains are realized and calibrated.

#### 3.1. Joint Tempo-Phase HMM (Bar Pointer Model) — FIRST ATTEMPT TESTED (v37), NEEDS CORRECTED OBSERVATION MODEL

**The biggest architectural gap.** madmom's DBN jointly tracks `(position_within_beat, beat_period)`. Position advances deterministically by 1 each frame. Tempo can only change at beat boundaries. Phase and tempo are structurally coupled. This is fundamentally different from our decoupled approach (Bayesian tempo every 250ms + CBSS phase independently).

**v37 attempt (hmm=1):** Half-time lock at 80 BPM. Likely caused by:
1. Too few tempo bins (20 bins, MAX_HMM_STATES=900)
2. Raw ODF observation model with power-law contrast (hmmContrast=2.0) — doesn't provide sharp discrimination between beat and non-beat states
3. madmom uses RNN activations (0.0-1.0, sharp peaks) — our raw spectral flux ODF lacks this discrimination

**Corrected approach (see Next Actions Priority 1d):**
- 40+ tempo bins with periods 18-60, ~1700 states
- madmom-style observation model: beat region (first 1/lambda of period) gets `obs = odf`, non-beat region gets `obs = (1-odf) / (lambda-1)`
- Transition lambda=100 (strongly penalizes tempo changes)

**Feasibility on nRF52840:**
- ~1700 states × 4 bytes = 6.8 KB forward vector + ~2 KB transitions = ~9 KB total
- ~1700 multiply-adds per frame at 66 Hz = 0.12% CPU. Trivially within budget.
- Sparse transitions: within-beat is just index shift, between-beat only at boundaries

**Would replace:** Bayesian tempo fusion + CBSS + predict-and-countdown. Essentially the entire rhythm tracking backend.

**Risk:** High — complete rewrite of core beat tracking. Must prove improvement before committing.

**Approach:** Prototype in Python first (offline, against ground truth). Compare to current system on 18-track test set. Only port to firmware if clearly better.

#### 3.2. Log-Spaced Sub-Band Filterbank (Previously Priority 7f)

Replace 3-band grouping (bass/mid/high) with 12-24 log-spaced bands for finer frequency discrimination. SuperFlux uses 24 bands/octave. With 128 FFT bins at 62.5 Hz/bin, 12-16 log-spaced bands is realistic. Separates kick from bass guitar, snare crack from vocal energy.

- **Effort:** Medium (~80 lines)
- **Risk:** Medium — requires per-band threshold calibration, may interact with band weights

**Deferred:** Phase 2.7 (hi-res bass Goertzel, already implemented) addresses the most critical bass resolution gap. Sub-band filterbank is a further refinement if bass resolution alone is insufficient.

#### 3.3. Particle Filter Beat Tracking — ✅ DONE (v38-39)

100 particles tracking `(beat_period, beat_position)`. Madmom-style observation model, info gate, phase-coherent octave investigator. PF+CBSS hybrid mode.

**Result:** Achieves 85% BPM accuracy (vs 56% Bayesian-only) but Beat F1 unchanged (~0.28). Correct tempo alone does not improve beat placement — **phase alignment is the F1 bottleneck**. PF is useful for BPM but doesn't solve phase. (v43 Bayesian fixes later achieved 88% BPM accuracy without PF, confirming this finding.)

#### 3.4. Knowledge-Distilled TinyML Onset Detector (Previously Priority 7e)

The only approach that can learn complex spectral patterns (kicks vs bass vs pads vs room modes). Performance gap between DSP and neural onset detection is ~10-15 F1 points.

- **Memory:** ~5-15 KB (model + activations)
- **CPU:** ~200-500us/frame (with CMSIS-NN on Cortex-M4F)
- **Risk:** High — requires training data, model development, TFLM integration

**Deferred:** Long-term research project. Phase 2 improvements may close enough of the gap.

---

### Features Identified for Removal/Simplification

| Feature | Status | Action | Rationale |
|---------|--------|--------|-----------|
| **FT observation** | Disabled (bayesft=0) | ✅ **Done** (v28) | No reference system uses FT for real-time beat tracking |
| **IOI observation** | Disabled (bayesioi=0) | ✅ **Done** (v28) | No reference system uses IOI for polyphonic beat tracking |
| **Adaptive band weighting** | Enabled | **Test disabling** | ~1600 lines + 2.9 KB RAM for potentially minimal benefit |
| **computeSpectralFluxBands()** | Bypassed | ✅ **Done** (v28, unified ODF) | Replaced by BandFlux pre-threshold value |
| **6 disabled detectors** | Code present, disabled | **Keep as-is** | Zero runtime cost; useful for future experimentation |
| **Phase correction** | Disabled (phasecorr=0) | **Keep disabled** | Documented failure on syncopated tracks |
| **Static Bayesian prior** | Disabled (bayespriorw=0) | **Keep disabled** | Hurts tracks far from 128 BPM center |
| **Ensemble fusion complexity** | Active | **Simplify for solo detector** | Agreement scaling, weighted averaging unnecessary with 1 detector |
| **ODF smooth width=5** | Active | **Re-evaluate** | 83ms latency; may be unnecessary with unified ODF |
| **Disabled BandFlux gates** | Code present, disabled | **Keep as-is** | Zero runtime cost; available for future testing |
| **Per-band thresholds** | Disabled | **Keep disabled** | Tested, -0.067 avg F1 regression |
| **diffframes > 1** | Set to 1 | **Keep at 1** | Tested, -0.098 avg F1 regression |

---

## Next Actions (Priority Order)

### Priority 1: Beat Tracking Accuracy

Phase alignment is the primary bottleneck. v43 fixed 4 Bayesian tempo bugs (BPM accuracy 33%→88%, double-time lock eliminated), but Beat F1 is unchanged at ~0.28. Correct BPM does not guarantee correct beat placement. Secondary: ~128 BPM gravity well on slow tracks (3:2 harmonic lock). Run-to-run variance (std=0.04-0.23) limits measurement precision.

#### ~~1a. PLP Phase Extraction~~ — COMPLETED, NO EFFECT

Implemented and tested in v42. Both single-bin DFT (~3% confidence) and comb filter bank IIR (~2% confidence) produce too-low confidence for reliable correction. Mean F1 identical to baseline (0.426 vs 0.426 over 4 runs). Redundant with onset snap. Disabled by default.

#### 1b. Lower CBSS Tightness — UNTESTED WITH NN ODF

Current tightness=8.0 (v40: raised from 5.0, +24% avg F1 with BandFlux ODF). With NN ODF, optimal tightness may differ — NN produces smoother, more periodic activations that could benefit from looser tightness. Requires hardware A/B test.

**Effort**: Parameter change via serial (`set cbsstight 5.0`), A/B test on 18-track suite.

#### ~~1c. Bidirectional Onset Snap~~ — IMPLEMENTED (v44)

Already implemented: `bidirectionalSnap=true` delays beat declaration by 3 frames (~45ms) for bidirectional onset snap window. Default ON since v44.

#### 1d. Joint Tempo-Phase HMM (Bar Pointer Model) — TESTED, FAILED (v37 + v46)

**Status: CLOSED.** Tested twice — v37 (joint HMM) and v46 (phase-only tracker with corrected observation model). Both regress vs CBSS. See v46 section in "Current Status" for full results.

**Why it fails in our context:** The Bernoulli/madmom observation model requires transients at beat positions. In room-captured audio, AGC + compression + whitening reduce transient contrast to the point where many beats have no detectable onset. CBSS's threshold-based approach handles this gracefully; HMM's probabilistic phase tracking does not.

**madmom achieves >74% F1** because it operates on studio-quality audio (clean line-in) where every beat has a clear spectral flux onset. Our mic-in-room setup has fundamentally different signal characteristics.

**IMPORTANT UPDATE (Mar 4, 2026):** The v46 failure was specific to the **Bernoulli observation model** (binary onset/no-onset), NOT the probabilistic phase tracking architecture. madmom's **continuous ODF observation model** uses the raw ODF value at every frame: high ODF at beat positions = evidence for beat, low ODF between beats = evidence for correct phase. Missing a beat degrades probability gracefully instead of catastrophically. This approach has NOT been tested. See "Untried Techniques" section (technique A) for details. The continuous ODF forward filter is the current Priority 1.

#### 1e. Signal Chain Mitigation — TESTED, NEUTRAL (v47, Mar 3, 2026)

v46 investigation revealed that the signal chain applies 3-4 cascaded adaptation stages that
reduce transient contrast before the ODF reaches CBSS. No reference system (BTrack, SuperFlux,
madmom) uses both compression AND per-bin whitening before onset detection.

**Reference system comparison — no system uses compression + whitening for ODF:**

| System | Compression | Whitening | Pre-ODF Processing |
|--------|------------|-----------|-------------------|
| BTrack | None | None | Adaptive threshold (local mean + HWR) |
| SuperFlux | log10(mul*spec+1) | None | Triangular filterbank (24 bands/octave) |
| madmom | log-filtered spectrogram | None | Filterbank (24 bands, 30-17kHz) |
| aubio | Optional | Optional (relax=250s) | Per-ODF config |
| Essentia | log1p(gamma*mag) | None | Percival harmonic enhancement on ACF |
| **This system** | **3:1 soft-knee + 6dB makeup** | **Per-bin running max (decay=0.997)** | **Band-weighted flux + additive threshold** |

**v47 Implementation (SETTINGS_VERSION 47):**
- `bfprewhiten` (default ON): BandFlux receives raw FFT magnitudes (no compressor, no whitening). All other consumers (energy, mel bands, visualizer) still see compressed+whitened magnitudes.
- `whitenbassbypass` (default OFF): Skips whitening for bass bins 1-6 (62-375 Hz) in the whitened path.
- +512B RAM (preWhitenMagnitudes buffer). 288KB flash (35%), 22KB RAM (9%).

**v47 Test Results — NO measurable improvement:**

Tested across 11 tracks with 3-device A/B (prewhiten=1 vs prewhiten=0 vs prewhiten+bassbypass).
Also tested compressor release (0.3s, 1.0s, 2.0s), compressor ratio (1.5:1 vs 3:1), and
BandFlux gamma (5, 10, 20) variants on techno-minimal-01.

| Finding | Details |
|---------|---------|
| **Transient detection unchanged** | All configs produce ~200 detections on techno-minimal-01 (recall ~0.83). BandFlux's `log(1+20*mag)` + adaptive threshold compensates for upstream processing differences |
| **Beat F1 within noise** | Same track same firmware: F1 ranges 0.24-0.52 across runs. Run-to-run variance (std=0.04-0.23) drowns any config effect |
| **BPM accuracy slightly better** | prewhiten=1 shows ~5-7% better BPM accuracy on some tracks (not consistent across full set) |
| **Compressor release 0.3s hurts** | Fast release causes pumping artifacts that increase ODF noise |
| **Gamma recalibration unnecessary** | gamma=5, 10, 20 all produce similar transient counts on raw input |

**Root cause: BandFlux self-normalizes.** Its `log(1+gamma*mag)` with gamma=20 maps any
input range to a manageable scale, and the adaptive additive threshold (running mean + delta)
adjusts to whatever the input magnitude distribution is. The upstream processing IS redundant,
but removing it doesn't help because BandFlux already compensates.

**Conclusion:** Signal chain decompression is architecturally sound (matches reference systems)
and retained as default (`bfprewhiten=1`), but it is NOT the F1 bottleneck. Phase alignment
remains the dominant limitation — the system detects the right onsets and the right tempo, but
beat predictions don't land close enough to ground truth beat times. This is a CBSS
predict+countdown limitation.

### Priority 2: Simplify — Remove Wasteful Features

#### 2a. Test Disable Adaptive Band Weighting
~1600 lines + 2.9 KB RAM. May rarely activate (conditions: periodicity > 0.1, avgEffective > 0.15, bandSynchrony > 0.3). Toggle `adaptiveBandWeightEnabled=false`, run 18-track sweep.

#### 2b. Simplify Ensemble for Solo Detector
Bypass multi-detector agreement logic when only 1 detector is enabled. Keep multi-detector path as dead code.

#### 2c. Simplify Bayesian Fusion
With FT+IOI disabled, reduce to `posterior = prediction × combObs × acfObs`. Two well-understood signals. Consider log-domain additive fusion for numerical stability.

### Priority 3: Remaining Improvements

#### ~~3a. Complex Spectral Difference ODF for CBSS~~ — ELIMINATED (Mar 2026 research)
Phase is too noisy via microphone in a reverberant room. Dixon (2006) found CSD performs **worse** than spectral flux on noisy signals. Not worth implementing for mic-in-room.

#### 3b. Log-Spaced Sub-Band Filterbank
Replace 3-band grouping with 12-24 log-spaced bands. Separates kick from bass, snare from vocals. ~80 lines.

#### 3c. TinyML Beat Activation CNN — VALIDATED (v54, v2 model)
Causal 1D CNN for beat activation. v2 model achieves 0.548 mean F1 (vs BandFlux ~0.28-0.35). v3 wider model (1008ms RF) in training. See NN section above for full results.

### Completed

**Phase 1 (Simplify):**
- ✅ FT+IOI disabled (v28) — no reference system uses these for real-time beat tracking
- ✅ Unified ODF (v28) — BandFlux pre-threshold feeds CBSS, eliminated duplicate computeSpectralFluxBands()
- ✅ Dual-threshold peak picking (v28) — local-max confirmation with 1-frame look-ahead
- ✅ Beat-boundary tempo (v28) — defers period changes to beat fire

**Phase 2 (Improve):**
- ✅ 40 tempo bins — tested v29, reverted (transition matrix drift 2x worse)
- ✅ Adaptive ODF threshold — tested v32, marginal (keep off)
- ✅ Hi-res bass — tested v32, negative (-9% F1, keep off)
- ✅ ODF mean subtraction disabled — tested v32, +70% F1
- ✅ Onset-density octave discriminator — v32, +13% F1
- ✅ Shadow CBSS octave checker — v32, +13% F1
- ✅ Frame rate correction — v39, OSS_FRAME_RATE 60→66
- ✅ Onset snap 4→8 frames — v39
- ✅ cbssTightness 5→8 — v40, +24% controlled comparison

**Phase 3 (Architecture):**
- ✅ Particle filter — v38-39, 100 particles, madmom obs model, PF+CBSS hybrid. BPM accuracy improved but F1 unchanged (phase bottleneck).
- ✅ Joint HMM (first attempt) — v37, half-time lock at 80 BPM. Needs corrected observation model (see Priority 1d).

**Phase 4 (Literature-based improvements, v44-v45):**
- ✅ Bidirectional onset snap — v44, +0.005 F1 (9/18 wins, large trance gains)
- ✅ Percival ACF harmonic pre-enhancement — v45, implemented (untested)
- ✅ PLL proportional phase correction — v45, implemented (untested)
- ✅ Adaptive CBSS tightness — v45, implemented (untested)

**Pre-Bayesian:**
- ✅ Microphone sensitivity — spectral compressor + per-bin adaptive whitening (v23+)
- ✅ ACF inverse-lag normalization — `acf[i] /= lag` (BTrack-style)
- ✅ Bayesian Tempo Fusion — replaced sequential override chain (v18-21)
- ✅ CBSS Adaptive Threshold — cbssthresh=1.0 (v22)
- ✅ Per-sample ACF Harmonic Disambiguation — fixed sub-harmonic lock
- ✅ Bayesian weight sweep — all 6 params swept (Feb 25)
- ✅ Multi-device testing infrastructure — 4-device parallel capture
- ✅ Onset density tracking
- ✅ Diverse test music library — 18 tracks

---

## Bayesian Tempo Fusion — IMPLEMENTED + TUNED (Feb 23-25, 2026)

**Status:** Implemented in SETTINGS_VERSION 18-25. Sequential override chain replaced with Bayesian posterior estimation. v25: harmonic comb ACF (0.8) + Rayleigh prior, tighter lambda (0.07), bidirectional disambiguation. FT/IOI re-enabled at 2.0 after spectral processing (v24), cbssthresh=1.0.

**Architecture summary (v24 — full fusion with spectral processing):**
```
Every 250ms:
  1. Compute autocorrelation of OSS buffer (with inverse-lag normalization, for periodicityStrength + harmonic disambig)
  2. FOR EACH of 20 tempo bins (60-180 BPM from CombFilterBank):
       - ACF observation: weight=0.8 (harmonic-enhanced: 4-harmonic comb + Rayleigh prior, v25)
       - FT observation: weight=2.0 (re-enabled — spectral compressor fixes normalization)
       - Comb observation: comb filter energy at bin, raised to 0.7
       - IOI observation: weight=2.0 (re-enabled — spectral whitening stabilizes onset detection)
  3. Viterbi transition: spread prior through Gaussian (sigma = 0.07 * BPM)
  4. Posterior = prediction × ACF^0.8 × FT^2.0 × comb^0.7 × IOI^2.0
  5. MAP estimate with quadratic interpolation → BPM
  6. Per-sample ACF harmonic disambiguation: check raw ACF at lag/2 (>50%), lag*2/3 (>60%), and lag*2 (0.5x downward, v25)
  7. CBSS adaptive threshold: beat fires only if CBSS > 1.0 * running mean
  8. EMA smoothing (tempoSmoothingFactor) → CBSS beat period update
```

**Key findings:**
1. Bayesian fusion alone cannot prevent sub-harmonic locking — per-sample ACF harmonic disambiguation (step 6) required.
2. Spectral processing (compressor + whitening, v23) fixed normalization issues that made FT/IOI unreliable. Re-enabling both at weight 2.0 yielded +49% avg Beat F1 vs control (v24).
3. Comb filter bank (Scheirer 1998 resonators) remains the primary tempo observation — continuous resonance, phase-sensitive, self-normalizing. Closest to BTrack's primary method.
4. Higher CBSS threshold (1.0 vs 0.4) is the single biggest improvement — fewer phantom beats = more stable BPM = better recall.
5. ACF inverse-lag normalization (BTrack-style `acf[i] /= lag`) reduces sub-harmonic bias. v25 added 4-harmonic comb summation + Rayleigh prior, raising ACF weight to 0.8.

**Resources:** 257KB flash (31%), ~17KB RAM (7%).

---

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | **Design philosophy** — visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history + CBSS eval results |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |

## Research Sources (February 2026 Survey)

### Beat Tracking Algorithms

| Algorithm | Authors | Year | Key Contribution | Embedded Feasible |
|-----------|---------|------|------------------|:-:|
| [BTrack](https://github.com/adamstark/BTrack) | Stark, Davies, Plumbley | 2009 | Autocorrelation + CBSS (our baseline) | Yes |
| [madmom DBN](https://madmom.readthedocs.io/en/v0.16/modules/features/beats.html) | Bock, Krebs, Widmer | 2016 | HMM joint tempo+phase tracking | State-space only |
| [BeatNet](https://github.com/mjhydri/BeatNet) | Heydari et al. | 2021 | CRNN + particle filter with octave investigator | Particle filter only |
| [BeatNet+](https://transactions.ismir.net/articles/10.5334/tismir.198) | Heydari et al. | 2024 | Percussion-invariant representations | No |
| [Real-Time PLP](https://github.com/groupmm/real_time_plp) | Meier, Chiu, Muller | 2024 | Sinusoidal kernel beat prediction | Possible |
| [Beat This!](https://github.com/CPJKU/beat_this) | Foscarin, Schlüter, Widmer | 2024 | 20M param transformer (SOTA) | No |
| [IBT](https://archives.ismir.net/ismir2010/paper/000050.pdf) | Oliveira, Gouyon, Martins | 2010 | Real-time multi-agent beat tracking | Yes |
| [BeatRoot](https://courses.cs.washington.edu/courses/cse590m/08wi/Dixon%20-%20Evaluation%20of%20BeatRoot%20(2007).pdf) | Dixon | 2007 | Multiple competing agents | Offline only |
| [Particle Filter Tempo](https://www.researchgate.net/publication/26532312_Particle_Filtering_Applied_to_Musical_Tempo_Tracking) | Hainsworth | 2004 | Sequential Monte Carlo beat tracking | Yes |
| [KF-PDA](https://www.researchgate.net/publication/4343925_On-line_Music_Beat_Tracking_with_Kalman_Filtering_and_Probability_Data_Association_KF-PDA) | Cemgil et al. | 2004 | Kalman filter + probabilistic data association | Yes (limited) |

### Tempo Estimation

| Algorithm | Authors | Year | Key Contribution | Embedded Feasible |
|-----------|---------|------|------------------|:-:|
| [Fourier Tempogram](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S2_TempogramFourier.html) | Grosche & Muller | 2011 | **Sub-harmonic suppression** (DFT of ODF) | Yes |
| [Autocorrelation Tempogram](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S2_TempogramAutocorrelation.html) | Grosche & Muller | 2011 | Harmonic suppression (complement to Fourier) | Yes (current) |
| [Percival & Tzanetakis](https://webhome.csc.uvic.ca/~gtzan/output/taslp2014-tempo-gtzan.pdf) | Percival & Tzanetakis | 2014 | Pulse train cross-correlation (tested, rejected) | Yes |
| [Comb Filter Bank](https://www.adamstark.co.uk/pdf/papers/comb-filter-matrix-ICMC-2011.pdf) | Stark | 2011 | Resonator bank tempo (tested, same wrong answer) | Yes (current) |
| [Multi-Metrical Tracking](https://www.mdpi.com/2076-3417/9/23/5121) | Holzapfel | 2019 | Hierarchical metrical structure constraints | Possible |
| [Klapuri Multi-Scale](https://www.semanticscholar.org/paper/Tempo-and-beat-analysis-of-acoustic-musical-Scheirer/1fa22e54f70de7a3b36e2ffc602f924f47ec9cbb) | Klapuri | 2006 | Tatum/tactus/bar multi-level tracking | Possible |

### Onset Detection

| Algorithm | Authors | Year | Key Contribution | Embedded Feasible |
|-----------|---------|------|------------------|:-:|
| [SuperFlux](https://github.com/CPJKU/SuperFlux) | Bock & Widmer | 2013 | Max-filter vibrato suppression (partially implemented) | Yes |
| [CNN Onset Detection](https://www.ofai.at/~jan.schlueter/pubs/2014_icassp.pdf) | Bock & Schlueter | 2014 | Multi-scale CNN onset detector (SOTA) | Distilled only |
| [Onset Detection Revisited](https://ofai.at/papers/oefai-tr-2006-12.pdf) | Dixon | 2006 | Comprehensive ODF comparison (flux, phase, complex) | Yes |
| [Complex Domain](https://www.researchgate.net/publication/200806123_Complex_Domain_Onset_Detection_for_Musical_Signals) | Duxbury | 2003 | Combined magnitude + phase onset detection | Yes |
| [Bello Tutorial](https://ieeexplore.ieee.org/document/1495485/) | Bello et al. | 2005 | Foundational onset detection survey | Reference |
| [Peak Picking](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S1_PeakPicking.html) | AudioLabs Erlangen | — | Local max + local mean dual peak picker | Yes |
| [Adaptive Thresholding](https://link.springer.com/article/10.1007/s11042-020-08780-2) | Springer | 2020 | GA-optimized median+stddev thresholds | Yes |
| [TinyML Distillation](https://www.nature.com/articles/s41598-025-94205-9) | Nature | 2025 | 1282-parameter distilled models for MCUs | Yes |

### Key Insight: Autocorrelation vs Fourier Tempogram

The fundamental mathematical reason for sub-harmonic locking: autocorrelation of a periodic signal at period T produces peaks at T, 2T, 3T... (sub-harmonics appear). The DFT of the same signal produces peaks at 1/T, 2/T, 3/T... (harmonics appear, sub-harmonics suppressed). These are complementary — using both together provides the most robust tempo estimation. This is documented in Grosche & Muller 2011 and the AudioLabs Erlangen FMP tutorials.
