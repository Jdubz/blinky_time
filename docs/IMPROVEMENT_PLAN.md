# Blinky Time - Improvement Plan

*Last Updated: April 16, 2026*

> **Historical content (v28-v64 detailed writeups, parameter sweeps, A/B test data)** archived via git history. See commit history for `docs/IMPROVEMENT_PLAN.md` prior to this date.

## Current Status

> **2026-04-25 update:** v32 deployed (b146) on edm_holdout 25 tracks × 3 devices: P=0.422 R=0.223 F1=0.265. v29 (F1=0.484) is still the best deployed. Comprehensive synthesis across 22+ runs (v11-v32) is in `docs/ML_IMPROVEMENT_PLAN.md` ("2026-04-25 — comprehensive synthesis"). The headline: at 30 mel bands and fmax=4 kHz we drop the spectral region (4-10 kHz: hi-hat / snare / cymbal energy) that every published 0.85+ onset detector relies on. Next experiment is mel resolution + fmax expansion to Schlüter '14 spec, gated on first measuring the algorithmic-detector ceiling on edm_holdout (Exp 3).

**Firmware:** b127 (SETTINGS_VERSION 94). AudioTracker with ACF+PLP architecture + pattern slot cache. Multi-source ACF (~4ms) across 3 mean-subtracted sources (spectral flux, bass energy, NN onset). Epoch-fold variance scoring. Cold-start template seeding. Pattern slot cache: 4-slot LRU. ~20 tunable params. AGC removed (v72) — fixed hardware gain (nRF52840: 32). 4 nRF52840 on blinkyhost, all managed via blinky-server. SafeBootWatchdog auto-enters BLE DFU on crash (b106+). TeeStream writes BLE before Serial (b106+). Custom bootloader (RAM magic) deployed on all devices — no physical reset button required for recovery. **b117 changes:** v22 model deployed, NN is PRIMARY onset detector, pulseNNGate parameter removed, mel filterbank corrected to match librosa HTK exactly (26/26 bands wrong since day one, avg 4.2 INT8 level error), MEL_DB_RANGE extracted as constexpr, prevOdf_ renamed to prevSignal_, TestChipConfig.h added for unconfigured bare chips, millis field in json info for clock sync. **b118 changes:** v23 model deployed (corrected mel filterbank + mic profile augmentation, KW F1=0.873, on-device F1=0.625). **b119-b120 changes:** NN-primary continuous visual pulse envelope. Robust PLP epoch-fold: NN-confidence-weighted epochs, per-bin reliability (CV-based), Winsorized mean, cross-correlation with NN fold for pattern validation. PLP reliability metrics in debug stream. **b123-b127 changes:** Local-maxima peak-picking on NN activation (replaced first-diff). Bass-band energy gate (50% threshold increase when bass ratio low). PLP pattern bias (30% threshold increase at off-beat positions, scaled by confidence). pulseOnsetFloor=0.30 (sweep-optimized). v25 model deployed (bias init per RetinaNet). Dead band flux buffers removed (6.3 KB heap saved). Stream pause race condition fixed (wait for acknowledgment). Production optimization: stream formatting gated on client presence. Bucket totem (b120) can't be flashed via software (GPREGRET race, reset button inaccessible).

> **ESP32-S3 support has been cut** (March 2026). All active development targets nRF52840 only.

**NN Model Status:** FrameOnsetNN Conv1D W16 onset-only model. v25 deployed on b127 (all 4 blinkyhost devices). KW F1=0.842 (offline). On-device F1=0.628. 13.4 KB INT8, 6.8ms inference nRF52840. Arena: 3404 bytes. v26 training in progress (asymmetric focal loss: gamma_neg=4.0, gamma_pos=0.0 — downweights easy negatives, focuses on hard negatives like chord changes).

> **F1 caveat (2026-04-20):** Every on-device F1 number quoted in this document (0.472 → 0.62 → 0.625 → 0.628) was measured on the 18 EDM tracks in `blinky-test-player/music/edm/`. All 18 of those tracks are inside the v27-hybrid training corpus — 14 in train, 4 in val, **0 held out**. These numbers are upper bounds, not realistic eval results. Any future retrain claim must use a held-out EDM test split; see `docs/HYBRID_FEATURE_ANALYSIS_PLAN.md` "Training-set contamination" for the action items.

**Key findings (April 12-16):**
- **On-device activations are NOT flat (April 16):** Offline FP32 on clean audio: mean=0.567, std=0.051, dynRange=0.125 (flat). On-device INT8 on real audio: mean=0.432, std=0.250, dynRange=0.734 (dynamic). The acoustic chain (speaker→room→mic) creates contrast the clean audio lacks. The earlier "flat activation" diagnosis was based on offline analysis and was WRONG for on-device. This invalidated the first-diff approach (which over-detected at 14.3/s).
- **Detection algorithm evolution (b123-b127):** b123 first-diff peak-picking over-detected (14.3/s vs 3.5/s GT, net F1=0.601). b126 local-maxima peak-picking (sweep showed 0.3 optimal threshold). b127 +bass-band gate+PLP pattern bias (minimal effect, F1=0.628). The 0.62 plateau is from the MODEL (precision=0.50), not the detection algorithm. Model fires on broadband spectral changes (chords, synths, vocals).
- **v25 training — bias init:** Output bias initialized to log(pos_ratio/(1-pos_ratio)) per RetinaNet. Dynamic range doubled offline (0.061→0.125) but still flat offline. On-device activations were already dynamic regardless — bias init was solving wrong problem. KW F1=0.842 (slightly below v24's 0.851).
- **v26 training in progress:** Asymmetric focal loss (gamma_neg=4.0, gamma_pos=0.0). Downweights easy negatives (silence) 10000x, focuses on hard negatives (chord changes). Previously tested in v12 with noisy labels + wrong mel — clean labels are a new baseline.
- **Architecture experiments completed:** exp-wide [48,48,32] W16: KW F1=0.861 (+1.0%), inference 19.5ms (over budget). exp-w32 [32,32] W32: KW F1=0.850 (no improvement from longer context). Wider model can't run real-time; longer context doesn't help.
- **Mel filterbank mismatch discovered and fixed:** All 26 mel bin edges in firmware were wrong since day one. Corrected to match librosa HTK exactly — avg 4.2 INT8 level error. `MEL_DB_RANGE` extracted as constexpr in SharedSpectralAnalysis.h.
- **NN-primary vs NN-gate:** Changing `updatePulseDetection()` to use NN smoothed activation as the primary signal improved on-device F1 from 0.472 to 0.62. This was a firmware-only change — the entire gain came from using NN output directly instead of gating spectral flux. pulseNNGate parameter removed.
- **mel_db_range=80 experiment (v21) was solving wrong problem:** Widening to [-80,0] dB reduced INT8 resolution without meaningful kick improvement. Reverted to mel_db_range=60.
- **Mel pipeline verified identical (April 14):** Training and firmware mel pipelines (steps 2-8) match within MAE=0.0017 (0.44 INT8 levels). The entire sim-to-real gap is step 1: what audio reaches the mic.
- **PLP pattern extraction verified accurate:** gtPatternCorr metric shows 0.84-0.97 on test tracks.
- **Device management:** Stream pause race condition fixed (wait for acknowledgment, not arbitrary sleep). Bucket totem (b120) can't be flashed via software (GPREGRET race, reset button inaccessible). All 4 blinkyhost devices on b127, all reporting consistently.

**Previous on-device gap diagnosis (April 10-11, partially correct):** Identified mel distribution shift (+0.25 mean, +13 dB) and bass mel saturation (6/26 bands at p95=1.0). v20 proved label quality is solved (offline +21%) but on-device F1=0.430. The real fix was NN-primary pulse detection (not mel range changes).

2. ~~**Model produces flat activations even at training calibration — CORRECTED (April 16).**~~ FP32 model on clean audio does show flat activations (mean=0.567, std=0.051), but on-device INT8 on real audio is dynamic (mean=0.432, std=0.250, dynRange=0.734). The acoustic chain creates the contrast. The offline flatness diagnosis was wrong for on-device.

3. ~~**Firmware uses simpler gate than offline eval — RESOLVED (b117).**~~ The real fix was making NN the PRIMARY pulse signal instead of using it as a gate on spectral flux. On-device F1 jumped from 0.472 to 0.62. Gain augmentation [-18, +18] dB (v19+) and mel filterbank correction also contributed.

**PCEN abandoned (April 10).** v18 failed: PCEN features have AUC=0.5061 for onset discrimination (random chance) vs log-mel AUC=0.6574. PCEN's adaptive AGC normalizes away onset/non-onset contrast. No published onset detection system uses PCEN — it's proven for detection-in-noise (birds, keywords), not musical transient detection.

**Training experiments (April 7-9):**
- v15: madmom MSE distillation, 52ch (mel+delta). Offline onset F1=0.745, KW F1=0.730. On-device onset F1=0.473.
- v16 (previously deployed on all 3 serial devices, b107; superseded by v19 on b111): no delta features (26ch). Offline onset F1=0.782, KW F1=0.727. **On-device onset F1=0.471 — identical to v15.** Delta features provide zero on-device benefit despite 5ms/frame extra inference cost. Confirms offline-to-on-device gap is NOT from feature type.
- v17: band-flux (29ch, 3 HWR mel flux replacing 26 delta). Offline onset F1=0.782, KW F1=0.746. **On-device A/B (April 9): onset F1=0.473 — identical to v16 (0.471).** Band-flux provides zero on-device benefit, same as delta features. Confirms gap is NOT from feature representation.
- v18: PCEN mel normalization (52ch, mel+delta). **FAILED (April 10).** Auto pw=12.7: val_loss plateaued at 0.4976 (barely below random), F1=0.011, recall=0.006 after 36 epochs. pw=20: immediate all-positive collapse. PCEN features have AUC=0.5061 for onset discrimination (random chance). Root cause: PCEN's adaptive AGC normalizes away the transient contrast the model needs. No published onset system uses PCEN for musical onset detection. Also discovered `base.yaml` had `log_epsilon: 1e-7` parsed as string by PyYAML — fixed to `1.0e-7`. Bug only affected v18 dataprep (introduced in PCEN commit c0009054, Apr 7).
- v19: **Aligned with published recipe, COMPLETE, DEPLOYED (b111, April 10).** Five fixes from literature review (Schlüter/Bock 2014, madmom): (1) plain BCE loss replacing asymmetric focal, (2) no global mixup (creates impossible frame-level targets), (3) hard binary targets (consensus > 0.1 → 1.0), (4) no freq_pos_encoding (onset detection is frequency-invariant), (5) no distillation (clean baseline). Same Conv1D [32,32] architecture as v16. Log-mel 26ch. Gain aug [-18, +18] dB. On-device F1=0.477.

**Fleet status (April 16, verified via blinky-server):**
- 062CBD12 — Hat Display, b127 (v25 model), serial, test chip ✅
- 659C8DD3 — Long Tube, b127 (v25 model), serial, installed device ✅
- 2A798EF8 — Hat Display, b127 (v25 model), serial, test chip ✅
- Bucket Totem — b127, can't be flashed via software (GPREGRET race, reset button inaccessible)
- ABFBC412 — Hat Display, b106, BLE-only (RSSI -94 dBm, MTU 20), weak signal

**Serial reliability (April 8):** Root cause identified and fixed — stock TinyUSB CDC sets TX FIFO overwritable on DTR drop, silently killing all serial output. Patch in `patches/tinyusb-cdc-no-overwritable-fifo.patch`, enforced by `build.sh` compile guard. Server hardened: get_info retry, sibling hold during flash, serial retry limit (3 fails → stop), DELETE endpoint for stale devices. See commit `9712664`.

**Labels:** Training data upgraded to consensus_v5 (7-system: beat_this, madmom, essentia, librosa, demucs_beats, beatnet, allin1) with BPM-aware downbeat grid correction and quarantine of 1753 uncorrectable tracks. 75.3% of tracks have perfect every-4th-beat downbeat grids. **v20 labels:** Demucs HTDemucs drum-stem separation → bandpass kick/snare onset detection on isolated drums. 735 silent stems quarantined (noise-on-silence false positives). 6015 clean labels. Pipeline safeguards: `.provenance.json` tracking, overwrite protection, `validate_kick_weighted.py` quality checks, `prepare_dataset.py` quality gating.

**Eval pipeline fixed (April 2):** `evaluate.py` now uses `mir_eval.onset.f_measure` (MIREX 50ms window) instead of `mir_eval.beat.f_measure` (70ms beat tolerance). Peak-pick min interval reduced from 200ms to 50ms to match firmware onset cooldown. All 4 v14 prerequisites complete. **Note: scores from v14+ are not directly comparable to pre-April-2 benchmarks** (50ms onset window vs 70ms beat window). v11's reported Kick F1=0.688, Snare F1=0.773 etc. used the old metric. v14 KW onset F1=0.659 uses the new metric.

**Key constraint:** The LED visualizer runs on a single thread at 60 Hz. Total frame budget is 16.7ms. Conv1D W16 inference takes 6.8ms on nRF52840 (well within 16.7ms budget — 60fps achieved). Mel-spectrogram CNNs require 79-98ms (too slow).

**Mel-spectrogram model history (CLOSED — all too slow):**

| Model | Architecture | Size | Offline F1 | Inference | Status |
|-------|-------------|------|:----------:|:---------:|--------|
| v4 | 5L ch32 standard | 33.3 KB | 0.717 | 79ms | Too slow (12 Hz) |
| v6-restart | 5L ch32 standard | 33.3 KB | 0.727 | 79ms | Too slow (12 Hz) |
| v7-melfixed | 7L ch32 standard | 46.3 KB | **0.787** | >79ms | Best F1, too slow |
| v8 | 7L ch48 standard | ~68 KB | 0.821 | N/A | Heap exhaustion |
| v9 DS-TCN 24ch | 5L DS-TCN | 26.5 KB | TBD | **98ms** | Too slow (10 Hz) |

The v9 DS-TCN was designed to be faster via depthwise separable convolutions (2.7× fewer MACs), but 8 INT8 ADD ops from residual connections cost ~5ms each (36ms total from requantization overhead), making it slower than the standard conv models.

## Active Priorities

### Priority 1: Predominant Local Pulse (PLP) — DEPLOYED (v81)

**Status: DEPLOYED. Multi-source ACF period detection (replaced Fourier tempogram March 31). Soft blend (no hard threshold). Cold-start template seeding. Pattern slot cache. Convergence fix in progress (slotSaveMinConf 0.50→0.25, plpConfAlpha 0.15→0.25, warmup 160→120 frames).**

See `docs/RFC_MUSICAL_PATTERN_VISUALIZATION.md` for full design.

**What was built:**
- **Multi-source ACF period detection (v83, replaced Fourier tempogram)**: ACF at beat-level lags (20-80) across 3 mean-subtracted sources (spectral flux, bass energy, NN onset). Parabolic interpolation refines peaks. Bar multipliers (2x/3x/4x) with sqrt penalty. Epoch-fold variance scoring selects period with best pattern contrast. ~4ms per update (vs 75ms old DFT tempogram).
- **Soft blend (no hard threshold)**: PLP cosine OLA and cosine fallback are continuously blended by confidence (0=pure cosine, 1=pure cosine OLA). No discontinuous switch. Confidence naturally approaches 0 during silence/ambient.
- **Cold-start template seeding**: 8 canonical patterns (four-on-floor, backbeat, halftime, breakbeat, 8th-note, dnb, dembow, sparse). After 1 bar of data, cosine similarity > 0.50 triggers a 50/50 blend of template and observed histogram. Validated: activation time dropped from 8-22s to 0-2.4s on most tracks (amapiano 0.3s, techno-minimal-emotion 1.3s, trance-goa 1.2s). Some tracks still slow (afrobeat 13.5s, breakbeat-bg 16.9s — patterns don't match templates well).
- **Steep signal gate**: Mic level gates confidence with a steep transition near noise floor (`plpSignalFloor`). Once audio is clearly present (>2x floor), gate is fully open. Only suppresses during true silence.
- **Beat stability gated learning**: PLP peak amplitude tracked via EMA. Stability = current/EMA. High stability (>0.7) = locked pattern. Low stability (<0.3) = fill/breakdown/section change. Used to gate bar histogram learning rate (fill/breakdown immunity).
- **Fisher's g tried and reverted**: Fisher's g-statistic (ratio of max DFT magnitude to sum) was implemented as a principled [0,1] confidence measure, but penalizes tracks with multiple similarly-strong periods (common in syncopated music). DFT magnitude used instead — soft blend handles the confidence-to-output mapping without needing a principled scale.
- **Epoch-fold pattern extraction** retained for slot cache pattern digest only (recency-weighted, ~3-epoch half-life). plpPulse output uses canonical cosine OLA.
- **Pattern normalization** uses min-max (signal is mean-subtracted, may have negatives)
- **Confidence** = ACF peak strength x signal presence (steep mic level gate)
- **Adaptive phase correction** removed — phase is implicit in cosine OLA
- Comb filter bank, Percival harmonic enhancement, Rayleigh prior, template matching, LRU cache all removed in v80

**Current test results (b108 validation, April 9, 3 devices × 18 tracks):**
- Onset F1=0.483 (stable vs b107 baseline 0.476). Best: techno-minimal-emotion 0.788, garage-uk-2step 0.586.
- PLP 50/54 working (18/18 tracks). Previous 2/18 regression was transient — confirmed by this run.
- 4 PLP failures are negative autoCorr (not zero): afrobeat/edm-trap/reggaeton/techno-minimal-01. 3 of 4 on device 2A798EF8 — likely acoustic placement issue. All are syncopated genres where patterns don't self-repeat at a single period.
- Param sweeps (April 6): plpnovgain=1.0 (deployed), **plpdecay=0.2** (best plp@T=0.348+autoCorr=0.281, updated from 0.3), slotswitchthresh=0.7 (deployed), plpvarsens=0.0 (deployed)

**ACF convergence fix VALIDATED (April 2):**
v88 fix (`slotSaveMinConf` 0.50→0.25, `plpConfAlpha` 0.15→0.25, warmup 160→120 frames) confirmed working. DnB tracks that never activated now score.

**Remaining work:**
- Sharper NN onset activations (v20 drum-stem labels should fix the 27% non-percussive label noise)
- Band-specific epoch-fold: infrastructure deployed (b110+), selection disabled. Variance and autoCorr metrics both regressed 8-10/18 tracks. Needs a more robust selection metric or genre-preset approach.
- Pattern slot cache tuning (switch/new thresholds, seed blend ratio)

**Key advantages of PLP over PLL:**
- No onset-beat classification needed (resolves circular reliability problem)
- Octave errors are non-issues — half/double time patterns still track musically
- BPM accuracy doesn't matter — the system tracks repeating energy patterns, not phase-locked oscillators
- 3-source input (spectral flux, bass energy, NN onset) provides richer signal than single-channel NN onset
- DFT magnitude inherently suppresses sub-harmonics (no PMR length normalization needed)
- DFT phase gives beat alignment for free (no separate cross-correlation step)

**PLL (v76, archived):** Abandoned March 20 — phase consistency 0.035-0.042 across all models, effectively random. Onset-gated PLL cannot converge because NN detects onsets but cannot distinguish on-beat from off-beat.

### Priority 2: NN-Modulated Pulse + NN Training

**Status: b127 deployed on all 4 blinkyhost devices (v25 model, KW F1=0.842, on-device F1=0.628). v26 training in progress (asymmetric focal loss: gamma_neg=4.0, gamma_pos=0.0). Key finding (April 16): on-device activations are NOT flat — acoustic chain creates contrast (dynRange=0.734 vs offline 0.125). F1 plateau at 0.62 is from model precision (0.50), not detection algorithm. Model fires on broadband spectral changes (chords, synths, vocals). Local-maxima peak-picking + bass gate + PLP bias deployed (b127).**

**Firmware improvements (b108→b120):**
- **b108 (April 9):** NN-modulated pulse output (spectral flux weighted by NN activation, self-tuning via nnConf). Derivative-based NN gate. onset F1=0.483.
- **b117 (April 12):** NN is now the PRIMARY onset detector. `updatePulseDetection()` uses NN smoothed activation (nnSmoothed_) as the signal — spectral flux is fallback only when NN unavailable. pulseNNGate parameter removed. On-device onset F1 improved from 0.472 to 0.62. Mel filterbank corrected to match librosa HTK exactly (26/26 bands were wrong, avg 4.2 INT8 level error). MEL_DB_RANGE extracted as constexpr. prevOdf_ renamed to prevSignal_. TestChipConfig.h added for unconfigured bare chips. millis field added to json info for clock sync.
- **b118 (April 13):** v23 model deployed (corrected mel filterbank + mic profile augmentation, KW F1=0.873, on-device F1=0.625).
- **b119-b120 (April 14):** NN-primary continuous visual pulse envelope. Robust PLP epoch-fold: NN-confidence-weighted epochs, per-bin reliability (CV-based), Winsorized mean, cross-correlation with NN fold for pattern validation. PLP reliability metrics in debug stream. Serial transport crash fix.
- **b123-b127 (April 15-16):** Local-maxima peak-picking on NN activation (replaced first-diff, which over-detected at 14.3/s vs 3.5/s GT). Bass-band energy gate (50% threshold increase when bass ratio low). PLP pattern bias (30% threshold increase at off-beat positions, scaled by confidence). pulseOnsetFloor=0.30 (sweep-optimized). v25 model deployed (bias init per RetinaNet). Dead band flux buffers removed (6.3 KB heap saved). Stream pause race condition fixed (wait for acknowledgment, not arbitrary sleep). Production optimization: stream formatting gated on client presence. Detection algorithm evolution: b123 first-diff F1=0.601 → b126 local-maxima → b127 +bass gate+PLP bias F1=0.628. Minimal FP suppression effect confirms 0.62 plateau is from model precision (0.50), not detection algorithm.

Failed attempts (all regressed from v3):
- v9 (tempo head + distillation): F1=0.233. Root cause: data prep crash → non-augmented data (209K vs 3M chunks) + tempo head useless (256ms RF can't encode tempo, Bock 2019 applies to beat tracking not onset detection).
- v10 shift_tolerant_focal: F1=0.11. Root cause: focal modulation on max-pooled predictions kills positive gradients + missing look_at mask creates conflicting gradients near positives. Shift tolerance and focal loss are fundamentally incompatible (Beat This! uses plain BCE, not focal).
- v10 auto pos_weight: F1=0.164. Root cause: auto pos_weight=35.6 vs v3's manual pos_weight=20. Higher weight over-penalizes with asymmetric focal + soft consensus targets.

Key finding: v3's pos_weight=20 is critical. Auto-calculation gives ~35.6 which doesn't work with asymmetric focal loss on soft consensus targets (strengths 0.14-1.0).

**Onset detection quality (v3 deployed):**

| Metric | v1 (backed up) | v3 (deployed) |
|--------|:------------:|:------------:|
| All Onsets F1 | 0.681 | **0.787** |
| Kick F1 (<200 Hz) | 0.607 | **0.688** |
| Snare F1 (200-4k Hz) | 0.666 | **0.773** |
| HiHat F1 (>4k Hz) | 0.704 | **0.806** |

**v12/v13 post-mortem (April 1): Apparent regressions were eval pipeline artifacts.**

Both v12 and v13 appeared to regress massively vs v11 (onset F1 0.56/0.53 vs 0.62, kick recall 0.47/0.42 vs 0.74). Deep investigation revealed this was a **threshold selection artifact**, not a real model quality difference. At equal thresholds (t=0.40), all three models are nearly identical:

| Metric | v11 @ t=0.40 | v12-best @ t=0.40 | v13 @ t=0.40 |
|--------|-------------|-------------------|--------------|
| KW Onset F1 | 0.649 | 0.645 | **0.653** |
| Kick recall | 0.709 | 0.696 | **0.721** |
| Snare recall | 0.692 | **0.695** | 0.700 |
| HiHat recall | 0.685 | **0.695** | 0.689 |

**Root causes of apparent regression:**
1. **Eval pipeline bug**: `sweep_thresholds()` in `evaluate.py` optimizes threshold against **beat F1** (`.beats.json`), not onset F1. Since v12/v13 have higher activation baselines, the sweep picks higher thresholds that massively under-detect. Fix: sweep against KW onset F1.
2. **Pipeline evaluates wrong model**: `train_pipeline.sh:113` hardcodes `final_model.pt` (SWA-averaged) instead of `best_model.pt`.
3. **SpecMix label bug (v13)**: `specmix()` in `train.py:120` applies a global area-ratio scalar to mix labels across all time frames. For frame-level onset labels, only the pasted time range should get mixed labels. Additionally, frequency-domain cuts break broadband onset semantics (onsets are defined by co-occurring energy across many bands).
4. **OWBCE proximity applied to all frames (v13)**: `train.py:359` multiplies `onset_weight` into loss for ALL frames (positive + negative). Per Song et al. 2024, proximity boost should apply only to positive class weight. Boosting negatives near onsets penalizes the model for correctly predicting low values near onset boundaries.
5. **Wider architecture ([48,48,32]) didn't help**: 28K params produces identical discrimination as 13K params. The task is data/label-limited, not capacity-limited.

**Per-genre findings**: v12/v13 genuinely improved on syncopated genres where v11 over-detects (amapiano +0.19 onset F1, garage +0.16, breakbeat +0.07). Catastrophic on trance (trance-infected-vibes: 0.16 vs 0.67). The wider models' conservatism helps complex rhythms but kills sparse content.

**What we learned:**
- The [32,32] v11 architecture is sufficient — extra capacity doesn't help onset detection with current training data
- SpecMix is theoretically unsound for frame-level sequence labeling (designed for per-clip classification)
- OWBCE concept has merit (v13 marginally best kick recall at equal threshold) but implementation was wrong
- The primary bottleneck is data/label quality, not model capacity

**Already deployed in v11 (proven):**
- ✅ Online mixup augmentation (Beta(0.4,0.4), p=0.5) — Source: SpecMix (2021), DCASE 2024
- ✅ Frequency positional encoding (52-dim learnable vector) — Source: FAC (ICASSP 2024)
- ✅ Asymmetric focal loss (γ_pos=0.5, γ_neg=2.0) — Source: Imoto & Mishima (2022)
- ✅ Delta features (spectral flux as explicit input) — Source: Bock 2012
- ✅ Label neighbor weighting (0.25) — Source: Schlüter & Bock 2014
- ✅ nRF52840 gain calibration (hw_gain_max=32 in base.yaml)

**Attempted in v12/v13 (not deployed — see post-mortem above):**
- ❌ **SpecMix (CutMix for spectrograms)** — Theoretically unsound for frame-level tasks. Label mixing uses global area scalar across all time frames (should be per-frame for sequence labeling). Frequency-domain cuts break broadband onset semantics. Designed for per-clip classification (Kim et al. 2021), not frame-level sequence labeling.
- ⚠️ **OWBCE onset-proximity focal loss** — Concept valid, implementation wrong. Proximity boost applied to all frames (should only boost positive class weight). Neighbor frames (y=0.25) excluded from onset map. v13 showed marginally best kick recall at equal threshold, suggesting the idea has merit with correct implementation.
- ❌ **Wider architecture [48,48,32]** — Same discrimination as [32,32] at 3x the model size and inference cost. Task is data/label-limited, not capacity-limited.
- ❌ **SWA** — Slightly worse than best checkpoint in v12. No benefit for this model size.

**v14 training complete — OWBCE showed no improvement over v11.**

All 4 prerequisites were fixed (evaluate.py → mir_eval.onset, train_pipeline.sh → best_model.pt, OWBCE proximity on positives only, SpecMix disabled). Training ran with corrected OWBCE: best epoch 30, val_loss=0.636, val_F1=0.132 (frame-level). Early stopped at epoch 45.

Offline evaluation with fixed pipeline (mir_eval.onset, 50ms MIREX window, 18 EDM tracks):
- v14 KW Onset F1: **mean 0.659** (range 0.530-0.761)
- Kick recall: 0.877, Snare recall: 0.892, HiHat recall: 0.865
- Comparable to v11 at equal thresholds (~0.649-0.653 from post-mortem)

**Conclusion: the v11 training recipe is the ceiling for the current training data.** OWBCE, SpecMix, wider architectures, and SWA all fail to improve over plain asymmetric focal loss with standard mixup. The bottleneck is **label quality** — current labels are derived from beat trackers, not onset detectors. Beat-derived labels miss off-beat onsets, hallucinate onsets on empty strong beats, and smooth timing to a grid.

**v15 knowledge distillation: superseded by v16.** Madmom MSE distillation with delta features. Offline onset F1=0.745, KW F1=0.730. On-device onset F1=0.473.

**v16 (no delta): previously deployed (b107, superseded by v19 on b111).** Identical recipe to v15 but without delta features (26ch instead of 52ch). Offline onset F1=0.782. KW F1=0.727. Best val_loss=0.3868 at epoch 8, early stopped epoch 23. Inference 6.8ms (vs ~11.7ms with deltas). On-device onset F1=0.471 — identical to v15 (0.473). Delta features provide zero on-device benefit.

**v17 (band-flux): evaluated, NOT deployed.** Onset F1=0.473 on-device vs v16 baseline 0.471. Band-flux provides zero on-device benefit. Same pattern as v16 vs v15: feature representation changes produce zero on-device benefit. The offline-to-on-device gap is NOT from feature representation.

**v18 (PCEN): FAILED (April 10).** Auto pw=12.7: best val_loss=0.4976 at epoch 21, early stopped epoch 36. Recall effectively zero (0.006). pw=20: immediate all-positive collapse. Root cause: PCEN features have AUC=0.5061 for onset discrimination (vs log-mel AUC=0.6574). PCEN's adaptive AGC normalizes away the transient contrast needed for onset detection. No published onset detection system uses PCEN for musical transients — it's proven for detection-in-noise (bird calls, keyword spotting) where signal is sparse against ambient. Also: `log_epsilon: 1e-7` in base.yaml was parsed as string by PyYAML (YAML 1.1 doesn't recognize `1e-7` without decimal), causing mel computation to fail. Fixed to `1.0e-7`. Bug only affected v18+ (introduced in PCEN commit c0009054).

**v19 (aligned with published recipe): COMPLETE, DEPLOYED (b111, April 10).** 5 recipe fixes (Schlüter/Bock 2014): plain BCE, no mixup, hard binary targets, no freq_pos_encoding, no distillation. Gain augmentation extended to [-18, +18] dB. Early stopped epoch 43. Offline onset F1=0.742 (vs v16 0.782 — slight offline regression), KW F1=0.735 (vs v16 0.727 — slight improvement). **On-device F1=0.477** (+0.006 vs v16 0.470). Offline-to-device gap narrowed from 40%→35%. NN-direct peak-picking tested and catastrophically failed (F1→0.162) — INT8 activation on shifted mel not sharp enough for standalone detection. Spectral flux + NN gate remains the working approach. Key finding from F1 decomposition: system **under-detects** (recall=0.43, misses 57% of reference onsets) but reference includes hi-hats the system correctly ignores. Precision=0.61. Offline kick recall=82%, snare recall=83%.

**v19b (sharp targets): FAILED (April 11).** Same as v19 but hard_binary_threshold=0.3 (single-frame onset zones instead of 3-frame). Trained all 60 epochs. val_loss=1.251 (barely converged from initial ~1.28, vs v19's 1.085). val_f1=0.166 (vs v19's 0.340). Root cause: threshold 0.3 binarizes away neighbor_weight=0.25 frames, leaving only single-frame positives (16ms at 62.5Hz). Targets are too sparse for the model to learn from. Confirms Schlüter/Bock finding — neighbor weighting is essential for temporal tolerance.

**v20 (drum-stem kick/snare labels): COMPLETE (April 11). Offline excellent, on-device regressed.** Addresses label quality: Demucs drum separation → bandpass kick/snare on isolated drums. 735 silent drum stems quarantined. 6015 clean labels. Offline KW onset F1=**0.892** (+21% vs v19 0.735). Kick recall=0.936, snare recall=0.937. **On-device F1=0.430** (-10% vs v19 0.477). Offline-to-device gap widened from 36%→52%. Root cause: bass mel bands 0-5 saturate at 1.0 during device music (p95=1.000), destroying kick onset contrast. Better labels made the model MORE specialized for clean mel distributions, worsening device performance. Label quality is solved; mel saturation is the bottleneck.

**v21 (widened mel range [-80,0] dB): REVERTED (April 12).** Same drum-stem labels as v20. Widened mel range to [-80,0] dB to fix bass saturation. However, mel_db_range=80 was solving the wrong problem — kick detection was already the strongest category. INT8 resolution reduction (3.2 vs 4.3 levels/dB) was a net negative. mel_db_range reverted to 60 in both firmware and base.yaml.

**v22 (mel_db_range=60, no quant-noise): superseded by v23 (April 12).** Same drum-stem labels as v20. mel_db_range=60 (matching firmware). No quant-noise regularization. KW F1=0.896. **On-device F1=0.62** (+32% vs v19, primarily from NN-primary pulse detection change in firmware). Mel filterbank corrected to match librosa HTK exactly — avg 4.2 INT8 level error fixed.

**v23 (corrected mel filterbank + mic profile augmentation): DEPLOYED (b118, April 13).** KW F1=0.873. **On-device F1=0.625** (slight improvement from v22's 0.620).

**v24 (recalibrated target_rms_db + new augmentations): COMPLETE (April 14).** target_rms_db recalibrated from -63 to -72 (device mel during music = 0.775 mean vs training 0.924 — 9 dB too loud). New augmentations: speaker THD, comb filter, MEMS soft clip, Freq-MixStyle. KW F1=0.851.

**v25 (bias init per RetinaNet): DEPLOYED (b127, April 15).** Output bias initialized to log(pos_ratio/(1-pos_ratio)). Dynamic range doubled offline (0.061→0.125) but still flat offline. On-device activations were already dynamic regardless (dynRange=0.734) — bias init was solving wrong problem. KW F1=0.842 (slightly below v24's 0.851).

**v26 (asymmetric focal loss): IN PROGRESS (April 16).** gamma_neg=4.0, gamma_pos=0.0. Downweights easy negatives (silence) 10000x, focuses on hard negatives (chord changes). Previously tested in v12 with noisy labels + wrong mel — clean labels are a new baseline.

**Architecture experiments completed (April 15-16):**
- exp-wide [48,48,32] W16: KW F1=0.861 (+1.0%), inference 19.5ms (over 16.7ms budget). Wider model can't run real-time.
- exp-w32 [32,32] W32: KW F1=0.850 (no improvement from longer context). 512ms context doesn't help over 256ms.

**Diagnostic tools deployed (April 9) — results updated (April 16):**
- `replay_device_capture.py`: FP32 model on clean audio: mean=0.567, std=0.051, dynRange=0.125 (flat). **But on-device INT8 on real audio: mean=0.432, std=0.250, dynRange=0.734 (dynamic).** The acoustic chain (speaker→room→mic) creates contrast the clean audio lacks. The "flat activation" diagnosis from offline analysis was WRONG for on-device.
- `mel_distribution_check.py`: Device music mel mean=0.74 vs training=0.49 (+0.25 shift, ~13 dB). Device ambient=0.47 ≈ training. Shift only during music — speaker overwhelms mic calibration.
- `stream nn` includes `nna` field (raw NN activation, pre-gating) alongside `onset` (gated pulse)
- Log epsilon fixed: `1e-7` → `1.0e-7` in base.yaml (PyYAML string parse bug, only affected v18+)

**NN inference performance (April 2 measurement):**

v11 (with delta features, 52 input channels): **11.7ms** on nRF52840 — 70% of the 16.7ms frame budget. v3 (without delta, 26 channels): **6.8ms** — 41% of budget. Layer 1 (Conv1D 52→32, k=5) accounts for 71% of compute due to the wide input. The 5.3x overhead over theoretical MAC throughput is from memory access, quantization, and TFLite framework dispatch (consistent with earlier optimization work).

**Performance approach: accuracy first, then optimize.** The correct order is:
1. Identify the optimal feature set for onset detection accuracy (v15 distillation, PLP tuning)
2. THEN evaluate which features can be dropped without accuracy loss
3. THEN ship a smaller model that fits the frame budget

**NEVER alternate frame workloads** (running NN every other frame). Alternating high/low latency frames causes visible jerking in LED animations. Every frame must have consistent timing. The solution is a better model, not frame skipping.

**Delta feature evaluation: COMPLETE — deltas provide zero on-device benefit.** v16 (26ch, no delta) on-device onset F1=0.471 vs v15 (52ch, mel+delta) onset F1=0.473. Deltas safely dropped, saving ~5ms/frame (11.7→6.8ms). v16 deployed.

**Future training improvements (research-backed):**

1. ~~**Onset-specific knowledge distillation**~~ → **v15 DONE** (madmom MSE, deployed as v16 recipe base)

2. ~~**Increase gain augmentation range**~~ — DONE (v19+). Extended from [-18, +6] to [-18, +18] dB. Applied in v19-v22. The bigger impact was NN-primary pulse detection (b117).

3. **Multi-resolution FFT input** (+3-5% F1, high effort) — Schlüter & Bock 2014's key innovation: 3 FFT window sizes (23ms, 46ms, 93ms) stacked as 3-channel input. Captures both transient detail (short window) and harmonic structure (long window). Every top CNN onset detector (madmom MIREX #1, Schlüter dissertation) uses this. Single most validated architectural choice in published onset detection. Requires firmware changes: 3 FFTs per frame (~4% → ~12% CPU), 3x mel buffer. Source: Schlüter & Bock (ICASSP 2014).

4. **Quantization-Aware Training (QAT)** (+1-3% F1, high effort) — Simulate INT8 quantization during training so model compensates for quantization noise. Larger benefit for small models where each weight matters. Complex to integrate: our PyTorch → Keras → TFLite pipeline requires QAT in the Keras domain, not PyTorch. Source: NVIDIA QAT benchmarks, DCASE 2024 low-complexity task.

5. **Reduce mel bands 26 → 23** (minor, medium effort) — Mel bands 23-25 are degenerate (identical `{116,127,127}` filterbank). Removes 11.5% wasted NN input bandwidth. Requires firmware `NUM_MEL_BANDS` change + retraining. Firmware change trivial, but breaks model compatibility.

6. **Increase mel bands 26 → 40** (+2-5% F1 estimated, high effort) — All published onset detection systems use 80 mel bands. Our 26 gives 1/3 the frequency resolution, limiting spectral discrimination. Going to 40 would double first conv layer size but fits within 60 KB budget. Requires firmware mel filterbank expansion, NN input shape change, and full reprocessing of training data. High impact for distinguishing spectrally similar events.

**Not worth pursuing:**
- Self-supervised pretraining (BYOL-A, Audio-MAE): models too large for MCU, no transfer path
- Curriculum learning: inconsistent evidence, moderate effort
- Wider architecture ([48,48,32]): **proven equivalent to [32,32]** at 3x size (v12/v13 post-mortem). exp-wide W16 confirmed: KW F1=0.861 (+1.0%) but 19.5ms inference (over budget).
- Wider windows (W32/W64): 150ms RF is optimal for onset detection (Schlüter 2014). Our 176ms is ample. exp-w32 confirmed: KW F1=0.850, no improvement from 512ms context.
- Transformer/Conformer: 1.4M+ params, far too large for Cortex-M4F
- Bidirectional models: can't run in real-time. Only ~2-5% F1 gain for onset detection (Bock 2012)
- Half-rate NN inference (every other frame): **NEVER** — alternating high/low frame latency causes visible jerking in LED animations. Every frame must have consistent timing.
- SE attention blocks: global pooling over 16-frame window loses temporal position info critical for onset detection
- Structured pruning: model already tiny (13K params). Pruning helps at 100K+ scale.
- Beat This! distillation: INVALID — beat tracker soft labels suppress off-beat onsets and inject phantom targets on empty strong beats
- Multi-class output (kick/snare/hihat): firmware uses single-channel onset. Per-instrument adds quantization overhead with no visual benefit.
- SpecMix / CutMix: **proven unsound** for frame-level onset detection (v13 post-mortem). Label mixing is wrong for sequence labeling tasks.
- SWA: **proven unhelpful** at this model scale (v12 post-mortem)
- OWBCE onset-proximity loss: **proven equivalent to plain asymmetric focal** (v14). Corrected implementation (proximity on positives only) showed no improvement. The bottleneck is label quality, not loss function.
- PCEN: **proven ineffective** for onset detection (v18). AUC=0.5061 (random chance) vs log-mel AUC=0.6574. Adaptive AGC normalizes away onset/non-onset contrast. No published onset system uses PCEN for musical transients.
- Asymmetric focal loss: **no published validation** for onset detection. Every proven system (Schlüter/Bock 2014, madmom, Beat This!) uses plain BCE. Focal loss adds hyperparameter complexity without measurable benefit over BCE+pos_weight (v14 finding).
- Global mixup for frame-level tasks: creates impossible targets by blending all 128 frames uniformly. SpecMix handles frame-level correctly but is also unsound for onset detection (v13). Published onset systems don't use mixup.
- Frequency positional encoding: contradicts Schlüter 2014 finding that onset detection should be "oblivious to frequency." Onsets are broadband transients; frequency position is irrelevant.

### Priority 3: Server Consolidation — COMPLETE ✅

**Status: All phases complete. blinky-server is the single owner of all device connections, test orchestration, and scoring.**

All 14 CJS test scripts, 6 standalone Python/shell tools, and the blinky-test-player CLI were deleted (16,538 lines removed). The MCP server was rewritten as a thin HTTP client (4,985 → 736 lines). Zero callable scripts open serial ports outside the server.

- ✅ Phase 1: Scoring engine (onset F1 + PLP metrics only, no beat/BPM)
- ✅ Phase 2: Metric cleanup (beat/BPM removed), OTA→firmware rename, serial lock deleted
- ✅ Phase 3: Test session infrastructure (per-device recording buffers)
- ✅ Phase 4: Test runner + REST endpoints (validation, param sweep, threshold tuning)
- ✅ Phase 5: MCP server → thin HTTP client (21 tools as HTTP wrappers)
- ✅ Phase 6: External scripts deleted (51 files, 16,538 lines)

**Available test endpoints:**
- `POST /api/test/validate` — run validation suite (onset F1 + PLP metrics). Validation now resets devices to defaults before each run (`_configure_device`).
- `POST /api/test/param-sweep` — multi-device parameter sweep with batching
- `POST /api/test/tune-threshold` — binary search for optimal onset threshold
- `POST /api/test/capture-nn/{id}` — capture NN diagnostic stream (mel bands + onset)
- `GET /api/test/jobs/{id}` — poll async job results

**Infrastructure improvements (April 12-14):**
- `scripts/deploy.sh`: single-command compile → upload → flash → verify pipeline
- `POST /api/fleet/upload`: binary firmware upload endpoint with API key auth (no scp needed)
- API key auth on all flash endpoints (auto-generated `~/.blinky-api-key`)
- Clock sync in `test_runner.py` for firmware timestamps (`millis` field in `json info`)
- Pre-push hook: `ruff check`, `ruff format`, `mypy` for blinky-server
- PLP metrics tooling: gtPatternCorr (cosine similarity between device pattern and GT-folded onsets), reliability, nnAgreement in scoring
- Debug streaming enabled during validation for PLP diagnostic fields
- Serial transport crash fix (TypeError from None fd during USB disconnect)

### Architecture History (Collapsed — see git log for details)

**FC → Conv1D → W16 onset-only journey (Feb-March 2026):** Beat-synchronous hybrid abandoned (circular dependency). Frame-level FC deployed. Conv1D W64 deployed (27ms). W192 FC regressed (flattening destroys locality). Dual-model abandoned (every published system uses single joint). Conv1D W16 onset-only deployed (All Onsets F1=0.681, 6.8ms). BandFlux fully removed (v67). See git history for `IMPROVEMENT_PLAN.md` for detailed writeups.

**Why frame-level works on Cortex-M4F:**

The mel-spectrogram CNN was slow (79-98ms) because of Conv2D operations — CMSIS-NN still requires 37ms for convolutions, plus overhead from Pad, SpaceToBatch, residual Add requantization. FC layers have none of this overhead:

| Approach | Input size | Rate | Inference | CPU |
|----------|-----------|------|-----------|-----|
| Mel CNN (conv, CLOSED) | 128×26 = 3,328 | 62.5 Hz | 79-98ms | >100% (impossible) |
| Beat-sync FC (ABANDONED) | 4×79 = 316 | ~2 Hz | 83µs | <0.1% |
| **Frame-level FC (every 4th frame)** | **N×26** | **15.6 Hz** | **~60-200µs** | **~0.1-0.3%** |
| Frame-level FC (every frame) | N×26 | 62.5 Hz | ~60-200µs | ~0.4-1.2% |

All frame-level FC options are well within the 10ms per-frame budget.

**Key design decisions:**

1. **Raw mel bands as stable interface.** Same principle as beat-sync approach — uses `rawMelBands_` (pre-compression, pre-whitening), decoupled from 47+ tunable firmware parameters. Only depends on 8 fundamental constants (sample rate, FFT size, hop, mel bands, mel range, mel scale, log compression, window) that never change.

2. **NN replaces BandFlux as ODF source.** The onset activation output provides a higher-quality ODF signal. BandFlux removed in v67.

3. **No circular dependency.** Feature extraction (raw mel frames) is independent of tempo tracking. The NN produces onset activation; ACF handles tempo estimation from spectral flux (NN-independent). PLP extracts repeating energy patterns for phase/pulse synthesis.

4. **ACF for period estimation (simplified).** Spectral flux (HWR) feeds ACF. Percival harmonic enhancement, Rayleigh prior, comb filter bank, and template matching all removed in v80. PLP Fourier tempogram (Goertzel DFT) selects optimal period across 3 mean-subtracted sources (spectral flux, bass energy, NN onset) — DFT magnitude selects period, DFT phase gives alignment. Cold-start template seeding (v81) accelerates warm-up.

5. **Downbeat deferred.** System focuses on onset detection + BPM + pulse. Downbeat tracking deferred indefinitely.

**Context window sizing:**

At 120 BPM, one beat = 0.5s = ~31 frames at 62.5 Hz. The context window should capture at least one full beat interval. Options to explore:

| Window | Frames | Input dim | Coverage at 120 BPM | Notes |
|--------|--------|-----------|---------------------|-------|
| 0.26s | 16 | 1,664→416 | ~0.5 beats | **Conv1D W16 DEPLOYED (FrameOnsetNN)** — 13.4 KB INT8, 6.8ms, All Onsets F1=0.681 |
| 0.5s | 32 | 832 | ~1 beat | FC model (cal63) |
| 1.0s | 64 | 1,664 | ~2 beats | Conv1D W64 (replaced by W16) — 15.1 KB INT8, 27ms |
| 1.0s | 64 | 1,664 | ~2 beats | FC W64 (marginal for downbeat) |
| 3.07s | 192 | 4,992 | ~6 beats (1.5 bars) | **FC REGRESSED** — too many flat inputs |

**Conclusion (March 15):** Wider windows are necessary for downbeat (need to see a full 4/4 bar) but FC architecture cannot exploit them. The first FC layer (4992→64) has 319K of the model's 322K total params, and must implicitly encode all temporal relationships through weight correlations. This destroys temporal locality. Conv1D with temporal pooling is the correct architecture for wide windows — preserves local patterns through convolutions, then progressively compresses time dimension via pooling before FC classification.

**Model architecture (initial):**

```
Input: 32 × 26 = 832 floats (flattened)
  → FC 832 → 64 (ReLU)
  → FC 64 → 32 (ReLU)
  → FC 32 → 2 (Sigmoid: beat_activation, downbeat_activation)
Output: [beat_prob, downbeat_prob] per frame
```

~56K params, ~56 KB INT8. Fits in flash budget (1 MB total, ~260 KB base firmware). Tensor arena ~8-16 KB. If too large, reduce hidden layers or window size.

**Projected resource usage:**

| Resource | BandFlux (removed v67) | Conv1D W16 (deployed) | Notes |
|----------|-------------------|-------------------|-------|
| ODF quality | ~28% F1 | All Onsets F1=0.681 | Learned vs hand-tuned |
| Inference time | <0.1ms @ 62.5 Hz | 6.8ms @ 62.5 Hz (nRF52840) | Well within 16.7ms frame budget |
| Tensor arena | 0 | 3404 bytes (of 32768 allocated) | Conv1D W16 |
| Mel frame buffer | 0 | ~1.7 KB (16×26×4 bytes) | Ring buffer |
| Model flash | 0 | 13.4 KB INT8 | Per-tensor quantization |
| Downbeat | No | No (onset-only) | System focuses on onset/BPM/phase |

**Training data and labels:**

Frame-level labels already exist from the mel-CNN work:
- Raw audio files (`/mnt/storage/blinky-ml-data/audio/`, ~7000 tracks)
- 4-system consensus beat/downbeat labels (frame-level, `/mnt/storage/blinky-ml-data/labels/consensus_v2/`)
- Mel extraction pipeline (`scripts/audio.py`) — already firmware-matched
- Audio augmentation (gain, noise, RIR, time-stretch) from `prepare_dataset.py`
- Mic calibration profiles (gain-aware augmentation)

The training pipeline from `prepare_dataset.py` → `train.py` produces frame-level mel spectrograms and frame-level beat/downbeat targets. The main change is replacing the CNN model with an FC model that operates on a sliding window of mel frames.

**Firmware changes:**

1. **Mel frame ring buffer (~50 lines)** — Simple circular buffer of raw mel frames. SharedSpectralAnalysis already computes rawMelBands_ every frame; just store the last N frames. Replaces SpectralAccumulator (which accumulated between beats).

2. **FrameOnsetNN (~150 lines, replaces BeatSyncNN)** — TFLite Micro inference. Input: mel frame window. Output: onset activation. Runs every frame at 62.5 Hz. Much simpler than BeatActivationNN (no sliding mel buffer management, no multi-channel output).

3. **AudioController.cpp (~30 lines changed)** — Replace BandFlux ODF with NN onset activation. NN downbeat output feeds `control_.downbeat`. Fallback to BandFlux when NN not compiled.

**Phased implementation:**

- ~~**Phase A (beat activation only):**~~ DONE — FC model deployed, beat+downbeat activation working on all 3 devices.
- ~~**Phase B (mel calibration):**~~ DONE — calibrated `target_rms_db` from -35 to -63 dB (mel mean 0.52, matching firmware AGC). Cal63 model trained on corrected data. On-device A/B (6 tracks): mean ODF 0.30 vs 0.20 (+50%), BPM accuracy improved 4/6, downbeat activations now functional (max 0.37-0.57 vs 0.00).
- ~~**Phase C (dual-model architecture):**~~ ABANDONED (Mar 16). Research showed every published beat/downbeat system uses a single joint model — the dual-model split underperformed the FC baseline on both tasks while using 8.5x more RAM. Reverted to single Conv1D with multi-task output.
- ~~**Phase C (revised): Single Conv1D W64 with sum head.**~~ DONE — Conv1D(26→24,k=5) → Conv1D(24→32,k=5) → Conv1D(32→2,k=1). Beat This! sum head constrains downbeat ≤ beat. W64 (1.024s, ~2 beats). 15.1 KB INT8, 27ms measured on device. Deployed on all 7 devices.
- ~~**Phase D (BandFlux removal):**~~ DONE (v67) — Removed EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult. 10 files deleted, ~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved. SETTINGS_VERSION 66→67.

**Research context (updated March 16, 2026):**
- ALL leading beat trackers use frame-level NNs with **single joint models**: BeatNet (CRNN), Beat This! (CNN+Transformer), madmom (BiLSTM), TCN beat tracker, BEAST (Streaming Transformer), WaveBeat (Strided Conv1D)
- **No published system splits onset from downbeat into separate models** — multi-task joint training improves both tasks via shared features, structural constraints (downbeats ⊆ beats), and regularization (Bock 2019: +5 F1 from tempo auxiliary task)
- Beat This! "sum head" technique constrains downbeat output ≤ beat output structurally
- Our Conv1D W64 with sum head follows this proven paradigm

#### Closed: Dual-Model Architecture (March 15-16, 2026)

**Attempted and abandoned.** Dual-model split (OnsetNN W8 + RhythmNN W192) was architecturally wrong:
- OnsetNN underperformed FC baseline — W8 too short to distinguish beats from non-beats
- RhythmNN (DB F1=0.114) < FC baseline (0.238) — 16x pooled labels lost 94.6% of beat labels (bug), plus 74ms inference (over budget)
- Combined 8.5x more RAM than FC for worse results on both tasks
- Every published system uses single joint model — no literature support for the split

#### Architecture Research (March 16, 2026)

**Platform compute budgets (30ms inference):**

| Platform | Clock | Cycles/MAC | MACs budget | Model sweet spot |
|----------|-------|-----------|-------------|-----------------|
| nRF52840 | 64 MHz | ~3 (flash waits) | ~640K | 2-3 layer Conv1D, 24-32ch, W32-64, 5-15K params |
| ESP32-S3 | 240 MHz | ~1.7 (ESP-NN) | ~4M | 3-4 layer Conv1D, 48-96ch, W64-128, 20-80K params |

ESP32-S3 has ~6.5x more compute budget — platform-specific models are feasible.

**Improving downbeat (DEFERRED — out of scope):**

System now focuses on onset/BPM/phase only. Downbeat detection deferred indefinitely. The following approaches remain viable if downbeat is revisited in the future:

1. **Tempo auxiliary head during training**: Shared backbone predicts tempo via FC branch, stripped at export. Zero firmware cost. +1-5% F1 (Bock 2019, BEAST). Code exists in `beat_fc_enhanced.py`.
2. **FiLM conditioning**: Modulate features by tempo/phase. Train/test mismatch problem — try after beat tracking improves.
3. **Beat-synchronous mel accumulator**: Depends on beat quality. Revisit when phase alignment is reliable.
4. **DSP features as extra channels**: Constant across Conv1D window, useless to convolutions.

**Input representation:** Mel bands confirmed correct by all SOTA systems. 62.5 Hz frame rate adequate.

#### Closed: W192 FC (March 15, 2026)

W192 FC (4992→64→32→2, 322K params, 314 KB INT8): severe regression from W32 FC. Root cause: FC flattening of 192×26=4992 inputs destroys temporal locality. The first FC layer (4992→64) contains 319K of the model's 322K total parameters and must implicitly encode all temporal relationships through raw weight correlations. Training converged to val_loss=0.497 with only 84.5% accuracy — the model failed to learn meaningful temporal patterns from the flat input. Superseded by dual-model architecture with Conv1D temporal pooling.

### Priority 5: ESP32-S3 Mic Calibration and Model — VERY LOW PRIORITY

**Status: Phase 1 DONE (March 15, 2026) — mic profile measured, training config created. ESP32-S3 PDM mic fixed (JTAG pin conflict + partial DMA read). Calibration showed nRF52840 and ESP32-S3 mics are similar enough that a platform-specific model is VERY LOW PRIORITY. Phase 2 (retrain with ESP32-calibrated data) deferred indefinitely.**

The XIAO ESP32-S3 Sense uses a PDM microphone via ESP-IDF I2S PDM-RX. Its frequency response, noise floor, and AGC transfer function differ from the nRF52840's built-in microphone. The nRF52840 cal63 model was trained on mel spectrograms captured at `target_rms_db=-63 dB` through the nRF52840 mic chain. Running that model on ESP32-S3 audio will produce mismatched mel statistics and degraded ODF quality.

**Calibration results (March 15, 2026):**

| Parameter | nRF52840 | ESP32-S3 |
|-----------|---------|---------|
| `target_rms_db` | -63 dB | **-30 dB** |
| Operating mel mean | ~0.52 | ~0.78 |
| Best SNR gain | gain=30 (hw AGC) | gain=30 (sw gain only) |
| Band gain range | 0.95–1.04 | 0.93–1.01 (flat) |
| Noise floor range | 0.58–0.94 | 0.66–1.00 |

The ESP32-S3 operates ~0.25 higher mel mean. The software-only AGC (no hardware gain register) converges to a higher operating point (~gain=14–18 during music) vs nRF52840's hardware AGC. Best SNR at gain=30 on both units (27.8–28.7 dB avg across bands 3–25). Above gain=30, SNR degrades (software gain adds noise without pre-decimation amplification).

**Measured unit-to-unit variation:** Two ESP32-S3 units (ACM0 MAC `11:F8:10`, ACM3 MAC `12:C1:A0`) are < 1 dB apart after confirming identical firmware. The original apparent 3–4 dB divergence was entirely due to firmware version mismatch (one unit was running old firmware with a different streaming rate — 40 Hz vs 21 Hz). Normal MEMS mic manufacturing tolerance is ±1–3 dB per spec; actual measured variation is ≤1 dB between these two units.

**Artifacts:**
- Mic profile: `ml-training/data/calibration/mic_profile_esp32s3.npz`
- Gain sweeps: `data/calibration/gain_sweep_ttyACM0_esp32.npz`, `gain_sweep_ttyACM3_esp32.npz`
- Training config: `ml-training/configs/frame_fc_esp32s3.yaml` (`target_rms_db=-30`, `hw_gain_max=30`)

**Remaining steps:**

2. **Train ESP32-S3 model** — Retrain the FC beat/downbeat model with ESP32-calibrated data:
   ```bash
   python scripts/prepare_dataset.py --config configs/frame_fc_esp32s3.yaml --augment \
       --mic-profile data/calibration/mic_profile_esp32s3.npz
   python train.py --config configs/frame_fc_esp32s3.yaml
   ```
   Same architecture (32-frame window, FC [64,32] → 2 outputs). Produces `frame_onset_model_esp32s3_data.h`.

3. **Firmware model selection** — Add `#ifdef BLINKY_PLATFORM_ESP32S3` to select the ESP32-S3 model at compile time, keeping the nRF52840 model unchanged.

**Hardware gain note:** The ESP32-S3 I2S PDM-RX slot config has **no hardware gain register** — confirmed by ESP-IDF source (`soc_caps.h`: `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER` not defined for S3, `i2s_ll.h`: no gain functions in PDM RX path). The full AGC range is applied as software post-decimation gain in `Esp32PdmMic::setGain()` / `poll()`.

### ~~Priority 2 (old): BandFlux Removal~~ — COMPLETED (v67)

**Status: COMPLETED — March 12, 2026**

Removed all BandFlux/EnsembleDetector code. SharedSpectralAnalysis promoted to direct AudioController ownership. Pulse detection inlined from EnsembleFusion (ODF threshold + tempo-adaptive cooldown). Non-NN fallback: `mic_.getLevel()` as simple energy ODF. See git log for details.

### ~~Priority 3: CBSS ODF Contrast~~ — COMPLETED (v66), CBSS REMOVED (v75)

**Status: COMPLETED — cbssContrast=2.0 was the default. CBSS entirely removed in v75, replaced by ACF + comb bank.**

A/B tested cbssContrast=1.0 vs 2.0 (BTrack-style ODF squaring). Results: 10 wins, 6 losses, 2 ties across 3 devices × 18 tracks. Mean BPM error 12.4 vs 12.6. Octave errors unchanged (9 vs 9). Default updated to 2.0 in v66. CBSS subsequently removed in v75.

### Future: Heydari 1D State Space

**Status: RESEARCH ONLY**

Heydari et al. (ICASSP 2022) — 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. ~860 states fits our memory budget. CBSS already removed (v75). Could complement PLP if additional tempo tracking robustness is needed.

## Current Bottlenecks

1. ~~**Flat NN activation shape — RESOLVED (b117).**~~ On-device onset F1 was stable at ~0.47 across v15-v19. Root cause was NOT the activation shape itself but the pulse detection approach: using NN as a gate on spectral flux instead of using NN directly. Making NN the primary pulse signal (nnSmoothed_) improved on-device F1 from 0.472 to 0.62. Additionally, mel filterbank was corrected (26/26 bands wrong since day one). PCEN was abandoned (AUC=0.5061, v18). **Mel pipeline verified identical** (MAE=0.0017) — remaining gap is entirely acoustic (speaker → air → room → mic vs clean digital). target_rms_db recalibrated from -63 to -72 for v24.

2. **PLP pattern consistency — OPEN.** 7/18 tracks show negative autoCorr in syncopated genres (breakbeat, garage, amapiano). Slot cache partially addresses via multi-pattern switching. Recent validation (April 9, 3 devices) shows PLP working on 51/54 track-device pairs, but a single-device retest 7h later showed PLP dead on 16/18 tracks (likely transient device state, not regression — same device had PLP working in the prior run). Needs re-validation after device power cycle.

2. ~~**Onset/phase circular reliability problem — RESOLVED.**~~ PLP architecture eliminates the circular dependency. PLP uses Fourier tempogram (Goertzel DFT) across 3 mean-subtracted sources (spectral flux, bass energy, NN onset) to extract repeating patterns. The NN onset detector continues to drive visual sparks/flashes independently.

3. ~~**~135 BPM gravity well — NON-ISSUE.**~~ With PLP, octave errors are non-issues. Half/double time patterns still track musically.

4. ~~**Pattern section switching — DEPLOYED (v82).**~~ Pattern slot cache: 4-slot LRU of 16-bin PLP pattern digests. Instant section recall via cosine similarity matching. Replaces v77 pattern memory (IOI histogram + bar histogram).

5. ~~**Mel level mismatch (RESOLVED March 13)**~~ — Fixed with cal63 model.
6. ~~**Downbeat detection (DEFERRED)**~~ — System focuses on onset/BPM/pulse only.
7. ~~**NN inference speed (RESOLVED)**~~ — 6.8ms nRF52840, 5.8ms ESP32-S3.

## SOTA Context (March 2026)

| System | Year | Architecture | Notes |
|--------|:----:|-------------|-------|
| BEAST | 2024 | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | Conv1D W16 ODF + ACF+PLP (mic-in-room, nRF52840) | Offline KW F1=0.873 (v23). On-device onset F1=0.625. PLP deployed v81. NN-primary b117. Robust PLP b119. |

**Note:** SOTA table previously listed Beat F1 (onset-vs-metrical-grid alignment). This metric is not comparable to our onset F1. SOTA systems are evaluated on line-in audio with standardized beat annotations; our system detects acoustic onsets through a microphone in a room.

**Key insight:** SOTA systems use strong neural frontends (CNN, CRNN, Transformer) that require 79ms+ on our hardware. The Conv1D W16 approach follows the same paradigm (frame-level NN activation → post-processing) but uses lightweight Conv1D layers, achieving 6.8ms inference (well within frame budget). The NN provides learned onset activation for visual pulse; spectral flux feeds ACF for tempo estimation; PLP extracts repeating energy patterns via Fourier tempogram (Goertzel DFT) across 3 mean-subtracted sources (spectral flux, bass energy, NN onset) for phase and pulse synthesis — DFT magnitude selects period, DFT phase gives alignment. Using raw mel bands as the stable interface decouples the NN from firmware signal processing parameters.

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~~Flat NN activation on-device~~ | ~~Log-mel gives sustained ~0.45 activation~~ | ~~RESOLVED~~ | Fixed by NN-primary pulse detection (b117) + mel filterbank correction. On-device F1: 0.472→0.62. |
| Offline-to-on-device gap | On-device onset F1=0.625 vs offline 0.87 (28% drop) | **Medium** — reduced but still present | Mel pipeline verified identical (MAE=0.0017). Remaining gap is entirely acoustic (speaker → air → room → mic). target_rms_db recalibrated -63→-72 for v24. |
| Run-to-run variance | Initial phase lock depends on exact audio timing | Requires 3+ runs for reliable eval | Silence state reset (5s) helps; inherent variability |
| Syncopated self-consistency | Breakbeat/garage/amapiano patterns don't repeat at single period | 7/18 tracks have negative autoCorr | Slot cache partially addresses. May be inherent to genre. |
| DnB half-time detection | librosa and firmware both detect ~117 vs ~170 | **None** — acceptable for visuals | -- |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** — organic mode is correct | -- |

## Closed Investigations (v28-v65)

All items below were A/B tested and showed zero or negative benefit, or proven infeasible. Removed from firmware in v64 unless noted.

- **Mel-spectrogram NN models (v4-v9)**: All architectures (standard conv, BN-fused, DS-TCN) exceed 79ms inference on Cortex-M4F @ 64 MHz. The v9 DS-TCN (designed for speed) measured 98ms due to INT8 ADD requantization overhead from residual connections. No mel-spectrogram CNN architecture can fit the 10ms per-frame budget. Superseded by frame-level FC approach (~60-200µs). NN always compiled in since v68 (ENABLE_NN_BEAT_ACTIVATION ifdef removed).

- **Beat-synchronous hybrid corrector (March 10-11, 2026)**: FC model on accumulated spectral summaries at beat rate (~2 Hz). Phase A (downbeat-only) achieved val_F1=0.548 but label analysis revealed fundamental problems: Cohen's d < 0.13 between downbeat/non-downbeat features, linear classifier gains 0% over baseline, only 51.6% of tracks have clean period-4 downbeats. Circular dependency: unreliable CBSS (~28% F1) produces noisy beat boundaries → noisy features → unreliable correction. ALL leading algorithms use frame-level NNs; only Krebs 2016 used beat-level (with bidirectional GRU, impossible in real-time). Pivot to frame-level FC avoids circular dependency and matches proven paradigm. Code retained: SpectralAccumulator.h, BeatSyncNN.h, models/beat_sync.py, scripts/beat_feature_extractor.py, scripts/export_beat_sync.py — but not actively used.

- **Full Python signal chain simulator (March 2026)**: Originally proposed reimplementing the complete BandFlux → OSS → ACF → comb filter → Bayesian fusion → CBSS pipeline in Python (~2000 lines). No longer needed — frame-level FC approach uses frame-level labels directly, no CBSS simulation required.

- **Forward filter** (v57-v60): Full 6-param sweep, 7/18 octave errors vs CBSS 4/18. Half-time bias fundamental.
- **Spectral noise subtraction** (v56): Baseline wins 13/18. Code retained, default OFF.
- **Template+subbeat** (v50): No net benefit (baseline 10 wins, subbeat 8).
- **Tempo bins 20→47** (v61): No improvement. Gravity well not a bin count issue.
- **Focal loss** (v5): Identical to v4.
- **HMM phase tracker** (v37, v46): Bernoulli obs model fails on mic audio. CBSS was retained at the time (since removed in v75).
- **PLP phase extraction** (v42): OSS too noisy at the time. Now revisited with improved architecture — see Priority 1 (PLP) and `docs/RFC_MUSICAL_PATTERN_VISUALIZATION.md`.
- **Signal chain decompression** (v47): BandFlux self-normalizes. Not the F1 bottleneck.
- **Particle filter** (v38-39): Improved BPM but not F1. Phase is the bottleneck.
- **Adaptive tightness, Percival harmonic, bidirectional snap** (v44-45): Marginal or no benefit.

## Visualizer Improvements

### Fire Generator Enhancement

**Status: PARTIALLY COMPLETE — audio reactivity done, visual depth improvements remain**

The fire generator (`Fire.h/cpp`) is a particle system with 3 spark types (FAST_SPARK, SLOW_EMBER, BURST_SPARK), thermal buoyancy, simplex noise wind via `ForceAdapter`, and audio-reactive spawn/velocity modulation. It inherits from `ParticleGenerator<64>` and renders to `PixelMatrix` via additive blending. All physics parameters are stored as dimension-normalized fractions and scaled at use-time via `scaledVelMin()`, `scaledSpread()`, etc. — no per-device tuning needed.

**Code review findings (March 15, 2026):**
- All 7 AudioControl signals are already consumed (phase, pulse, downbeat, energy, rhythmStrength, onsetDensity, beatInMeasure). The original plan items 1-4 are implemented.
- `FireDefaults` struct removed (commit 15ce5c7). Was vestigial from old heat-diffusion model — `Fire::begin()` never read it.
- Background noise (`MatrixBackground`) iterates every pixel with 2 simplex noise evals per pixel. At 32×32 = 1024 pixels this is ~2048 noise calls/frame — still under 1ms at 240 MHz (ESP32-S3) but worth monitoring.

**Scaling constraint: particle pool size.** `ParticleGenerator<64>` hard-caps at 64 particles (`uint8_t` template, max 255). `scaledMaxParticles()` caps at `min(64, 0.75 * numLeds)`. On the 32×32 display (1024 LEDs), 64 particles cover 6% of pixels. The background noise layer fills the remaining 94%. This is the primary visual bottleneck for larger matrices — the fire looks sparse because the particle layer can't fill the frame.

#### Completed (items from original plan)

All originally planned audio coupling is implemented in `Fire.cpp`:

- **Phase-driven thermal buoyancy breathing** — `updateParticle()` lines 300-331: `phaseMod = 0.5 + 0.5 * phasePulse`, applied to `scaledThermalForce()`. Also phase-driven drag (1.5% extra off-beat).
- **Downbeat dramatic effects** — `spawnParticles()` lines 138-143: `downbeatSpreadMult_` (2.5× → 1.0 over 0.5s), `downbeatColorShift_` (white/blue tint, 0.5s decay), `downbeatVelBoost_` (1.5× velocity, 0.3s decay). `particleColor()` applies the tint.
- **Onset density → particle character** — `spawnTypedParticle()` line 237: `densityLifeMult = 1.5 - 0.75 * densityNorm`. Also drives noise speed in `generate()` lines 64-66 and wind turbulence line 85.
- **Beat-in-measure accent patterns** — `spawnParticles()` lines 146-159: beat 3 at 50%, beats 2/4 at 25%, odd/even left/right rocking bias via `spawnBias_`.

#### Remaining Improvements

Ranked by visual impact. All items use only `width_`, `height_`, `numLeds_`, `traversalDim_`, `crossDim_` for scaling — no device-specific constants.

##### ~~Tier 1: High Impact, Low Effort~~ — ALL DONE

**~~1. Dynamic particle pool~~** — DONE (commit b856b98). Removed compile-time `ParticleGenerator<N>` template. Pool allocated in `begin()` from `particleDensity() * numLeds`. 32×32 display gets 768 fire particles (was capped at 64). All generators (Fire/Water/Lightning) auto-size.

**~~2. Domain-warped noise background~~** — DONE (commit 5060701). `MatrixBackground::sampleNoise()` uses Stefan Petrick domain-warp for fire style: first noise field distorts coordinates of second. Swirling lava-lamp movement. Same cost (2 noise evals/pixel).

**~~3. Multi-palette blending~~** — DONE (commit 5060701). Two palettes (warm campfire, hot white-blue) in `particleColor()`. Blended by smoothed `energy * rhythmStrength` via `paletteBias_` (low-pass, ~0.5s time constant). Quiet = warm amber, loud rhythmic = white-hot.

**~~4. Audio-driven gamma curve~~** — DONE (commit 5060701). `powf(intensity/255, gamma)` remap before palette lookup. Gamma 1.3 (cool) → 0.7 (hot) driven by `paletteBias_`. More ember glow when loud, only brightest sparks when quiet.

##### Tier 2: High Impact, Medium Effort

**~~5. Curl noise wind field~~** — DONE
Replaced fbm3D offset-pair approximation in `MatrixForceAdapter::applyWind()` with proper Bridson curl noise: finite-difference curl of a shared scalar noise field. 4 `noise3D_01` evals per particle. Divergence-free velocity field → particles swirl around each other. Applied as advection (position displacement), not force. All generators on matrix layouts benefit (Fire, Water, Lightning).

**~~6. Energy → dynamic flame height~~** — DONE
Added dynamic kill in `Fire::updateParticle()`: `flameTop = height * (1 - (0.4 + 0.6 * energy))`. Particles above this threshold are killed. Quiet music = short flame (40% height), loud = full height. Also: background intensity now modulated by energy (`0.3 + 0.7 * energy`), so ember bed brightens with volume. Matrix-only (linear layouts unaffected).

**7. Spawn-on-death cascading embers**
When a high-intensity spark dies (boundary kill or max age), spawn 1-2 dim child embers with reduced velocity and longer lifespan. "Shower of sparks" effect.
- Child sparks: intensity 30-60 (deep red range), lifespan 1.5× parent, velocity 0.3× parent + random spread, type SLOW_EMBER.
- **Pool exhaustion guard**: max 2 child spawns per frame, only when `pool_.getActiveCount() < scaledMaxParticles() * 0.8`. Prevents cascade from starving beat-triggered spawns.
- Modify: `ParticlePool::updateAll()` callback in `ParticleGenerator::updateParticles()` — when particle dies, call `Fire::onParticleDeath()`. Or add a `deathCallback` to the pool template.
- More impactful with larger pool (#1 above) — 64 particles is too tight for cascading.

##### Tier 3: Medium Impact, Creative

**8. Ember pulsing**
Slow embers don't fade linearly — they pulse. Per-particle sinusoidal modulation in `renderParticle()`:
`renderIntensity = intensity * (0.7 + 0.3 * sin(age * freq + phase_offset))`
Requires adding a `phase_offset` field to `Particle` (or derive from spawn position). Creates "breathing coals" effect. Zero spawn/kill cost — purely cosmetic per-frame modulation.
- Can use existing `p->x + p->y` as phase seed (deterministic, no extra storage).

**9. Reaction-diffusion flame base (FitzHugh-Nagumo)**
Run a 1D reaction-diffusion system along the bottom row to modulate spawn intensity. Creates naturally-forming "flame tongues" that split, merge, and oscillate.
- Buffer: `float activator_[width_]` + `float inhibitor_[width_]` — 256 bytes at width=32, 128 bytes at width=16.
- Cost: ~width multiply-adds per frame. At 32-wide: negligible.
- Audio coupling: `energy` → reaction rate (faster = more tongue formation), `pulse` → inject activation spike at random position.
- Modify: `Fire::spawnParticles()` — multiply `spawnProb` by `activator_[x]` for each spawn position.

**10. Vortex filaments at flame base**
2 counter-rotating virtual vortices that alternate activation. Particles get rotational velocity kicks, creating S-curve flame shapes.
- Per vortex per particle: 1 sqrt + 1 division (distance and angle). At 192 particles × 2 vortices: 384 sqrt calls. ~0.1ms.
- Downbeat trigger: inject temporary vortex for 1s (mushroom-cloud bloom). Dramatic on 32×32 where the bloom has room to develop.
- Modify: `Fire::updateParticle()` — add vortex velocity contribution when `downbeatVelBoost_ > 0`.

#### Audio Signal → Fire Parameter Mapping (Current State)

| Signal | Current Implementation | Remaining Opportunities |
|--------|----------------------|------------------------|
| `phase` | Thermal buoyancy breathing, spawn pump, velocity boost, drag, wind breathing, background brightness | — (fully utilized) |
| `pulse` | Burst spark count, velocity boost, wind gust magnitude | — (fully utilized) |
| `downbeat` | Extra sparks, spread expansion (2.5×), color temp shift, velocity burst (1.5×) | Vortex injection (#10) |
| `energy` | Spawn rate, noise speed, wind turbulence, **palette blend (#3)**, **gamma (#4)**, **flame height (#6)**, **bg brightness (#6)** | — (fully utilized) |
| `rhythmStrength` | Organic/music blend (spawn, velocity, wind, type), **palette blend (#3)** | — (fully utilized) |
| `onsetDensity` | Lifespan modulation, noise speed, wind turbulence amplitude | Reaction-diffusion rate (#9) |
| `beatInMeasure` | Beat 1/3/2/4 accent patterns, left/right spawn rocking bias | — (fully utilized) |

#### Cleanup: ~~Remove Vestigial `FireDefaults`~~ — DONE (commit 15ce5c7)

Removed `FireDefaults` struct, `PropagationModel` hierarchy, `CenterSpawnRegion`, unused `SpawnRegion` methods, `Particle::trailHeatFactor`, `ParticleFlags::EMIT_TRAIL`, `Generator::coordsToIndex/indexToCoords`, `GeneratorType::CUSTOM`. 32 files, -787 lines. Particle struct 28→24 bytes.

#### References

- [Bridson SIGGRAPH 2007: Curl-Noise for Procedural Fluid Flow](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf)
- [Stefan Petrick: Self-Modulating Noise Fire Effect](https://gist.github.com/StefanPetrick/819e873492f344ebebac5bcd2fdd8aa8)
- [Inigo Quilez: Domain Warping](https://iquilezles.org/articles/warp/) — Noise domain warping for organic patterns
- [FastLED Fire2012 (Mark Kriegsman)](https://github.com/FastLED/FastLED/blob/master/examples/Fire2012/Fire2012.ino)
- [Fabien Sanglard: How DOOM Fire Was Done](https://fabiensanglard.net/doom_fire_psx/)
- [Andrew Chan: Simulating Fluids, Fire, and Smoke in Real-Time](https://andrewkchan.dev/posts/fire.html)

### Lightning → Plasma Globe Redesign

**Status: PLANNED**

The current Lightning generator produces stationary Bresenham line bolts that flash on and fade out in ~0.3s. This creates a strobe-like effect that is visually harsh at low resolutions. The goal is to replace it with a **plasma globe** aesthetic: persistent, flowing tendrils of light that sweep slowly and organically, always-on and never flashing.

**Why the current Lightning doesn't work:**
- **Zero motion**: Particles have `vx=vy=0`, bolts appear and die in place
- **Fast fade**: 30 intensity/frame = gone in ~8 frames (~130ms)
- **Discrete events**: beat → spawn bolt → fade → nothing. No continuity between events
- **MAX blending**: Creates harsh, clipped brightness peaks
- A plasma globe is the opposite: continuous, flowing, always-on

**Architecture: Extend Generator directly, NOT ParticleGenerator.** Plasma is a continuous field, not discrete particles. No particle pool, no spawn/kill lifecycle.

#### Layer 1: Plasma Background (Demoscene Sine-Wave Plasma)

Sum 3-4 sine waves at different frequencies/phases across the LED field. Map summed value through a purple/violet palette. Very cheap (~1 multiply + lookup per pixel), always-on dim glow.
- Audio: `energy` modulates brightness, `phase` shifts sine offsets for breathing

#### Layer 2: Noise-Field Tendrils (Simplex-Noise-Steered Paths)

3-6 persistent tendrils, each a path from center outward:
- At each step, sample `noise3D(x, y, time)` to determine direction bias
- Tendrils sweep slowly, guided by noise field evolution
- Brightness gradient: white core → lavender → deep violet → black
- ~60-120 `noise3D` calls per frame, well under 1ms on Cortex-M4F

#### Tier 1: Core Plasma (Biggest Visual Impact)

**1. Sine-wave plasma background with violet palette**
Sum 3-4 sine waves with different spatial frequencies and time-varying phases to produce a slowly-shifting organic glow across all pixels. Map through a 4-stop palette: black → deep violet → purple → magenta. ~1 multiply + 1 lookup per pixel.

**2. Noise-steered tendrils from center (3-4 initially)**
Each tendril: start at center, step outward. At each step, sample `noise3D(x, y, t)` to bias direction. Tendrils persist across frames (state: just angle + length), creating smooth sweeping motion. Core brightness white, fading to violet at tips.

**3. Core-to-edge brightness gradient**
Radial falloff from center outward. Center is always bright (white/lavender), edges dim (deep violet). Combined with tendril rendering, this creates the characteristic plasma globe depth.

#### Tier 2: Audio Reactivity (All Smooth, Never Flash)

**4. Phase-locked tendril breathing**
Tendril brightness and length pulse with `phase`. On-beat (phase=0): tendrils extend to full length, peak brightness. Off-beat: retract slightly, dim. Creates a rhythmic "pumping" without any flash.

**5. Energy → overall brightness modulation**
`energy` scales background plasma brightness and tendril intensity together. Low energy: dim ambient glow. High energy: vivid, saturated plasma. Always smooth — energy is already a slow-moving signal.

**6. Pulse → tendril extension**
On transient (`pulse`): tendrils momentarily reach further outward, with smooth ease-out return over ~0.3s. Creates "reaching" effect on kicks/snares without strobing.

#### Tier 3: Polish

**7. Mutual tendril repulsion**
Tendrils that are too close angularly repel each other, maintaining visual spread. Simple pairwise angular distance check, add small angular velocity away from neighbors. Prevents clustering on one side.

**8. Downbeat color warmth shift**
On bar 1 (`downbeat > 0.5`): core shifts slightly warm (white → pink/magenta tint), smooth decay back to white over 0.5s. Subtle but marks musical structure.

**9. Onset density → tendril count adaptation**
`onsetDensity` modulates active tendril count (3-6). Sparse ambient music → 3 lazy tendrils. Dense dance music → 5-6 active tendrils. Gradual transitions over 2-3 seconds, never instant add/remove.

#### Audio Signal → Plasma Parameter Mapping

| Signal | Plasma Parameter | Behavior |
|--------|-----------------|----------|
| `energy` | Background brightness + tendril intensity | Higher energy = brighter overall glow |
| `phase` | Noise time offset + sine phase shift | Breathing sync — tendrils pulse in phase with beat |
| `pulse` | Tendril length extension | Transient → tendrils reach further momentarily |
| `rhythmStrength` | Blend organic↔music mode | Low: slow random drift. High: phase-locked breathing |
| `onsetDensity` | Tendril count (3-6) | Sparse music → fewer tendrils. Dense → more |
| `downbeat` | Color warmth shift | Bar 1 → white core shifts slightly warm/pink |
| `beatInMeasure` | Tendril rotation bias | Different beats favor different angular sectors |

**Key principle: Nothing flashes or strobes.** Audio modulates continuous parameters (brightness, speed, spread, length) — never triggers discrete spawn/kill events.

#### Resource Budget

| Resource | Current Lightning | Plasma Globe | Notes |
|----------|------------------|-------------|-------|
| RAM | ~2 KB (40 particles) | ~600 bytes (tendril state + noise scratch) | 70% reduction |
| CPU | <1ms | ~1-1.5ms (noise3D + sine lookups) | Comparable |
| Flash | Minimal | Minimal | No model/tables needed |

#### Color Palette

4-stop gradient for tendril/background rendering:
- Stop 0: Black (0, 0, 0) — beyond tendril reach
- Stop 1: Deep violet (40, 0, 80) — distant plasma
- Stop 2: Lavender (140, 80, 200) — mid tendril
- Stop 3: White (255, 240, 255) — tendril core / center

#### References

- [Stefan Petrick: Noise-Field Fire/Plasma](https://gist.github.com/StefanPetrick/819e873492f344ebebac5bcd2fdd8aa8) — Self-modulating noise for organic flow
- [Demoscene Plasma Tutorial (Lode Vandevenne)](https://lodev.org/cgtutor/plasma.html) — Classic sine-sum plasma technique
- [Simplex Noise (Stefan Gustavson)](http://staffwww.itn.liu.se/~stegu/simplexnoise/simplexnoise.pdf) — Efficient 3D noise for tendril steering

### Water Generator Enhancement

**Status: PLANNED**

The water generator uses a 30-particle rain system with simplex noise background. Drops spawn from the top edge (matrix) or random positions (linear), fall under gravity, and splash radially on impact. The background is a thresholded noise field with blue/green coloring. Audio reactivity drives spawn rate via phase breathing and beat-triggered wave bursts.

**Current weaknesses:**
- **Sparse** — 30 particles on 128 LEDs creates rain, not ocean
- **No surface simulation** — no visible waterline, no wave motion, no body of water
- **No ripples** — drops splash radially but no expanding ring patterns
- **Fixed color** — no depth gradient, no mood shifts, no bioluminescence
- **Background disconnected** — noise layer doesn't interact with particles
- **Wind is dead** — `windBase=0`, no audio-driven gusts
- **No foam/whitecaps** — wave crests have no visual distinction
- **`beatInMeasure` and `onsetDensity` unused**

**Proposed architecture: Layered water system.** Replace single-layer particle rain with stacked composited layers for a richer scene. Keep ParticleGenerator base class (particles still useful for rain/splashes/bioluminescence).

#### Tier 1: High Impact, Low Effort

**1. Two-buffer ripple simulation**
Classic demoscene water algorithm. Two `int16` buffers, 5 operations per cell per frame. Audio events inject impulses, ripples propagate and interfere automatically. This single addition transforms sparse "rain" into "living water surface."
```
new[x] = ((prev[x-1] + prev[x+1]) >> 1) - current[x]
new[x] -= new[x] >> 5  // ~3% damping
swap(prev, current)
```
- Audio: `pulse` → drop injection, `beat` → larger impulse, `downbeat` → edge wave sweep
- Cost: 512 bytes RAM, ~2K cycles/frame

**2. Depth-gradient base coloring (matrix)**
Row-dependent base color before particles/noise: pale cyan at surface → turquoise → deep blue → near-black at bottom. Creates immediate sense of water depth with zero CPU cost (per-row tint lookup).
```
Row 0 (surface): (140, 220, 255) — pale cyan
Row 2-3:         (0, 180, 220)   — turquoise
Row 5-6:         (0, 30, 120)    — deep blue
Row 7 (bottom):  (0, 10, 60)     — near-black
```

**3. Bioluminescence on audio events**
Blue-green glow (RGB ≈ 0, 80-255, 100-200) triggered by `pulse`. Glow appears at random position, persists 0.5-1s with exponential decay. Maps perfectly to transients — water "lights up" on kicks/snares without strobing.
- Audio: `pulse` → spawn glow, `energy` → glow brightness, `downbeat` → large area glow burst
- Cost: 128-byte glow buffer, ~500 cycles/frame

**4. Phase-driven wave speed breathing**
Modulate noise time advancement with `phase`: `noiseTime += baseSpeed * (0.5 + 0.5 * phaseToPulse())`. Water flows faster on-beat, slower off-beat. Creates rhythmic breathing without flash. Currently noise speed only varies by energy (0.012-0.05).

#### Tier 2: High Impact, Medium Effort

**5. Gerstner wave surface (matrix)**
3 summed trochoidal waves create rolling ocean surface with sharp crests and flat troughs. On 16×8: illuminate the row nearest the surface height with max brightness, exponential falloff below. Surface highlights at crests where slope changes sign. Sharp crests + flat troughs = unmistakably "water" even at low res.
```
x_disp = (steepness/k) * cos(k*x0 - w*t)
y_disp = (steepness/k) * sin(k*x0 - w*t)
```
- Audio: `energy` → wave amplitude, `phase` → wave speed, `downbeat` → inject large swell
- Cost: ~50K cycles/frame (6-8 trig calls per LED)

**6. Foam/whitecap at wave crests**
When surface height or turbulence (large neighbor height differences) exceeds threshold, blend toward white. Foam persists via separate buffer with slow decay (~1s half-life). Drifts with surface motion.
- Audio: `energy` → foam threshold (more energy = more foam), `rhythmStrength` → foam regularity
- Cost: 128 bytes RAM, ~500 cycles/frame

**7. Domain-warped noise background**
Replace current simplex noise with domain-warped noise: `noise(x + noise(x,y,t), y, t)`. Creates organic swirling currents and eddies instead of drifting blobs. Dramatically more organic.
- Audio: `energy` → warp amplitude, `phase` → time offset
- Cost: 2-3× current noise cost (~100K cycles), still under 0.2% CPU

**8. Onset density → rain intensity mapping**
`onsetDensity` (currently unused) drives rain character:
- Sparse (0-1/s): 1-2 drops/sec, gentle ripples, calm water
- Moderate (2-4/s): 5-10 drops/sec, overlapping ripples
- Dense (4-6/s): 15+ drops/sec, chaotic interference, foam activation
- Auto-matches rain to music density without manual tuning

#### Tier 3: Medium Impact, Creative

**9. Caustic overlay (matrix only)**
Animated Voronoi noise or `abs(noise1 + noise2)^0.5` creates underwater "swimming light" pattern. Applied as additive cyan/blue layer under particles. Per-cell: 9 neighbor checks × (2 sin + 1 sqrt + compare).
- Audio: `energy` → caustic brightness, `phase` → animation speed
- Cost: ~100-140K cycles/frame (~0.2% CPU)

**10. Multi-palette color system**
3 palettes via Inigo Quilez cosine formula `color(t) = a + b * cos(2π(c*t + d))`, blend by audio state:
- **Deep ocean** (low energy): dark blues, near-black
- **Tropical** (moderate energy): turquoise, bright cyan
- **Storm/moonlit** (high energy + high rhythm): silver/white highlights, dark base
- Blend factor: `energy × rhythmStrength`

**11. Ridged noise wave crests**
`1.0 - abs(noise3D(x, y, t))` creates sharp bright ridges with smooth dark valleys — looks like wave crests catching light. Can replace or blend with the current thresholded noise background.

**12. Drop trails (motion blur)**
Render falling drops as 2-3 LEDs with decreasing brightness along velocity vector. Makes individual drops visible at low resolution and adds sense of speed. Just render at `(x, y)`, `(x-vx*dt, y-vy*dt)`, `(x-2*vx*dt, y-2*vy*dt)` with 100%, 50%, 25% intensity.

**13. Beat-driven swell accumulator**
On each beat, add 0.1 to a "swell" variable that decays at 0.02/frame. Swell increases wave height and brightness. Multiple beats accumulate into growing swell that subsides between phrases. Makes musical sections feel like rising/falling seas.

#### Audio Signal → Water Parameter Mapping

| Signal | Current Use | Proposed New Mappings |
|--------|-------------|----------------------|
| `energy` | Weak spawn rate modifier (0.5-1.0×) | Wave amplitude, caustic brightness, foam threshold, color temperature, rain intensity |
| `phase` | Spawn breathing (0.4-1.0×) | Wave speed breathing, noise time modulation, background brightness |
| `pulse` | Beat → wave burst, organic transient drops | Ripple buffer injection, bioluminescence spawn, swell accumulation |
| `rhythmStrength` | Organic↔music blend | Foam regularity, rain pattern (random vs phase-locked), color palette blend |
| `downbeat` | Extra wave drops | Edge wave sweep, large bioluminescence burst, swell spike, palette warmth shift |
| `onsetDensity` | *Unused* | Rain intensity/character, turbulence level, ripple spawn rate |
| `beatInMeasure` | *Unused* | Wave direction bias (left/right alternation), accent ripple size |

**Key principle: Water absorbs energy into continuous flow.** Unlike fire (discrete sparks) or plasma (continuous tendrils), water should respond through amplitude and speed modulation — bigger waves, faster flow, more interference — not through discrete flashes.

#### Resource Budget

| Component | RAM | CPU (128 LEDs) |
|-----------|-----|----------------|
| Ripple buffers (2 × 128 × int16) | 512 bytes | ~2K cycles |
| Foam buffer (128 × uint8) | 128 bytes | ~500 cycles |
| Bioluminescence buffer (128 × uint8) | 128 bytes | ~500 cycles |
| Gerstner waves (3 waves) | — | ~50K cycles |
| Domain-warped noise | — | ~100K cycles |
| Caustics (Voronoi) | — | ~140K cycles |
| Particle pool (existing, 30) | ~600 bytes | ~5K cycles |
| **Total new** | **~1.5 KB** | **~0.25% CPU** |

All layers combined use under 0.3% CPU at 60 Hz. Enormous headroom on the nRF52840.

#### References

- [Demoscene 2D Water Effect (Hugo Elias)](https://web.archive.org/web/20160418004149/http://freespace.virgin.net/hugo.elias/graphics/x_water.htm) — Two-buffer ripple simulation
- [Catlike Coding: Flow / Waves](https://catlikecoding.com/unity/tutorials/flow/waves/) — Gerstner wave implementation
- [Inigo Quilez: Domain Warping](https://iquilezles.org/articles/warp/) — Noise domain warping for organic patterns
- [Inigo Quilez: Cosine Palettes](https://iquilezles.org/articles/palettes/) — Parametric color gradients
- [Lode Vandevenne: Plasma Tutorial](https://lodev.org/cgtutor/plasma.html) — Sine-sum interference patterns
- [The Book of Shaders: Voronoi](https://thebookofshaders.com/12/) — Animated Voronoi for caustics

## Design Philosophy

See **[VISUALIZER_GOALS.md](VISUALIZER_GOALS.md)** for the guiding philosophy. Key principle: visual quality over metric perfection. Low F1 on ambient/trap/machine-drum may represent correct visual behavior (organic mode fallback). False positives are the #1 visual problem.

## Key References

**Beat tracking (general):**
- BEAST: Streaming Transformer (ICASSP 2024)
- BeatNet+: CRNN + particle filter (TISMIR 2024, Heydari et al.)
- RNN-PLP-On: Real-time PLP beat tracking (TISMIR 2024, Meier/Chiu/Muller)
- Novel-1D: 1D state space with jump-back reward (ICASSP 2022, Heydari et al.)
- Percival 2014: Enhanced ACF + pulse train evaluation (IEEE/ACM TASLP)
- Krebs/Bock/Widmer 2015: Efficient state-space for joint tempo-meter (ISMIR)
- Davies 2010: Beat Critic octave error identification (ISMIR)
- Scheirer 1998: Comb filter bank tempo estimation

**Frame-level / embedded NN (Priority 1 references):**
- BeatNet (Heydari et al. 2021): CRNN frame-level activation + particle filter post-processing. Proven frame-level paradigm.
- Beat This! (2024): CNN + Transformer, frame-level mel activation. SOTA offline.
- madmom (Böck et al. 2016): Bidirectional LSTM on mel spectrogram, frame-level beat/downbeat activation.
- Krebs/Böck/Widmer 2016: Beat-synchronous downbeat RNN — the only beat-level approach, but requires bidirectional GRU + DBN. F1 drops from 90.4% → 77.3% with estimated beats.
- Gkiokas et al. 2017: CNN Beat Activation Function for dancing robot on ARM Cortex-A8. Practical embedded beat tracking with HW constraints.
- No published TinyML beat tracking on Cortex-M class hardware exists (as of March 2026).

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | Design philosophy — visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |

## OTA Firmware Update Safety (March 31, 2026)

### Current State

**UF2 (USB mass storage):** 100% safe. Invalid/corrupt files silently rejected. Bootloader protects itself. Old firmware survives interrupted copy. No bricking risk.

**BLE DFU (wireless):** Reliable with caveats. Legacy DFU (SDK v11) is single-bank — START_DFU erases application flash BEFORE data transfer. If transfer fails, device has no app and stays in bootloader. Mitigations implemented:
- Pre-flight BLE quality check (RSSI + NUS round-trip test) blocks DFU if connection weak
- 3 retry attempts with SYSTEM_RESET recovery between attempts
- Speculative VALIDATE recovers from lost BLE completion notification
- HCI adapter reset clears corrupted BlueZ state
- Fleet manager auto-retries DFU_RECOVERY devices with exponential backoff
- Device stays in DFU bootloader indefinitely — never permanently stuck, just waiting for firmware

**Remaining risk:** A device that fails all DFU retries stays in bootloader with erased flash. The fleet manager will keep retrying (exponential backoff: 1 min, 2 min, 4 min, 8 min). The only scenario requiring physical USB access is a dead BLE radio (hardware failure).

### Roadmap: QSPI Dual-Bank OTA (eliminates destructive flash erase)

**Goal:** New firmware written to XIAO's 2 MB QSPI flash as a staging area. Old firmware stays intact in internal flash. Only after complete transfer + CRC validation does the bootloader erase and copy. Failed transfer = device boots old firmware.

**Flash layout:**

| Region | Location | Size | Purpose |
|--------|----------|------|---------|
| MBR + SoftDevice | Internal 0x00000-0x27000 | 156 KB | Protected, never erased |
| Application (Bank 0) | Internal 0x27000-0xF4000 | 820 KB | Running firmware |
| Bootloader | Internal 0xF4000-0x100000 | 48 KB | Protected, handles DFU |
| Staging (Bank 1) | **QSPI 0x000000-0x200000** | **2048 KB** | New firmware staged here |

**Why internal dual-bank doesn't work:** Our firmware is 510 KB. Internal dual-bank splits 780 KB usable into two 388 KB banks. 510 KB > 388 KB — doesn't fit.

**Implementation (requires custom bootloader):**
1. Add QSPI driver to Adafruit bootloader (nrfx_qspi, minimal init)
2. Modify `dfu_dual_bank.c`: redirect Bank 1 writes to QSPI flash
3. After transfer + CRC pass: erase internal Bank 0, copy from QSPI
4. If transfer fails: QSPI staging area is discarded, internal app intact
5. Test exhaustively on bare chips before fleet deployment

**Effort:** Medium-high. Custom bootloader work (~1 week). Risk: bootloader bugs require SWD recovery. Mitigated by testing on bare test chips first (reset button + SWD pads accessible).

**Alternative: MCUboot.** Full bootloader replacement with automatic rollback support. Requires migration from Arduino to Zephyr RTOS. Not feasible without rewriting the entire firmware stack.

**Priority:** Low until a device is permanently lost to failed BLE DFU. Current mitigations (pre-flight check + auto-retry) make this extremely unlikely.
