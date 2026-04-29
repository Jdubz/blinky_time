# ML Training Improvement Plan

> **2026-04-27 corpus reset:** `edm_holdout/` (the GiantSteps LOFI adversarial corpus) was deleted entirely. Multiple regressions came from using adversarial content as the headline F1 metric and chasing model fixes for problems that were partly content-difficulty. **The only validation corpus going forward is `blinky-test-player/music/edm/` (18 tracks).** All historical F1 numbers in this doc that mention `edm_holdout` should be read as "measured on adversarial content that no longer informs decisions." See CLAUDE.md "CRITICAL: Validation Corpus".
>
> The GiantSteps LOFI tracks are still in the *training* corpus (they always were — `Makefile` symlinks them via `giantsteps-tempo-dataset/audio/*.mp3`); they're no longer treated as a separate evaluation set. The held-out role moves to a tier-1 subset of `edm/` once tier-1 is defined.

> **2026-04-24 audit**: v30's on-device failure triggered a deep audit of the entire audio analysis stack. 24 findings across training data prep, training loop, TFLite export, firmware audio path, and validation observability. Detailed in `docs/AUDIO_SYSTEM_AUDIT_2026_04_24.md`. Fix plan tracked as tasks #77-#88. Key items that would have caught v30 automatically: activation-distribution logging (training + validation), peak-picked val F1, FP32 output quantization investigation.

> **Pre-v30 F1 numbers on edm/ are training-contaminated upper bounds.** The v29 "ungated baseline on edm/" recorded in this doc (P=0.55, F1=0.547) is the cleanest pre-fix deployment-representative number we have; interpret with a contamination discount until clean edm/ results replace it.

## 2026-04-22 — focal-loss regression + v29 reset

**Short version.** The last three model generations (v26, v27-hybrid, v28_mel_only) all have ~0.25 val precision / ~0.97 recall. Investigation traced this to a loss-function change at v26. Fix is a revert on a single new config (`v29_mel_only`). No hardware or label-pipeline changes required.

### The regression

Precision across historical runs (val set, frame-level, threshold 0.5):

| Run | Loss | P | R | F1 |
|-----|------|---|---|----|
| v21-mel80 | `bce` | **0.47** | 0.67 | **0.55** |
| v23-micprofile | `bce` | 0.42 | 0.62 | 0.50 |
| v26-focal | `asymmetric_focal γ-=4` | 0.23 | 0.99 | 0.37 |
| v27-hybrid | `asymmetric_focal γ-=4` | 0.27 | 0.95 | 0.42 |
| v28_mel_only | `asymmetric_focal γ-=4` | 0.25 | 0.97 | 0.39 |

v21/v23 used plain weighted BCE. v26 switched to `asymmetric_focal` (Imoto & Mishima 2022) with `gamma_neg=4.0`. Precision halved immediately; recall saturated near 1.0. Every subsequent model inherited the same loss.

### Why it broke

Two cascading configuration issues:

**1. Label inflation** — training log shows `Positive ratio: 0.2102`. Measured directly on `Y_train.npy`:

| Y value | % of frames |
|---------|------------:|
| 0.00 (silence) | 78.79% |
| 0.25 (±1 neighbor of an onset) | 14.06% |
| 1.00 (onset center) | 7.15% |

With `hard_binary_threshold: 0.1`, the 14.06% of 0.25-valued neighbor frames get promoted to 1.0 → 21% positive rate. True onset centers are only 7.15%. Each onset thus produces a 3-frame-wide positive plateau.

**2. Loss misconfigured for this label density** — `asymmetric_focal` with `gamma_neg=4.0` downweights easy negatives by `(1-p)^4`. Safe at <1% positive rate (what the paper assumed); catastrophic at 21%. The loss surface has almost no gradient telling the model "don't fire here" for the vast majority of non-onset frames. Combined with `pos_weight` auto-set to 3.8× on positives, training drives outputs high everywhere.

Val metric compounds the confusion: P/R is computed frame-by-frame at sigmoid > 0.5, not at peak-picked firings. A 3-wide plateau + a 3-wide label region can look like "lots of TPs and FPs" in the frame count even when on-device peak-picking would collapse them correctly. But gate (b) analysis (HYBRID_FEATURE_ANALYSIS_PLAN.md §"Gate (b) result — b137 held-out") measured **2953 TP / 8991 FP** peak-picked firings on v27 → 24.7% device-level precision, matching the val number. Peak-picking wasn't saving us; the model is genuinely firing 4× too often.

### What is NOT broken

Checked and ruled out during the investigation:

- **Dedup.** Labels still have 0 close pairs <15 ms across 30 random tracks. Median 5.24 onsets/sec is genuine drum density.
- **Label source.** `generate_kick_weighted_targets.py` runs librosa `onset_detect` on bandpass-filtered drum stems (kick <200 Hz, snare 200-4000 Hz, hihat >4000 Hz). `merge_tol=0.03s` dedupes cross-band firings. Hihats weighted 0 → excluded. Kicks + snares weighted 1.0.
- **Frame rate math.** 4.88 non-hihat onsets/sec × 1 frame / 62.5 Hz = 7.8% positive, matches the 7.15% measured at `y > 0.5`.
- **Dataset/feature pipeline** (audit during v28 prep). Mel bands, hybrid-feature storage, augmentation, feature-index selection all verified.

### v29 fix

Single new config `configs/conv1d_w16_onset_v29_mel_only.yaml`, three changes from v28:

| Field | v28 | v29 |
|-------|-----|-----|
| `loss.type` | `asymmetric_focal` | `bce` |
| `labels.neighbor_weight` | 0.25 | 0.0 |
| `labels.hard_binary_threshold` | 0.1 | 0.5 |

Result: sharp 1-frame labels, ~7% positive rate, auto `pos_weight` ~13×, plain weighted BCE. Matches the v21/v23 recipe that produced P=0.47.

Data reuse: config points at the existing `processed_v28` directory. The `neighbor_weight` and `hard_binary_threshold` settings apply at load time (not at prep time), so no re-prep is needed. ~11 h retrain on the current GPU.

Expected: val P ≈ 0.45, F1 ≈ 0.55, on-device FP rate roughly 1/3 of v27.

### v28_mel_only held-out results (25 tracks × 4 devices, 2026-04-22)

Peak-picked, 100 ms tolerance against `.beats.json` ground truth on `blinky-test-player/music/edm_holdout/`:

| Metric | v27-hybrid (prior) | v28_mel_only | Δ |
|--------|-------------------:|-------------:|---:|
| Mean F1 (100 ms tol) | 0.398 | **0.489** | +0.091 |
| Mean precision | 0.247 | **0.381** | +0.134 |
| Mean recall | ~0.95 | 0.754 | −0.20 |
| FP per TP | 3.04 | **1.63** | −1.41 |
| F1 @ 50 ms | — | 0.337 | — |
| Mean latency | — | +7 ms (median +9 ms) | — |

**This is the largest F1 jump on held-out we've ever measured — and it came from *removing* the v27 hybrid features (flatness, raw_flux), not from changing the loss or the labels.** v28_mel_only uses the same broken focal loss as v27; the +0.09 F1 / +0.13 P gain is entirely attributable to the mel-only input.

Implication: the v27-hybrid add of `[flatness, raw_flux]` — which went in without a mel-only ablation — was a **regression**, consistent with the gate-c finding that flatness has R² = 0.85 vs mel (the NN was being fed ~85%-redundant information as an extra channel). This validates `HYBRID_FEATURE_ANALYSIS_PLAN.md`'s §"Don't stack without ablation" warning retroactively.

Device variance (4 physically identical XIAO nRF52840 Sense):

| device | P | R |
|--------|---:|---:|
| 062CBD12EB69 | 0.37 | **0.90** |
| 2A798EF8E684 | 0.37 | 0.79 |
| 659C8DD3ADF8 | 0.39 | 0.69 |
| ABFBC41283E2 | 0.40 | **0.64** |

Recall spread of 26 pp across identical hardware is mic / enclosure sensitivity, not a training problem. Track as a separate calibration task.

Best → worst track spread: F1 0.69 (3642438.LOFI, four-on-the-floor techno) → F1 0.26 (210560.LOFI, sparse). Sparse-percussion failure mode is not addressed by v29.

Raw artifact: `/tmp/val_v28_mel_only.json` (full job object, blinkyhost job `bc7d2c080561`). No `persist_raw:true` capture this run — re-run with that flag if we want gate-b numbers on v28.

### v29_mel_only held-out results (2026-04-23)

After a false start that stacked loss + label changes in one config (killed at ep14 after F1 plateaued at 0.28), the correct single-variable experiment ran: **only** `loss.type: asymmetric_focal → bce`. All other v28 settings (neighbor_weight=0.25, hard_binary_threshold=0.1) kept identical.

| metric | v27-hybrid | v28_mel_only | **v29_mel_only** |
|--------|---:|---:|---:|
| **Val F1** (frame) | 0.416 | 0.391 | **0.542** |
| **Val P** (frame) | 0.266 | 0.245 | **0.467** |
| **Held-out F1** (peak-picked) | 0.398 | 0.489 | **0.484** |
| **Held-out P** (peak-picked) | 0.247 | 0.381 | **0.384** |
| Held-out R | ~0.95 | 0.754 | 0.727 |
| FP per TP | 3.04 | 1.63 | 1.61 |

**The loss fix was val-metric cosmetic, not on-device real.** Peak-picking normalizes out the "focal-broad-plateau" vs "BCE-sharp-peak" output-shape difference. Same underlying discrimination → same on-device P. The v26 regression existed in val-set P/R but was invisible in on-device behavior once peak-picking collapsed plateaus.

**The v27→v28 +0.091 F1 gain (removing hybrid features) was the actual lever** and it's locked in from v28 onward.

### Disproven direction: wider input window

Confirmed 2026-04-23 by reviewing past experiments. **Widening `window_frames` does nothing.**

| Experiment | Input window | RF | Val F1 |
|-----------|-------------|----|-------:|
| v21-mel80 (baseline) | W16 = 256 ms | 9 frames = 144 ms | 0.552 |
| exp-w32 | W32 = 512 ms | 9 frames = 144 ms | 0.468 |
| exp-wide-48-48-32 | W32 = 512 ms | 11 frames = 176 ms | 0.481 |
| exp-wide-w32 | W32 = 512 ms | 11 frames = 176 ms | 0.481 |

`window_frames` is the input buffer — the *receptive field* (what the model actually uses at the current output frame) is set by `kernel_sizes × layers`. With `kernels=[5,5]`, the output depends on the last 9 input frames regardless of whether the buffer is 16 or 32. The extra frames in W32 input are *ignored by the convolutions*. The deeper 3-layer `[48,48,32]` variant buys only +2 RF frames at 2.4× the parameter count, with no measurable F1 gain.

**Operational reasoning this direction is off the table:**

1. **Onset identification needs transient info from 10-30 ms**, not 500 ms of history. More RF extends into the *past* (tail of the previous event), not the attack we're trying to detect.
2. **Latency matters.** Each frame of extra RF is another frame of wait-before-firing. For LED-reactive visuals, a 50 ms vs 150 ms latency is visible to the human eye.
3. **Inference cost.** Deeper models cost CPU cycles per frame. Shortening the window is on the long-term wishlist, not widening.

Dilated Conv1D (configs exist: `deep.yaml`, `ds_tcn.yaml`, `wider_rf.yaml`) **was never actually trained** despite configs being present. Same reasoning applies — dilation extends RF into the past, which is precisely what this task doesn't need.

### What could actually improve on-device precision at 0.38

Gate-b's 2026-04-20 finding: at firing moments, no deterministic shape feature's |d| exceeds 0.10 between TPs and FPs. The NN is firing on events that look spectrally identical to onsets in the current 256 ms window. More RF won't help; dilation won't help; the information isn't there.

Candidate directions (in descending expected impact, none yet tried):

1. **Beat-phase prior as NN input** — firmware already tracks tempo via PLP/CBSS. Feeding "phase within the current beat" as a 1D scalar channel gives the NN a musical prior it cannot extract from raw mel. Rhythmic kicks on the grid score high; isolated FPs score low. One channel, no RF change.
2. **Time-since-last-firing prior** — scalar input channel "ms since the previous onset we fired on." Rhythmic patterns have regular spacing; FPs are clustered or sparse. Captures isolated-vs-clustered directly.
3. **Bass-band ratio feature** — mel band 0 (40-100 Hz) vs mel band 15 (400-800 Hz) ratio at the current frame. Kicks concentrate energy in <100 Hz; tonal events don't. Kept intra-frame (no RF impact).
4. **Device-specific gain calibration** — the 26 pp recall spread across identical devices (062CBD R=0.89, ABFBC4 R=0.64 on v29) is mic/enclosure, not training. Worth a separate track.

Characteristics these all share that the rejected directions don't: they add *discriminative information* at the onset moment rather than more context around it.

### Blocks currently in flight

1. **v28 single-feature ablation queue** (`v28_mel_crest`, `v28_mel_raw_flux`, `v28_mel_hfc`, `v28_mel_flatness`) — **cancelled.** F1 numbers measured against a broken baseline would be uninterpretable. See §"Gate (e) blocked — 2026-04-22" in HYBRID_FEATURE_ANALYSIS_PLAN.md.
2. **v29_mel_only** — trained, deployed, held-out validated. Becomes the new production baseline, but does not improve on-device performance over v28_mel_only. Kept for code hygiene (correct loss) but not deployed.

## 2026-04-23 — reframing the problem: work with the model, not against it

After v29's on-device F1 came in effectively identical to v28 (loss fix was val-metric-only), a deeper investigation was triggered: *if the model genuinely cannot distinguish percussion from melodic impulses from deterministic shape features, how do we work with that constraint rather than fight it?*

Four parallel research threads ran. Summary, then the plan.

### Thread 1: Is the problem the labels?

Current training-label pipeline:
```
full mix → demucs drum stem → bandpass (kick<200 Hz, snare 200-4000 Hz) → librosa onset_detect → label
```

**Finding: kick_weighted_drums labels are contaminated by demucs stem bleed.**

- **32% of tracks have >0.7× bleed ratio** — substantial >200 Hz energy in the "drum" stem, typical for electronic music with 808s / sub-bass / tonal percussion where bass and kick are intentionally harmonically coupled (HTDemucs can't separate them cleanly on such content).
- **Kick-band vs snare-band energy correlation is high** on 10% of tracks (>0.6) — when snare spikes, the kick band also spikes. The bandpass filter can't undo that coupling.
- **2× the label density of consensus labels on the full mix** (152.8 events/track for kick_weighted vs 75.9 for consensus_v5). A clean drum stem + sparse kicks shouldn't produce 2× the onset count of a 7-system mix-level consensus.
- **Track 000397** spot-check: labeled "kicks" show only **0.50 dB** low-frequency energy spike at the labeled frame — essentially noise-level, meaning the label fires on tonal artifacts that aren't real kicks in the mix.
- **5.2% of tracks** (353/6,751) marked `skipped: true` for silent drum stems — failed separation, explicitly filtered. But passing the RMS check doesn't mean the separation was *clean*.

**Mechanism that matches gate-b's null result.** The model learns to fire on the acoustic signatures its training labels call "kicks" — which include bleed artifacts that aren't actually percussion. At inference, the model continues firing on those same tonal signatures → FPs. Gate-b measured no shape feature separates TP from FP because at the label level, many "TPs" are mis-labeled tonal events that look spectrally identical to the "FPs." **Our 0.38 on-device precision is label-limited, not architecture-limited.**

### Thread 2: Are better labels already on disk?

Yes, partially.

- `consensus_v5/` (6,993 files, freshest consensus dir): 7-system **metrical beat** consensus, ~2.2 onsets/sec. Sparse. Beat-level, not onset-level. Not what we want.
- **`onsets_consensus/` (6,993 files): 5-system acoustic onset consensus on full mix, ~3.9 onsets/sec.** This IS what we want. Covers drums + melodic transients on the full mix, no stem separation, so no bleed artifacts.
- Already used in v11-v19b training runs via `labels_type: "onset_consensus"`. v19-baseline got val P=0.28 F1=0.34 — lower-looking, but **not comparable** to kick_weighted val numbers (different task definition; broader target).

### Thread 3: What about rhythmic filtering in firmware?

Firmware already has everything needed.

- **PLP/CBSS fully implemented** in `AudioTracker.cpp`. `getPlpPhase()`, `getPlpConfidence()`, `getPeriodicityStrength()`, `getPlpNNAgreement()` all available every frame.
- **Three onset gates already exist** in `updatePulseDetection()`:
  1. Bass-band energy gate (hi-hat suppression via inverse bass-ratio scaling)
  2. PLP pattern bias (soft 30% threshold off-beat when `plpConfidence > 0.3`)
  3. Crest-factor gate (disabled by default)
- **Gate insertion point is clear at `AudioTracker.cpp:903`** — current fires when `signalPresence > pulseMinLevel AND isLocalMax AND cooldownOk AND crestOk`. A ±50 ms beat-grid AND-condition inserts cleanly here.
- **ACF warmup takes ~1.6 s** (`ossCount >= 100` at 66 Hz); confidence EMA-smooths with half-life ~200 ms. Need `rhythmStrength > 0.2` guard to avoid suppressing startup and ambient/sparse content.
- **No re-architecting required.** `plpPhase_` is updated every frame before `updatePulseDetection()` is called; the AND-gate is a pure logical condition.

### Thread 4: Multi-channel instrument model

Almost fully built, never deployed.

- `labels_type: "instrument"` in `prepare_dataset.py` — works; produces `(n_frames, 3)` with channels = [kick, snare, hihat], each binary
- `train.py` — handles multi-channel targets with per-channel auto `pos_weight` and per-channel loss via broadcasting
- `FrameOnsetNN.h` — supports 1–4 output channels; extraction method `extractOutput(channel)` works
- `export_tflite.py` — `num_output_channels` parameter wired end-to-end
- `evaluate.py` — per-channel F1 scoring implemented
- `v8.yaml` config exists with `num_output_channels: 3` — but **never trained**, no `outputs/v8/` directory
- **Missing piece**: `AudioTracker::updatePulseDetection()` only reads channel 0. Adding multi-channel routing (kick→bass pulse, snare→mid flash, hihat→ignore) is 2-3 days of firmware integration.
- **Total cost to ship**: 1-2 days ML + 2-3 days firmware = 3-5 days. **Depends on clean labels** — contaminated kick/snare labels hurt this path equally.

### The plan, revised 2026-04-23 after gate experiment and corpus-exclusion fix

#### v29 ungated baseline on edm/ (PRIMARY) — 2026-04-23

Measured via `beatgridmin=0.0` branch of the threshold sweep on 18 edm/ tracks:

| corpus | n | P | R | F1 | FP/TP |
|--------|--:|--:|--:|---:|------:|
| **edm/ (primary, representative)** | 18 | **0.55** | **0.58** | **0.547** | 0.82 |
| edm_holdout (secondary, adversarial) | 25 | 0.38 | 0.73 | 0.484 | 1.61 |

This is our real v29 number. edm/ precision is meaningfully higher than the edm_holdout number we'd been using for decisions (Δ = +0.17 P, +0.06 F1). The edm_holdout underestimates deployment performance, which is why VISUALIZER_GOALS §5 explicitly says low F1 on ambient/sparse content is often correct behavior.

**Caveat:** v29 training included the 18 edm/ tracks (same-name, byte-identical audio leaked from prior corpus merge, see 2026-04-20 note in HYBRID_FEATURE_ANALYSIS_PLAN.md). The edm/ number above is training-contaminated. v30 is the first model prepared with a genuine exclude of edm/ and edm_holdout — its numbers will be the first clean primary-corpus measurement.

#### ~~Step 1: firmware PLP AND-gate~~ — tried, empirically dead (2026-04-23)

Built (b141, b142, b143), tunable `beatgridmin` exposed via SerialConsole, full threshold sweep on edm/ via `/api/test/param-sweep`:

| beatgridmin | F1 on edm/ (18 tracks) |
|------------:|-----------------------:|
| **0.00 (disabled)** | **0.547** |
| 0.10 | 0.350 |
| 0.15 | 0.317 |
| 0.20 | 0.310 |
| 0.25 | 0.220 |
| 0.30 | 0.236 |
| 0.40 | 0.200 |

**Every non-zero gate value regresses F1.** Diagnostic `plpAtTransient = 0.13-0.15`: the PLP pattern amplitude at the moment the NN fires is typically 13-15% (near pattern *minimum*, not peak). So any threshold ≥ 0.1 rejects most firings, including the true ones.

Root cause: PLP's predicted accent position is not phase-aligned with actual kick onsets. Possible explanations include epoch-fold amplitude-range compression, bar-length ACF period with multiple accent positions per period, and simple timing offset between the subsystems. We don't need to isolate which — the outcome is the same: **the PLP beat grid is not reliable enough to gate NN firings on, at any threshold.**

The hard AND-gate design was also fundamentally wrong for musical-change robustness: verse→chorus transitions have legitimate rhythmic shifts where PLP hasn't re-locked yet, and a hard gate would silence exactly those. Flexibility and adaptability are core system goals; hard thresholds are the opposite of that.

Infrastructure is kept in place (tunable registered, code path present, default `beatGridPatternMin = 0`) so the gate can be re-tested if PLP ever gets a significant accuracy improvement. Do not ship enabled.

#### ~~Step 1b: soft `patternBias` sweep~~ — deprioritised (2026-04-23)

The existing soft `patternBias` (`AudioTracker.cpp:873-887`) uses the same `plpPulseValue_` with a 30% threshold increase at off-beat positions when confident. Given the gate sweep showed PLP and NN firings aren't aligned, raising the soft-bias coefficient would have the same directional bias. Parametric sweep deferred until PLP accuracy is measured.

#### Step 2 (in flight, ~11 h training after prep): v30 on `onsets_consensus` labels

Addresses the Thread 1 label-contamination finding directly. Config `configs/conv1d_w16_onset_v30_mel_only.yaml`:
- `labels_type: "onset_consensus"` (5-system acoustic consensus on full mix)
- `onset_consensus_dir: /mnt/storage/blinky-ml-data/labels/onsets_consensus`
- Same loss (bce), same architecture (Conv1D W16 30 mel) as v29
- `--exclude-dir ../edm --exclude-dir ../edm_holdout` (fixed argparse to accept both)

**Product reframing**: v30 detects *any musical transient* on the full mix, not just kicks+snares on a bleed-contaminated drum stem. Consistent with VISUALIZER_GOALS §3: rhythmic filtering happens downstream in firmware; the NN's job is robust transient detection. Genre stabs, chord changes, and bass onsets now become TPs by definition, eliminating a large class of "FPs" that were really labelling errors.

Target: edm/ F1 ≥ 0.60, P ≥ 0.55 (first clean held-out number).

#### Step 3 (new priority): PLP-as-training-input (was Step 4)

The gate experiment proved PLP is not usable as a post-filter. But the *concept* — "use rhythmic context to disambiguate TPs from FPs" — is still right. The correct implementation is to feed PLP features (phase + confidence + pulse value) as NN input channels rather than bolt them on as a post-filter. The NN learns per-sample when to trust them. Section changes handle gracefully: when PLP confidence drops, the NN can learn to ignore the PLP channel and fall back to mel-only reasoning. Flexibility and adaptability built in.

Cost: 1-2 weeks. Requires porting the firmware's PLP/ACF math to Python for training-time parity (same gap-4 class of issue we had with flatness/flux). `librosa.beat.plp()` is NOT a drop-in — byte-parity with the firmware matters.

Blocked on v30 result — if v30 alone reaches target on edm/, this may not be needed.

#### Step 4 (contingency, formerly Step 3): v31 on madmom-only full-mix labels

Only if v30 on `onsets_consensus` labels also shows contamination or low ceiling. Single-system madmom CNN onset detector on full mix, no stem separation. Existing `teacher_soft_dir` infrastructure can load madmom activations.

#### Step 5 (contingency): multi-channel instrument model

70% of infrastructure already built (see Thread 4). Missing piece is AudioTracker multi-channel routing (~2-3 days). Pursue only if Steps 2-4 don't reach the precision target on edm/.

## 2026-04-24 evening — v30/v31 collapse investigated; v32 plan

v30 and v31 both produced collapsed activation distributions (std 0.15 and 0.089 respectively, vs v29's 0.34). v31's hypothesis was that v30's continuous label strengths caused the collapse and that hard binarization at training time would restore v29's bimodal output. **It didn't. v31 collapsed worse.**

### Investigation findings (2026-04-24, post-v31 export)

Direct measurement of mel-diff signal-to-baseline ratio at each consensus label, ±3-frame tolerance window, 20-track sample:

| Source | per-strength mel-diff signal |
|---|---|
| consensus 1/5 systems | **0.43×** (sub-random) |
| consensus 2/5 systems | **0.46×** (sub-random) |
| consensus 3/5 systems | 1.65× |
| consensus 4/5 systems | 1.11× (anomaly) |
| consensus 5/5 systems | 1.54× |
| kick_weighted (v29 source) | 1.45× |
| reference: top-20% mel-diff | 14.4× |

**Root cause:** the v30/v31 label set was the union of 5 detectors at any agreement level. 76% of labeled positive frames are 1- or 2-system detections whose mel-diff is *below* random — they mark frames where mel-energy change is *less* than at random points in the audio. The model regresses to the mean across this label noise, producing the collapsed activation distribution.

**Why v29 worked:** kick_weighted labels mark percussive events with unambiguous low-frequency energy spikes. Even with documented stem-bleed contamination, the labeled frames coincide with mel-energy events. Signal ratio 1.45× is workable; v29 reached val_F1=0.54 frame-level.

### v32 plan — `min_systems: 3` filter at prep time

Filter consensus labels to ≥3-system agreement at prep time. Rationale:

- **Per-strength signal at ≥3-systems = 1.65×**, beats v29's kick_weighted (1.45×).
- **Cumulative ≥3-systems = 1.52× signal** with ~16.5% positive frame ratio after 3-frame plateau — sparse enough to be sharp, dense enough for the model to see many examples.
- **Density 207 events/min** at min_systems=3 vs 282 at min_systems=1 — broader coverage than kick_weighted (~100 events/min) without the contamination of single-detector noise.
- **Broader event coverage than kick_weighted** — picks up any percussive + strongly-confirmed harmonic onset, not just kicks/snares from a contaminated stem.

The filter is implemented as `cfg.labels.min_systems` in `prepare_dataset.py`. Default 1 preserves backwards-compat. v32 config sets 3.

v32 also opts into T4.4's `early_stopping_metric: val_peak_f1` — v31 confirmed val_loss-best is not on-device-realistic-best.

### Disproven directly by this round

- **"Continuous label strengths cause collapse" (v31 hypothesis)** — wrong. Sharp binarization didn't help and made things worse, because the underlying labels were polluted with sub-perceptual events at any encoding shape.
- **"Single-system detector noise averages out across 5 detectors" (v30 implicit assumption)** — wrong. The merge algorithm at 70ms tolerance creates a union, not a robust intersection. Without a `min_systems ≥ 3` floor, every detector's idiosyncratic noise gets a vote.

### Anomaly to investigate post-v32

The 4-system bucket has *lower* per-strength signal (1.11×) than the 3-system bucket (1.65×). Two plausible causes — (a) the merge algorithm chains harmonic detections across the 70ms window when a 4th detector hits, and (b) certain detector subsets co-fire on harmonic events that mel features can't see. Worth investigating but not blocking v32.

#### Step 2.5 (in flight): v32 on `onsets_consensus` with `min_systems: 3`

`configs/conv1d_w16_onset_v32_mel_only.yaml`. Requires `prepare_dataset.py` rerun to `processed_v32` (the filter applies at prep, can't reuse processed_v31). Expected outcome: training-time signal returns to v29-quality (val_F1 ≈ 0.50-0.55, post-export activation std > 0.30) with broader event coverage than kick_weighted.

### Measurement instrumentation priorities

1. **PLP accuracy measurement** via `persist_raw` — capture `plpPhase_`, `plpPulseValue_`, `plpConfidence_` every frame; correlate with ground-truth beat times. This is the prerequisite diagnostic for Step 3. If PLP has low accuracy vs ground truth, no treatment (gate OR training input) will salvage it.
2. **Per-corpus per-device tiering** — report edm/ as primary F1, edm_holdout as secondary "does it degrade gracefully on hard content," never aggregate across corpora.
3. **Gate-b re-run on v30** — re-execute `run_gate_b.py` on v30 firings once validated. If clean labels change the TP/FP populations, some shape feature might now discriminate where none did on v27.

### Directions explicitly rejected

- **Wider input window / deeper Conv1D / dilated conv.** Disproven and/or fundamentally mismatched with the 10-30 ms onset-identification constraint. See §"Disproven direction: wider input window" above.
- **Hard PLP AND-gate (any form).** Disproven 2026-04-23 via full threshold sweep. See §"Step 1" above.
- **More shape features as NN inputs.** Gate-b on v27 showed no deterministic shape feature separates TP from FP at firing moments. Re-evaluate only after v30 with clean labels gives a fresh gate-b measurement.
- **Bigger model / more parameters.** Signal-discrimination bottleneck, not capacity bottleneck.

### Current tasks tracking this plan

- #69 PLP beat-grid AND-gate — done, disproven, infrastructure kept, default disabled
- #70 Train v30 on onsets_consensus labels — prep re-running with both exclude-dir (argparse bug fixed 2026-04-23), training next
- #71 Fallback: v31 madmom-only full-mix labels (blocked on 70)
- #72 Fallback: multi-channel instrument model (blocked on 71)
- *(new)* PLP-as-training-input — not yet a task; create once v30 result is in

## 2026-04-25 — v32 deployed, results, comprehensive synthesis

### v32 on-device validation (b146, edm_holdout, 25 tracks × 3 devices)

```
device          P     R     F1    F1_50  F1_70  F1_100 F1_150
2A798EF8E684    0.403 0.273 0.300 0.195  0.237  0.300  0.373
ABFBC41283E2    0.442 0.203 0.256 0.160  0.195  0.256  0.318
659C8DD3ADF8    0.421 0.191 0.238 0.135  0.168  0.238  0.312
OVERALL         0.422 0.223 0.265 0.164  0.200  0.265  0.335
```

**v32 vs v29 (current best deployed):** v32 has *higher precision* (0.422 vs 0.384 v29) — the `min_systems: 3` label fix did improve label specificity — but *much lower recall* (0.22 vs ~0.65). Model fires sparingly but correctly. Overall F1 0.265 still well below v29's 0.484. Per-device variance (0.19-0.27 R) matches the audit D4 finding.

**Activation distribution stayed compressed:** v32 INT8 export std=0.123 (FP32 [0.014, 0.998] → INT8 [0.184, 0.934]). Per the n=4 monotonic correlation audit established (std≥0.30 → F1≈0.48; 0.12-0.15 → 0.27-0.29; <0.10 → unusable), this matched the prediction. **The min_systems=3 filter narrowed one failure mode but exposed another:** the model now produces clean predictions when it does fire, but doesn't fire often enough.

### Comprehensive synthesis — what 22+ runs (v11-v32) actually tell us

After exhausting the obvious dimensions (loss function, label encoding, label sharpness, label source filter, bias init, micprofile aug, hybrid features, wider/deeper architectures, W16→W32 receptive field), the val_F1 ceiling is structural, not tunable inside the current recipe.

**Frame-level val F1 envelope, by recipe family:**

| Family | val F1 | runs |
|---|---|---|
| `kick_weighted` + BCE + 30-mel | **0.55 ± 0.01** | v20 0.561, v21 0.552, v22 0.560, v29 0.543 |
| `kick_weighted` + focal/aug | 0.47-0.50 | v23, v24, v25 |
| Wider/deeper/W32 (any labels) | 0.47-0.48 | exp-w32, exp-wide-* |
| Focal-loss (any labels) | 0.37-0.42 | v26, v27-hybrid, v27-hybrid-real, v28 |
| `onset_consensus` labels | **0.31-0.34** | v30 0.311, v31 0.336, v32 0.336 |

On-device F1 has been pinned at 0.47-0.49 since v16 across radically different feature/loss configurations. The single biggest on-device F1 jump in project history (0.47→0.62) came from a *firmware* signal-path change (b117 NN-primary pulse), not a model change.

**Activation std vs on-device F1, n=4:**

| Run | INT8 act std | On-device F1 |
|---|---|---|
| v29 | 0.34 | 0.484 |
| v30 | 0.145 | 0.29 |
| v31 | 0.089 | not deployed |
| v32 | 0.123 | 0.265 |

Monotonic. T2.5 catches collapse offline before any device cycle is wasted.

### What every published 0.85+ onset detector has that we don't

External comparison (Schlüter & Böck 2014 ICASSP, Böck DAFx 2012, Vogl ISMIR 2017, Foscarin Beat This! 2024):

| | Schlüter '14 | Böck RNN '12 | Beat This! '24 | **Ours (v32)** |
|---|---|---|---|---|
| Mel bands | **80** | 20+40+80 stacked | 128 | **30** |
| Frequency range | 27 Hz–16 kHz | 30 Hz–17 kHz | 30 Hz–10 kHz | **40 Hz–4 kHz** |
| Frame rate | 100 Hz | 100 Hz | 50 Hz | 62.5 Hz |
| Receptive field | 150 ms | full-BLSTM | 30 s | 256 ms |
| Labels | hand-annotated | hand-annotated | multi-annotator | **5-system algorithmic union** |
| Reported F1 (50ms) | 0.89 | 0.88 | 0.89 (beat) | 0.27-0.48 |
| Params | ~few×10K | ~30K | 20M (also 2M) | 10K |

**Schlüter '14 reached 0.89 with a FF-CNN of similar parameter count to ours.** So architecture capacity is not the binding constraint at 10K params. The dominant deltas vs published systems are:

1. **fmax 4 kHz vs 10+ kHz.** We drop hi-hat / snare / cymbal energy entirely. These are exactly the signals that disambiguate percussive events in dense full-mix EDM.
2. **30 mel bands vs 80+.** We under-resolve narrow-band percussive transients.
3. **5-system algorithmic consensus vs hand labels.** Peer-detector agreement caps consensus-label training around F1 0.5-0.6 regardless of model capacity.

### Things every run shared (= unexamined dimensions)

- Conv1D, kernel [5,5], W16, ~256ms receptive field
- 30 mel bands, 40-4000 Hz, 62.5 Hz frame rate
- Single binary onset target (no multi-task)
- BCE-family loss (no shift-tolerant since v15)
- Single-channel input (no multi-resolution)
- INT8 post-training quantization
- Algorithmic labels (consensus or kick_weighted, never hand-annotated subset)

### Things explicitly disproven (don't retry)

- **Hybrid spectral features as NN inputs** — gate-b on b137 held-out: all 6 features TP-vs-FP |d| ≤ 0.10 pooled, well below 0.30 threshold. Path B dead.
- **Continuous label strengths cause collapse** (v31 hypothesis) — wrong, hard binarization made things worse.
- **Single-detector noise averages out across 5 detectors** (v30 implicit) — wrong, 1- and 2-system events are sub-random vs mel-diff.
- **Wider+deeper architecture moves the ceiling** — exp-wide-* set, no gain.
- **W16 → W32 receptive field widening** — exp-w32, no gain.
- **Stacking another loss variant on the same input representation** — nine variants tried, ceiling unchanged.

### Next experiments, ranked by evidence

Each has a falsifiable prediction. Run sequentially; abandon at first negative result that contradicts the prediction.

#### Exp 3 (DONE 2026-04-25): measured the algorithmic detector ceiling — **inverts the synthesis**

Ran madmom CNN + RNN against the same `.beats.json` ground truth, same tolerances as device validation:

```
detector   F1@50ms F1@70ms F1@100ms F1@150ms P@100ms R@100ms
madmom CNN 0.460   0.475   0.483    0.494    0.337   0.918
madmom RNN 0.479   0.498   0.509    0.525    0.373   0.873
                          ───────                  ───────
v29 (deployed)             0.484                          ← matches CNN
v32 (deployed)             0.265
```

**The ceiling on edm_holdout is F1 ≈ 0.5, set by labels-vs-target mismatch, not by model quality.** madmom's recall is 0.87-0.92 (finds nearly all `.beats.json` events) but precision is 0.34-0.37 (also fires on lots of unlabeled hi-hats / chord changes / vocal onsets). The `.beats.json` ground truth marks rhythm-trigger beats (~150-400/track); madmom fires on every onset (~3× more). This profile (high recall, low precision against rhythm-beats criterion) sets the F1≈0.5 wall.

**Implications, in order of how much they invalidate the prior plan:**

1. **The published 0.85+ F1 numbers are not comparable to ours.** Schlüter '14 reports 0.89 on `onset_db` (hand-annotated, diverse-genre, all onsets). Our validation criterion is a curated rhythm-trigger subset. They evaluate different products. Citing the 0.85 ceiling as a target for *our* product was a category error.
2. **v29 has been at the data ceiling all along.** All 22+ runs trying to break past F1=0.48 were chasing an unreachable number on this validation set. The structural-ceiling synthesis (mel resolution, fmax) was correct only as an explanation of *general* onset-detection ceilings — but our problem isn't general onset detection.
3. **v32 is genuinely below the ceiling** at F1=0.265 — but that's the *output-collapse* problem (std=0.12 → peak-picker rarely fires → recall=0.22), not a label-quality or representation problem. v29 reached the ceiling at std=0.34 with the same architecture, same mel resolution.
4. **Exp 1 (mel resolution + fmax expansion) is dropped** as a next move on this validation set. It might still help if we change the validation criterion to general onsets, but that's a product-spec change, not a model fix.
5. **Exp 2 (single-source madmom labels) becomes uninteresting** at the same F1 target — madmom itself only hits 0.51 here.

**The new ranked plan**:

- **Exp 4 (DONE 2026-04-25 PM): rolled devices back to v29 — uncovered firmware regression.** v29 b147 baseline with default settings: F1=0.257 (R=0.22), matching v30/v32 deployments. v29 b147 with `set beatgridmin 0`: **F1=0.470 (R=0.64)**, matching historical pre-b142 v29 within run-to-run noise. The PLP beat-grid AND-gate added in b142 (default `beatGridPatternMin=0.4`) suppresses recall by ~65% on rhythmic content. Every deployment since b142 has been gate-bottlenecked, not model-bottlenecked. This invalidates yesterday's "v32 is below the data ceiling because of activation collapse" conclusion: the v32 collapse is real offline (export std=0.123), but the on-device F1=0.265 was further suppressed by ~0.20 of firmware throttle.

### PLP-accuracy diagnosis — gate is principle-broken (2026-04-25 PM)

Followed up the gate finding with a `persist_raw=true` validation on 5 edm_holdout tracks (single device, b148+v32). Per-track aggregates:

| Track | F1 (no gate) | gtOnsetsMatched/Total | atTransientNorm | nnAgreement | gtPatternCorr |
|---|---|---|---|---|---|
| 1234669 | 0.55 | 34/97 (35%) | 0.75 | 0.31 | 0.86 |
| 2734862 | 0.65 | 20/107 (19%) | 0.58 | 0.35 | 0.94 |
| 3642438 | 0.71 | 24/154 (16%) | 0.81 | 0.51 | 0.99 |
| 4604737 | 0.61 | 0/95 (0%) | 0.0 | 0.34 | 0.0 |
| 4611640 | 0.44 | 27/77 (35%) | 1.01 | 0.02 | 0.98 |

**Key observations:**

- **`gtOnsetsMatched/Total` is 0–35%.** The gate at threshold 0.4 would only let through 0–35% of true onsets, exactly matching the recall collapse (R=0.22 with-gate vs 0.65 without).
- **`atTransientNorm < 1.0` on 4/5 tracks.** PLP value at NN firing times is *below* the track-mean PLP — the signal isn't "high at onsets, low between," it's roughly orthogonal to onsets.
- **`gtPatternCorr` is 0.86–0.99 on 4/5 tracks.** PLP *does* find the rhythm-period structure, but per-frame alignment is too noisy to predict individual onset times. Pattern-correct, frame-wrong.
- **`nnAgreement` is 2–51%.** NN and PLP disagree on which frames are onsets. The AND-gate suppresses on disagreement.

**Conclusion.** PLP is a reliable PERIOD tracker but a poor PER-FRAME gate. The b142 design assumed the latter and it fails empirically.

**Decision (#92 done):** flip `beatGridPatternMin` default 0.4 → 0.0 (b149). Runtime tunable preserved so the gate can be re-enabled per device or for sparse-content profiles where false positives are more expensive than missed firings.
- **Exp 5 (investigation, not training): why does v29 produce std=0.34 while v32 produces 0.12 at the same architecture / mel resolution?** v29 used kick_weighted (drum-only events, ~76/min density). v32 used consensus min_systems≥3 (any-onset, ~207/min density). The label *density* and *event-type breadth* may explain the activation collapse — sparse single-purpose events produce sharp predictions; dense any-onset events smear them. If true, the next training direction is *narrower-target labels* (drum-only, kicks-only, beat-grid-only), not broader.
- **Exp 6 (data work): generate clean drum-only labels without demucs stem bleed.** v29's kick_weighted labels were contaminated (32% of tracks had >0.7× bleed ratio per `HYBRID_FEATURE_ANALYSIS_PLAN.md`). A clean drum-only target — perhaps from a better separation model, or filtered by a kick-frequency-band energy criterion — could let v29's recipe push past 0.484.
- **Exp 7 (product-spec change, not training): redefine the validation criterion** to "any salient onset" rather than rhythm-beats. Reannotate `.beats.json` to include hi-hats and harmonic onsets, then madmom RNN immediately becomes the product baseline at its 0.87 recall. This is the "join the published F1 ladder" path — but it's a product decision.

**What is still disproven** (carries over from earlier synthesis): hybrid spectral features (gate-b dead), continuous label strengths cause collapse (v31 wrong), wider/deeper architecture, W16→W32 widening, additional loss variants, min_systems threshold tightening. None of those move the on-device F1 on this validation set.

**What to NOT conclude yet:** the input-representation hypothesis (mel resolution / fmax) isn't disproven — it's just not the binding constraint *for this validation criterion*. If the product spec moves toward general onset detection (Exp 7), that hypothesis becomes testable again.

#### Exp 1 (highest-ROI training experiment): mel resolution to Schlüter '14 spec

Single-axis change vs v29: `n_mels: 30 → 80`, `fmax: 4000 → 10000`, keep window/labels/loss identical.

- **Falsifiable prediction:** val_F1 frame > 0.65 within 30 epochs. If still ≤ 0.55, mel resolution alone wasn't the constraint; pivot to Exp 2.
- **Cost:** ~2 days (re-prep, retrain, device feasibility check).
- **Risk:** nRF52840 RAM/flash budget. 80 mel × 16 frames context = 1.28KB INT8. Verify fits before launching prep.

#### Exp 2 (only if Exp 1 plateaus): single-source madmom labels

Replace `onsets_consensus` with raw madmom CNNOnsetProcessor outputs.

- **Falsifiable prediction:** val_F1 frame ≥ 0.50. If still ≤ 0.40, the consensus-union wasn't the binding constraint and labels aren't the lever.
- **Cost:** ~1 day (regenerate labels, retrain).

## 2026-04-27 — v33 b151 mechanics, evidence from existing diagnostics

v33 (50 mel, 30-8000 Hz) deployed at b151. Offline `val_peak_F1=0.690` (1.8× v32). On-device edm_holdout F1=0.449 — below v29's 0.484 ceiling. Before guessing at fixes, the existing `validate.json` was re-analyzed for what the diagnostics actually say. **No new validation runs required for these findings; all derived from `signals.gaps`, `diagnostics.onsetOffsetMs`, and per-track `onsetTracking` already in the b151 result.**

### Per-track summary (25 tracks × 3 devices, mean per track)

| | refOnsets | rate | F1 | P | R | IQR (ms) | medOff (ms) |
|---|---|---|---|---|---|---|---|
| Bottom 5 (mean) | 51 | 2.99 | 0.28 | 0.23 | 0.47 | 380 | — |
| Top 5 (mean) | 116 | 4.19 | 0.61 | 0.55 | 0.71 | 160 | — |
| All 25 mean | 84 | 3.43 | 0.45 | 0.39 | 0.57 | 244 | -3 |

### Findings, with the correlation that backs each one

**1. Model fires at content-independent rate.** `corr(refOnsets, rate) = +0.07`. Detection rate is ~3.4/sec ± 1.0 regardless of GT density (which spans 33→155 events/35s). The rate variance (CoV=0.29) is uncorrelated with content density. This is the bottleneck mechanism: dense-onset tracks happen to align FPs onto real onsets; sparse-onset tracks accumulate FPs as background noise.

**2. Bottom tracks have *better* peak-mode feature discrimination than top tracks.** Mean cohensD on the bad-F1 bucket vs good-F1 bucket:

| feature/peak | top-8 cohensD | bot-8 cohensD |
|---|---|---|
| centroid | 0.55 | **0.74** |
| flatness | 0.63 | **0.78** |
| rolloff | 0.40 | **0.66** |
| hfc | 0.51 | 0.55 |
| crest | **1.13** | 0.68 |
| raw_flux | **0.72** | 0.63 |

When the model fires on bad tracks, the input features look *more* onset-shaped (high centroid/flatness/rolloff peaks) than when it fires on good tracks. **Rules out "input features can't separate onset from non-onset" as the bottleneck** — the discrimination is there, it's just being applied to the wrong frames. Crest is the lone exception (better on good tracks); v29's reliance on crest may be one reason it hit ceiling without the others.

**3. Frame-level cohensD is universally weak (~0.0 ± 0.15 across all 6 features).** Only peak-mode discrimination (when the model fires) is strong. Frames where the model *does not* fire look indistinguishable from GT-onset frames at the feature level. The model's temporal layers do most of the work; the per-frame features are ambient noise punctuated by sparse peaks.

**4. IQR is a sparse-GT measurement artifact, not a timing problem.** `corr(refOnsets, IQR) = -0.905`. Median offset is -3 ms across all tracks — **timing calibration is correct**. mir_eval's onset matching pairs each detection with its nearest GT; on sparse-GT tracks, FPs land 200+ ms from any GT and inflate IQR mechanically. The "stdDev=129ms, iqr=193ms" stat in `diagnostics.onsetOffsetMs` is dominated by this matching artifact, not by genuine timing scatter.

**5. F1 is dominated by content density.** `corr(refOnsets, F1) = +0.89`. Tracks with more GT events score higher F1 because the constant-rate model firing has more chances to align with real onsets. Single best predictor of track F1 is the GT count, beating any feature-discrimination metric.

### Diagnosis

The v33 bottleneck is **precision on sparse content driven by content-independent firing rate**. The model produces a detection roughly every 290 ms regardless of input. On dense EDM (refOnsets ≥ 100, ~3 events/sec) this is approximately right; on sparser content (refOnsets < 60, ~1.5 events/sec) the surplus firings become FPs (P drops from 0.55 → 0.23, recall holds 0.5+).

This is not a representation problem (peak-mode features discriminate fine), not a timing problem (median offset 0), not a label problem (offline val_peak_F1=0.690 is real on the same labels). It is a **threshold/decision problem**: `pulseOnsetFloor=0.30` produces a near-constant cross-rate because the activation distribution is centered too close to the threshold.

### What we still need persist_raw=true to verify

The above diagnoses the symptom. To confirm the mechanism we need on-device activation telemetry, which the b151 run did not capture (`persist_raw=false`). Specifically:

- **Activation mean/std/quantiles per track.** Hypothesis: distribution is centered near 0.30 with std ~0.08, so the threshold sits AT the noise floor. If true, the activation_stats `pAboveThreshold` will be ~50% in quiet sections.
- **Per-frame activation at GT vs non-GT timestamps.** Hypothesis: signal-vs-baseline ratio on-device is much weaker than offline. If signal_frames at GT-onset frames have mean 0.5 vs baseline 0.3, threshold-based gating will fail; if 0.7 vs 0.2, gating works and the issue is elsewhere.
- **Comparison of best (3642438, F1=0.70) vs worst (210560, F1=0.23) tracks.** Same model, dramatically different precision — what's different about the activation distribution?

Tracked as #97 (re-validate 4 representative tracks with `persist_raw: true`).

### Implications for next experiment

If finding (1) is the binding constraint, three actionable directions, ranked by how falsifiable they are:

1. **Threshold sweep on b151 firmware** (no retrain). Sweep `pulseonsetfloor` 0.25 → 0.55 with the existing b151 deployment. *Falsifiable prediction:* if there exists a threshold where dense-track F1 holds and sparse-track P rises to >0.4, the bottleneck is purely the threshold and a single-axis fix recovers ~0.05-0.08 F1. If no such threshold exists, the activation distribution itself is the problem (model output not separating onset from baseline) and we need recipe changes.
2. **Re-validate v29 b151 with the same mel constants** (50/30-8000 Hz) to factor out the firmware change. v29 was last validated under 30-mel mel constants. If v29 also drops on 50-mel mels, the mel-resolution change in firmware is the real regressor.
3. **Train v34 with focal loss + harder negatives** to break the constant-rate firing. Single-axis vs v33: same labels, same n_mels, but explicit `gamma=2.0` focal + 2× negative weighting. *Falsifiable prediction:* activation std rises from v33's offline 0.18-0.20 to >0.30, and the rate-vs-content-density correlation rises from 0.07 to >0.4.

### Threshold sweep result (2026-04-27 — hypothesis (1) FALSIFIED)

Sweep job 89d6b4295cde: `pulseonsetfloor ∈ {0.20, 0.30, 0.40, 0.50}` × 8 tracks (4 dense + 4 sparse) × 3 devices on b151.

```
thr   bucket    F1     P      R     rate/s
0.20  DENSE    0.553  0.511  0.615  3.92
0.20  SPARSE   0.330  0.253  0.595  3.47
0.30  DENSE    0.561  0.585  0.544  3.00
0.30  SPARSE   0.234  0.245  0.336  1.88
0.40  DENSE    0.492  0.531  0.470  2.88
0.40  SPARSE   0.238  0.260  0.320  1.98
0.50  DENSE    0.496  0.606  0.438  2.35
0.50  SPARSE   0.226  0.241  0.274  1.60
```

**Hypothesis falsified.** No threshold satisfies "hold dense F1 while raising sparse P >0.4". Sparse-track precision is **independent of threshold** (P=0.24-0.26 for all 4 thresholds while R drops from 0.60 → 0.27). Raising the threshold removes TPs and FPs at the same rate — i.e., the model's activation distribution at FP frames overlaps heavily with TP frames, with no "onset" peak rising clear of the noise floor.

**Per-track sanity check confirms.** All 4 sparse tracks show P essentially flat across thresholds (210560: 0.15→0.20, 4017611: 0.15→0.13, 5335389: 0.29→0.20, 4611640: 0.42→0.44). The activation magnitude is not selectively higher at real onsets.

**Decision:** the bottleneck is **activation distribution shape**, not threshold. Move to recipe changes (Exp 3) and capture activation telemetry (#97 persist_raw) to characterize the distribution before training v34.

**Free stopgap.** thr=0.20 lifts BOTH-bucket F1 from 0.398 (default) to 0.442 (+0.044, +11%). Drop firmware default to 0.20 alongside the next deploy. Does not fix the real problem but recovers ~0.04 F1 at zero cost. Landed at b152 — `pulseOnsetFloor` default 0.30 → 0.20 in `AudioTracker.h`, `SerialConsole.cpp` (defaults reset), `ConfigStorage.cpp` (setDefaults).

### persist_raw activation distribution (2026-04-27 — INVERTS the v33 plan)

Job 7a60e695f827: `persist_raw=true` validation on 4 tracks (3642438, 3298908, 210560, 4017611) × 3 devices, ~1140 frames each. **The activation is centered HIGH, not low — and it does NOT separate GT-onset frames from non-GT frames on-device.**

**On-device activation distribution (mean across 3 devices per track):**

| track | F1 (b151) | mean | std | p50 | p90 | p99 | %>0.20 | %>0.30 |
|---|---|---|---|---|---|---|---|---|
| 210560 (sparse, F1=0.23) | 0.23 | 0.420 | 0.148 | 0.413 | 0.620 | 0.808 | 94% | 80% |
| 3298908 (dense, F1=0.62) | 0.62 | 0.436 | 0.229 | 0.409 | 0.780 | 0.957 | 83% | 68% |
| 3642438 (dense, F1=0.70) | 0.70 | 0.413 | 0.156 | 0.398 | 0.627 | 0.842 | 93% | 78% |
| 4017611 (sparse, F1=0.26) | 0.26 | 0.424 | 0.164 | 0.407 | 0.640 | 0.855 | 93% | 78% |

**Activation median is ~0.40 on every track.** The model's sigmoid output is centered near the upper-middle of [0,1], not collapsed near 0. The activation crosses 0.20 on 81-96% of frames and crosses 0.30 on 65-81% of frames. This is **output saturation**, not collapse — and explains exactly why threshold sweeps don't move precision: the threshold is slicing through the bulk of a near-constant distribution.

**Signal-vs-baseline at GT frames (±50ms tolerance):**

| track | nGT | gtMean | bgMean | lift | cohens D |
|---|---|---|---|---|---|
| 3642438 (dense, F1=0.70) | 477 | 0.398 | 0.423 | **−0.027** | **−0.179** |
| 3298908 (dense, F1=0.62) | 326 | 0.485 | 0.417 | +0.068 | +0.300 |
| 210560 (sparse, F1=0.23) | 96 | 0.431 | 0.419 | +0.011 | +0.079 |
| 4017611 (sparse, F1=0.26) | 117 | 0.404 | 0.426 | **−0.022** | **−0.133** |

**On 2 of 4 tracks the activation is LOWER at GT-onset frames than at random non-GT frames** (3642438 d=−0.18, 4017611 d=−0.13). On the dense F1=0.70 track — our *best* track — GT frames have *measurably lower* mean activation than non-GT frames. Only 3298908 shows meaningful positive lift (d=+0.30); 210560 is essentially 0.

**The model's NN output is not signaling onset events on-device.** Activation is high everywhere; it neither rises at real onsets nor falls between them. The peak-picker is finding local maxima in near-constant high-activation noise — which explains the content-independent firing rate (corr=0.07 from earlier finding (1)) and why threshold sweeps don't separate TPs from FPs (sweep finding).

**Why the offline-vs-on-device gap (val_peak_F1=0.690 → 0.449)? — ROOT CAUSE FOUND 2026-04-27**

**The firmware mel filterbank table was stale at v33.** `MEL_BANDS[NUM_MEL_BANDS]` in `SharedSpectralAnalysis.cpp` was a 30-entry array literal from the v32 layout. When `NUM_MEL_BANDS` was bumped 30 → 50 for v33, the array literal was not regenerated. C++ aggregate initialization silently zero-filled bands 30-49 with `{0, 0, 0}`, all reading FFT bin 0 (DC). The neural network received identical garbage values (≈0.9 across bands 30-49) for 40% of its input on every frame.

Mel parity test on a v33 holdout track (after fix): MAE between firmware and training mel = 0.004. Before fix: MAE = 0.113 (28× worse), with bands 42-49 producing exactly 0.9066 every frame.

This explains every symptom in one mechanism:
- Activation centered at 0.40 (not at 0): garbage-but-bounded mel values produce stable mid-range activations
- No GT-vs-baseline separation: 40% of input is constant noise, drowning out the discriminative 60% the model learned
- Threshold sweep doesn't help: no threshold can recover info that was lost at the input
- Content-independent firing rate: the constant 40% of inputs make the activation roughly constant regardless of audio
- 30-mel models worked perfectly (v29 F1=0.484 with no firmware fix needed): MEL_BANDS[] matched the v32 30-mel layout, so no zero-fill triggered

The previous "input gain shift / +13dB" hypothesis was wrong — it was the right *symptom* (input distribution mismatch) but the wrong *cause*. The microphone-vs-studio gain difference is real but small; the dominant signal was that 20 of 50 mel bands were constant DC.

**Fix:** regenerated 50-mel × 30-8000 Hz `MEL_BANDS[]` table in `SharedSpectralAnalysis.cpp` to match `librosa.filters.mel(sr=16000, n_fft=256, n_mels=50, fmin=30, fmax=8000, htk=True)`. Added a `static_assert(NUM_MEL_BANDS == 50 && MEL_MIN_FREQ == 30 && MEL_MAX_FREQ == 8000)` in the cpp so the next time the constants drift, compile fails instead of silently zero-filling. Built b153 with the fix.

**Process gap (now closed):** the `static_assert` in `SharedSpectralAnalysis.h` validated the *constants triplet* but not whether the *table* matched. The bug was invisible to it. Plus several layered silent fallbacks (`weightSum > 0 ? sum/weightSum : 0.0f`) hid the corruption from runtime telemetry. Audit completed 2026-04-27 (#102) found ~10 silent fallbacks across the firmware audio path; CRITICAL/HIGH ones now fail loudly via `BLINKY_ASSERT` or ERROR-level logs. Rule added to `CLAUDE.md` under "CRITICAL: No Silent Fallbacks".

This **invalidates the v34 plan entirely**:
- ❌ Focal loss + harder negatives — wrong direction (was already excluded)
- ❌ Input gain CMN (#101) — would have masked the symptom but not fixed the root cause; **superseded** since the table fix should restore offline-vs-on-device parity
- ❌ Wider gain augmentation in training — same; not the binding constraint

**Real next step: re-validate v33 b153 (firmware fix only, no retrain).** v33 was trained on properly computed 50-mel × 30-8000 Hz mel; the model itself is fine. With the firmware mel filterbank now matching training, the offline val_peak_F1=0.690 should translate much closer on-device. Predictions:
- Activation distribution centered well below 0.4 with healthy std (ideally 0.10-0.20 mean, 0.20-0.30 std)
- GT-vs-baseline cohens d positive on all tracks (ideally d > 0.5)
- On-device F1 close to madmom CNN's 0.46 ceiling on this corpus, possibly higher

**Falsifiable failure modes for the fix:**
- If on-device F1 ≤ 0.50 after b153 deploy: there's *another* firmware-vs-training mismatch we haven't found (window function? noise subtraction? log scaling?) — extend mel parity test to FFT magnitudes, then re-audit.
- If activation distribution still saturates at 0.4: the model itself learned the saturated representation as a side effect of training data. Retrain v33 from clean mel with #97-equivalent persist_raw monitoring.

### v33 b153 on edm/ — first clean baseline on the primary corpus (2026-04-27)

Job 22c3a6abd57e: 18 tracks × 4 devices, persist_raw=true. **This is the only valid v33 metric** per the new corpus rule.

**Headline:**

```
F1=0.570  P=0.564  R=0.612
F1@50ms=0.370  F1@100ms=0.570  F1@150ms=0.636
```

**Per-track ranking:**

| F1 | track | P | R | refOnsets |
|---|---|---|---|---|
| **0.720** | garage-uk-2step | 0.83 | 0.64 | 146 |
| **0.674** | dnb-liquid-jungle | 0.62 | 0.75 | 119 |
| **0.660** | reggaeton-fuego-lento | 0.64 | 0.68 | 147 |
| **0.656** | techno-deep-ambience | 0.71 | 0.61 | 147 |
| **0.643** | techno-minimal-emotion | 0.61 | 0.68 | 151 |
| **0.624** | breakbeat-drive | 0.64 | 0.61 | 138 |
| **0.610** | trance-party | 0.60 | 0.63 | 125 |
| 0.593 | amapiano-vibez | 0.73 | 0.50 | 161 |
| 0.593 | breakbeat-background | 0.67 | 0.54 | 142 |
| 0.570 | dnb-energetic-breakbeat | 0.58 | 0.58 | 105 |
| 0.570 | trance-goa-mantra | 0.57 | 0.58 | 100 |
| 0.561 | afrobeat-feelgood-groove | 0.50 | 0.64 | 124 |
| 0.516 | dubstep-edm-halftime | 0.54 | 0.51 | 114 |
| 0.495 | techno-minimal-01 | 0.42 | 0.62 | 89 |
| 0.487 | techno-dub-groove | 0.48 | 0.50 | 108 |
| 0.471 | edm-trap-electro | 0.44 | 0.52 | 80 |
| 0.449 | techno-machine-drum | 0.31 | 0.86 | 49 |
| 0.365 | trance-infected-vibes | 0.28 | 0.56 | 71 |

**Per-device (small spread):** 062CBD12EB69 F1=0.596, 2A798EF8E684 F1=0.588, 659C8DD3ADF8 F1=0.554, ABFBC41283E2 F1=0.542.

**Activation distribution still saturated** (mean=0.453, p50=0.418, %>0.30 = 73.9%). Same shape as on the deleted edm_holdout corpus — confirms saturation is content-independent and is driven by input distribution mismatch, not by track difficulty.

**Signal-vs-baseline cohens d is mixed and frequently negative.** Strong positive lift on breakbeat-background (+0.88) and breakbeat-drive (+0.72); strong negative on afrobeat (−0.56), trance-goa-mantra (−0.58), amapiano (−0.43), techno-deep-ambience (−0.40). Counterintuitively, the F1=0.72 best track (garage-uk-2step) has d≈0. This says the saturated sigmoid still carries onset information in its **derivatives** (sharp local changes) even when its absolute level is squashed; the firmware peak-picker recovers most of that value.

### Tier-1 must-perform set (acceptance criterion: F1 ≥ 0.6 every track)

The 7 tracks at F1 ≥ 0.6 cover the system's bread-and-butter content across genres:

- garage-uk-2step (0.72) — clean 4-on-floor + ghost snares
- dnb-liquid-jungle (0.67) — high-BPM breakbeat
- reggaeton-fuego-lento (0.66) — dembow pattern
- techno-deep-ambience (0.66) — minimal techno, clear kick
- techno-minimal-emotion (0.64) — minimal techno
- breakbeat-drive (0.62) — high-energy breaks
- trance-party (0.61) — 4-on-floor with builds

These 7 are the *acceptance criterion* for any future model deploy. v34 must hold or improve every one of them.

### v34 plan — input domain adaptation (NOT a model architecture change)

Single-axis change vs v33. Architecture, labels, n_mels, fmin/fmax, loss, optimiser all stay identical. Only the *input augmentation pipeline* changes — because the diagnosis is sim-to-real, not capacity.

1. **Re-calibrate `target_rms_db` for 50-mel × 8-kHz.** The current value (−72 dBFS) was calibrated April 14 for 26-mel × 4-kHz fmax. Wider band integration at fmax=8000 raises mel mean at the same audio RMS. Procedure: capture device mel via `stream nn` mode while playing 3-4 representative tracks, compute mel-band mean over the capture, choose `target_rms_db` so training mel mean matches device mel mean (target ≈0.40 mean to match observed runtime).
2. **Restore gain augmentation `[-18, +18] dB`.** v29 used this; v33 dropped it. The model never saw the level shifts it sees in deployment.
3. **Add mic-profile sampling.** v29 had `mic_profile_flag` enabled; v33 didn't. Per-band gain shifts from the actual mic.
4. **Optional: RIR augmentation.** Existing infra (`base.yaml: noise_dir`). Adds room reverb. Lower priority — gain aug is the bigger lever.

**Falsifiable predictions for v34 on edm/:**

- Activation mean drops from 0.453 → ≤0.25
- Activation std rises from 0.234 → ≥0.30
- Cohens d turns uniformly positive across tracks (no <-0.3 outliers)
- Tier-1 tracks all reach F1 ≥ 0.7 (closing toward offline 0.690 ceiling)
- Mid/bottom tier gains +0.05 minimum

**Risk:** v34 could break the 7 tier-1 tracks while gaining the bottom 11. The acceptance bar is *no track regresses below F1 0.55* AND *every tier-1 track ≥ its current value*.

If v34 succeeds: that's the new deployed model. If v34 plateaus or regresses: input-side hypothesis is wrong, return to investigating the saturated activation at the source (training itself produces saturated outputs vs the data — a curriculum/loss issue).

### v34 result + correction (2026-04-28)

**v34 plateaued at val_peak_F1 = 0.627** (vs v33's 0.690), early-stopped at epoch 16. Best peak_F1 was epoch 1 (0.627), no improvement across all 16 epochs. Frame F1 still climbed (0.482 → 0.534) — pattern consistent with saturation: model learns to predict "high mel = onset-likely" on average (frame F1 up) but loses temporal contrast (peak F1 flat).

**Correction to the v34 plan above:** the claim "v33 dropped gain aug, v34 restored it" was WRONG. Both v33 and v34 ran with `--augment` (Makefile default), both applied the same hardcoded `[-18, -12, -6, +6, +12, +18]` dB gain augmentation. The only substantive config diff was `target_rms_db: -72 → -30`.

**Real cause:** interaction between `target_rms_db` and gain augmentation. At v34's `-30 + +18 dB = -12 dBFS RMS` effective, the loudest training variant lands above device runtime (~-15 dBFS RMS) and drives instantaneous peaks into clipping/saturation, saturating bass mel bands. v33's `-72 + +18 dB = -54 dBFS RMS` was well below saturation. Same gain aug, different RMS baseline — different result. (`target_rms_db` calibrates RMS, not peak; high-crest signals like drum transients can still clip near 0 dBFS even when RMS is well below it — that's the saturation mechanism.)

**Mel mean per config (measured):**
- v33 prepped data: ~0.37 (bimodal: clean+aug-quiet around 0.10, conditioned around 0.75)
- v34 prepped data: 0.808 (unimodal+saturated; +18 aug variant clips at 1.0)
- Device runtime: 0.754 (mel mean of firmware-computed mel during music playback — not directly comparable to training mel mean since the audio level at the mic is unknown)

v33's bimodal training distribution included device-like levels via the conditioned variant (compressor + whitening). It worked offline (val_peak_F1=0.690) and at degraded-but-functional on-device F1=0.570. v34 collapsed both modes into one centered too high, with the +18 dB aug pushing into saturation.

### v34b plan (2026-04-28)

Single-axis change vs v33: `target_rms_db: -72 → -48`. Calibrates the gain-aug *peak* RMS to device level instead of the *center*:
- target_rms_db + 18 = -30 dBFS RMS (target for the loudest aug variant) → target_rms_db = -48
- **Predicted prep mel mean** (linear interpolation from measured v33=0.37 at -72 and v34=0.808 at -30): **~0.62** at -48
- Per-variant prediction (rough; actual values are content-dependent):
  - Clean variant: -48 dBFS RMS audio, mel mean ~0.40 (well below saturation; spectral content faithful)
  - Gain aug +18 dB peak: -30 dBFS RMS audio, mel mean targets device-runtime distribution (measured 0.754). Whether this exact value is hit depends on the audio's spectral content — if training mel at -30 dBFS RMS lands somewhat lower, the conditioned variant's whitening still covers the device-loud regime.
  - Conditioned variant: ~-42 dBFS RMS pre-compressor; +6 dB makeup + per-band whitening pull mel toward 1.0 in active bands. Stays in the 0.7-0.85 range.

This sits between v33 (-72) and v34 (-30) and tests whether the calibration direction is right when saturation is avoided. If v34b also plateaus around 0.62, the calibration hypothesis is wrong and we revert to v33-as-deployed and approach from a different angle.

**Falsifiable predictions for v34b:**
- Offline val_peak_F1 ≥ 0.66 (between v33's 0.69 and v34's 0.63)
- On-device F1 on edm/ ≥ v33's 0.570 (acceptance bar)
- On-device NN activation mean ≤ 0.40 (vs v34's 0.45 — note: this is the model's *output* activation distribution measured during validation, not the training mel mean)

### v34b result + entire framing overturned (2026-04-28)

v34b plateaued at val_peak_F1=0.477 by epoch 4 — far below v34's 0.627, far below v33's 0.690. Every attempt to "fix" the sim-to-real gap by raising target_rms_db has made things worse. Stopped at epoch 7.

Stepped back and ran two parity tests with existing tooling:

**Test 1: offline peak-picker on the v33 b153 on-device activation stream** (`/tmp/v33_b153_edm.json` from #97). Train-time `_peak_pick_1d` (simple local-max + threshold + 50 ms cooldown) applied to the recorded device activations:

```
                          F1 (mean across 18 edm/ tracks × 4 devices)
firmware peak-picker      0.570  ← what we measured as on-device
offline _peak_pick_1d     0.298  @ thr=0.20
                          0.323  @ thr=0.30
                          0.370  @ thr=0.40
```

The firmware peak-picker (bass gate + PLP bias + tempo-adaptive cooldown) is **adding +0.20-0.27 F1** vs simple local-max picking on the same activation stream. The complex gating works.

**Test 2: offline `evaluate.py` on edm/ corpus directly** (mel computed offline by training pipeline, simple peak-picker applied). Aggregate F1 ≈ 0.45 across the 18 tracks. Firmware F1=0.570 on the same content. **Firmware beats offline by +0.12 F1** thanks to its peak-picker.

### What the val_peak_F1=0.690 number actually was

`val_peak_F1` during v33 training was measured on the **15% val_split of the *training* corpus** (`val_split: 0.15` in base.yaml). The edm/ corpus is excluded from training entirely. **These are different test sets.** The "0.690 offline → 0.570 on-device" delta is *not* a deployment loss — it's a content-difficulty gap between in-distribution training-val and held-out edm/, plus the simple-vs-complex picker delta going in our favor.

### v33 b153 is performing optimally — there is no "sim-to-real gap" to close

- Offline-on-edm/ with simple picker: **~0.45**
- On-device (firmware peak-picker): **0.570**
- Firmware adds +0.12 F1 on top of what the model output supports offline

The on-device system is **outperforming a straightforward offline evaluation on the same content.** v34/v34b were chasing a phantom — there was no input-distribution gap to close because the firmware was already extracting more value from the model than offline tools assume.

### Things now disproven (do not retry)

- Raising `target_rms_db` to "match device level" — 2 experiments, both regressed
- "Sim-to-real input distribution alignment" framing — based on mis-comparing different test sets
- The April 14 base.yaml comment claiming `target_rms_db=-72` calibrates to device mel mean=0.775 — re-measured 2026-04-27, training mel mean at -72 is actually 0.37; either the original measurement was on the conditioned variant, or the calibration drifted across the 26→50 mel pipeline change. v33 worked despite this miscalibration.
- Reading `val_peak_F1` reported during training as comparable to firmware F1 on held-out content — they're on different test sets.

### Real next experiments

Ranked by evidence basis after 2026-04-28 peak-picker parity tests:

1. **Firmware peak-picker tuning sweep** — bass gate, PLP confidence threshold, cooldown. Currently adds +0.12 F1 over simple offline picker; tuning could find a better operating point. Cheap (no retraining), uses existing `param-sweep` tooling. **Highest expected ROI.**
2. **Cleaner training labels** (#72-blocker) — `kick_weighted` from demucs has 32% bleed contamination per HYBRID_FEATURE_ANALYSIS_PLAN.md. Cleaner targets directly improve offline F1, which firmware lifts further. ~few days of label-side work.
3. **Recompute v33's actual offline-on-edm/ ceiling with the firmware-style peak-picker logic** (bass gate + PLP bias). If we can reproduce ~0.57 offline using the firmware-equivalent picker in `analysis/peak_picker.py` (#35), we have a fast offline iteration loop instead of the 30+ min per-experiment device-validation cycle.

### Tool fixes landed 2026-04-23

- `prepare_dataset.py --exclude-dir` → `action='append'`: previously silently kept only the last value; now unions stems from all passed dirs.
- `prepare_dataset.py MIN_FREE_GB`: raised 50 → 200 to match actual merge-step footprint (final arrays 100-130 GB each, 1.2× buffer for merge).
- `prepare_dataset.py --auto-clean-stale`: new flag auto-deletes stale `processed_v*` dirs and non-current mel cache entries in non-interactive mode. Previously interactive prompts silently skipped in tmux via `yes N`, letting 200+ GB of dead data persist and starving the merge step.

## Background (historical — pre-v26, retained for context)

Baseline model (W32 frame-level FC, 55K params, plain BCE loss) achieves:
- Beat F1: 0.49 mean / 0.54 median on 18 EDM test tracks
- Downbeat F1: 0.24 mean / 0.24 median
- **Critical issue**: 4-7x downbeat over-detection on 14/18 tracks

Root cause analysis found three problems: (1) training recipe used plain BCE
which punishes correct predictions offset by 1-2 frames, (2) downbeat
consensus came from only 2 of 4 systems (essentia and librosa provide NO
downbeats), giving only 40.8% inter-system agreement, and (3) the W32
context window (0.5s = ~1 beat) is too short to identify downbeat position
within a bar.

## Deployed Model: W32 FC (cal63, on all 3 devices)

FC(832→64→32→2), 55K params, 56.8 KB INT8, W32 (0.5s). Beat F1=0.491, DB F1=0.238.
Cal63 mel calibration (-63 dB RMS). Consensus v5 labels (7-system).

## ~~Active Priority: Dual-Model Architecture~~ (superseded — 2026-04-22)

The dual-model plan below (W8 OnsetNN + W192 RhythmNN, March 15 2026) was not taken forward. Current production architecture is a single Conv1D W16 model (30 mel bands, 32-32 channels, k=5, 16-frame window ≈ 256 ms). See §"2026-04-22 — focal-loss regression + v29 reset" above for current state.

**Status: TRAINING (March 15, 2026) — HISTORICAL**

**Problem:** Single models can't serve both onset detection (needs short window, high precision,
every frame) and downbeat detection (needs full-bar context, 3+ seconds). W192 FC was attempted
but regressed severely (Beat F1=0.370, DB F1=0.145) — FC flattening of 4992 inputs destroys
temporal locality.

**Solution:** Two specialized Conv1D models:

**OnsetNN** — kick/snare detection for visual triggers
- Conv1D(26→24,k=3) × 2 → Conv1D(24→1,k=1,sigmoid)
- W8 (128ms), ~3.6K params, ~4 KB INT8, <1ms inference, every frame (62.5 Hz)
- Config: `configs/onset_conv1d.yaml`
- Feeds OSS buffer as primary ODF and directly drives AudioControl.pulse

**RhythmNN** — downbeat and bar structure detection
- Conv1D(26→32,k=5) → AvgPool(4) → Conv1D(32→48,k=5) → AvgPool(4) → Conv1D(48→32,k=3) → Conv1D(32→2,k=1)
- W192 (3.07s = 1.5+ bars at all tempos), ~16.6K params, ~16 KB INT8, <8ms inference, every 4th frame (15.6 Hz)
- Config: `configs/rhythm_conv1d_pool.yaml`
- Conv1D preserves temporal locality; AvgPool1d progressively compresses time (192→48→12)
- Drives CBSS beat/downbeat tracking, AudioControl.downbeat, beatInMeasure

### Resource Budget (Dual Model)

| Resource | OnsetNN | RhythmNN | Combined | Budget | Headroom |
|----------|---------|----------|----------|--------|----------|
| Flash (model) | ~4 KB | ~16 KB | ~20 KB | ~500 KB | 96% |
| RAM (arena) | ~2 KB | ~8 KB | ~10 KB | 16 KB | 38% |
| RAM (window) | ~0.8 KB | ~19.5 KB | ~20 KB | 236 KB | 92% |
| Inference | <1ms@62.5Hz | <8ms@15.6Hz | — | 10ms/frame | OK |

### Training (in progress)

Both models training in tmux on consensus_v5 + cal63 data (chunk_frames=384):
```bash
tmux attach -t onset    # Conv1D W8, beat-only
tmux attach -t rhythm   # Conv1D+Pool W192, beat+downbeat
```

### Closed: W192 FC (March 15, 2026)

FC(4992→64→32→2), 322K params, 314 KB INT8. Beat F1=0.370, DB F1=0.145.
First FC layer has 319K/322K params — must learn all temporal correlations through
raw weight matrices. Superseded by Conv1D+Pool dual-model architecture.

## Labeling Pipeline

### Systems (7 total, 5 with downbeats)

| System | Beats | Downbeats | Architecture | Status (March 14) |
|--------|-------|-----------|-------------|-------------------|
| Beat This! | yes | yes | TCN+Transformer | Done (6993) |
| madmom | yes | yes | RNN+DBN | Done (6993) |
| essentia | yes | no | signal processing | Done (6993) |
| librosa | yes | no | onset energy | Done (6993) |
| demucs_beats | yes | yes | Demucs drums → Beat This! | Done (6993) |
| beatnet | yes | yes | CRNN + particle filter | Done (6993) |
| allin1 | yes | yes | NN + structure analysis | 935/6993 (13%). Blocked on optimization — see below. |

**allin1 fix (March 14):** DBN threshold (0.24) was clipping away all activations
on 30s clips after 3-way normalization compressed peaks below threshold.
Monkey-patched in `_allin1_helper.py` to set `threshold=None`. Also fixed
stdout contamination from Demucs via fd-level redirect.

**merge_consensus_labels_v2.py:** Updated with weights for new systems:
demucs_beats=0.85, beatnet=0.8, allin1=0.7.

**Test track annotations:** 18 EDM test tracks updated with 6-system consensus
(March 14). All tracks have beat_this, madmom, essentia, librosa, demucs_beats,
beatnet. Synced to blinkyhost.

**allin1 performance (investigated March 14):** The subprocess-per-track approach
was running allin1's 8-fold DiNAT ensemble on CPU (86s/track) instead of GPU
(~1s/track), plus 4.2s subprocess startup overhead per track, plus duplicate Demucs
separation. Total: ~104s/track. Fix: batch subprocess (load models once) + GPU
inference + reuse pre-separated Demucs stems. Expected: ~1.6s/track = 64× faster.
Implementation blocked on Demucs batch separation (shared with stem-augmented
training, see below).

### ~~Next: Consensus v5~~ — DONE (March 15)

Consensus v5 generated with all 7 systems. 6993 labels. base.yaml updated to use v5.
Dual-model training (OnsetNN + RhythmNN) in progress on v5 data.

## Training Recipe (all applied via base.yaml)

1. **Shift-tolerant BCE loss** — ±48ms annotation jitter tolerance (+12.6 F1 on downbeat in Beat This!)
2. **SpecAugment** — 2 freq masks (max 4 bands), 1 time mask (max 8 frames)
3. **Pitch shift** — ±5 semitones via prepare_dataset.py --augment
4. **Knowledge distillation** — Deferred until teacher labels regenerated

## Next: Demucs Stem-Augmented Training

**Status: PLANNED (after W192 baseline completes)**

### Motivation

The model must learn to detect kicks and snares in full mixes where they compete
with bass, synths, and vocals. Demucs HTDemucs can separate any track into 4 stems:
drums, bass, vocals, other. These stems provide **free, high-quality training
augmentations** that directly target the model's task:

- **Drums only**: Pure percussion — teaches the model exactly what beat-relevant
  features look like in mel space, without any masking from other instruments
- **No-vocals (drums+bass+other)**: Simulates instrumental sections and breakdown
  passages where vocals drop out — common in EDM
- **Drums+bass**: Emphasizes the rhythmic foundation (kick+bass alignment is a
  key feature for downbeat detection in EDM)

Each variant uses the **same beat/downbeat labels** as the full mix — the beats
happen at the same times regardless of which instruments are audible. This is
fundamentally different from pitch shift or time stretch augmentation, which modify
timing or spectral characteristics. Stem augmentation modifies the **instrument
mix** while preserving beat positions exactly.

### Why This Matters for Our Model

The current model trained on full mixes must learn to ignore vocals, synth pads,
hi-hats, and other non-beat-relevant content. By also training on isolated drums,
the model can directly learn kick/snare mel patterns. This is especially valuable
for downbeat detection — in many EDM genres, the kick pattern on beat 1 differs
from beats 2-4 (e.g., the "four-on-the-floor" kick with added bass on beat 1).

Published work supports this: Beat Transformer (ISMIR 2022) and Drum-Aware Ensemble
(IEEE 2021) both showed improved beat/downbeat detection from drum-separated input.
The `demucs_beats` labeling system already proved this — it runs Beat This! on
isolated drums and produces annotations with independent error characteristics.

### Pipeline

#### Step 1: Batch Demucs Separation (~2 hours GPU)

Run HTDemucs on all ~7000 tracks, saving all 4 stems. This also produces the
stems needed for the allin1 optimization (stem reuse).

```bash
# New script: scripts/batch_demucs_separate.py
# Loads HTDemucs once, processes all tracks sequentially on GPU
# Saves: {demix_dir}/htdemucs/{track_id}/drums.wav, bass.wav, other.wav, vocals.wav
# ~1s/track on GPU = ~2 hours for 7000 tracks
# Disk: ~70 GB (4 stems × 30s × 44.1 kHz × 2ch × 2 bytes × 7000 tracks)
tmux new-session -d -s demucs "cd ml-training && source venv/bin/activate && \
    python scripts/batch_demucs_separate.py \
    --audio-dir /mnt/storage/blinky-ml-data/audio/combined \
    --output-dir /mnt/storage/blinky-ml-data/stems \
    --device cuda 2>&1 | tee outputs/demucs_separate.log"
```

#### Step 2: Extend prepare_dataset.py

Add a `--stems-dir` flag that generates additional training variants per track:

```
For each track:
  1. Full mix (original) → all existing augmentations (gain, noise, RIR, pitch, stretch)
  2. Drums only → clean mel spectrogram (same beat labels)
  3. No-vocals (drums+bass+other) → clean mel spectrogram (same beat labels)
  4. Drums+bass → clean mel spectrogram (same beat labels)
```

Stem variants get clean audio only (no noise/gain/RIR augmentation on top). The
stems themselves ARE the augmentation — adding noise to isolated drums would
defeat the purpose.

This adds 3 variants per track at original speed, increasing the base dataset by
~3× before other augmentations. With 50% subsampling per epoch, the model sees
a diverse mix of full and separated audio each epoch.

**Data budget**: With chunk_frames=384, current full-mix data is ~130 GB. Adding
3 stem variants = ~520 GB total. Available disk: 450 GB. Tight.

Options:
- Store stems on `/mnt/storage/` (435 GB free) and only add drums-only (1 variant, ~260 GB total) — safe
- Use drums-only + no-vocals (2 variants, ~390 GB) — fits but tight
- All 3 variants — needs disk cleanup or second drive

**Recommendation**: Start with drums-only variant (1 extra) — this is the highest
impact and fits comfortably. Add no-vocals later if needed.

#### Step 3: Retrain W192 on Stem-Augmented Data

```bash
python scripts/prepare_dataset.py \
    --config configs/frame_fc_w192.yaml \
    --augment \
    --stems-dir /mnt/storage/blinky-ml-data/stems \
    --stem-variants drums \
    --mic-profile data/calibration/mic_profile.npz \
    --exclude-dir ../blinky-test-player/music/edm \
    --rir-dir /mnt/storage/blinky-ml-data/rir/processed

python train.py --config configs/frame_fc_w192.yaml --output-dir outputs/w192_stems
```

### Expected Impact

| Training Data | Beat Signal | Downbeat Signal |
|---------------|------------|-----------------|
| Full mix only | Kicks masked by bass/synths | Bar structure obscured by arrangement |
| + Drums only | Clean kick/snare patterns | Kick accent patterns (beat 1 vs 2-4) visible |
| + No-vocals | Instrumental rhythm | Bass+kick alignment on downbeats |

The drums-only variant should disproportionately help downbeat detection, since
kick drum accent patterns (stronger kick on beat 1, hi-hat variations across the bar)
are the primary acoustic cue for bar position in EDM.

### Dependency: allin1 Optimization

The batch Demucs separation in Step 1 also enables the allin1 labeling optimization.
Currently allin1 is slow because it runs Demucs internally per-subprocess. With
pre-separated stems:

1. allin1's `analyze()` detects existing stems in `demix_dir` and skips separation
2. Combined with a batch subprocess (single model load), expected speedup: ~64×
3. All ~7000 tracks in ~6 hours instead of 400+

Run allin1 labeling after Demucs separation to get both benefits.

## Completed Work

- **W32 cal63 (deployed March 12)**: Beat F1=0.49, DB F1=0.24. Mel calibration fixed (target_rms_db -35→-63 dB).
- **W64 (deployed ACM0 March 14)**: On-device 11/18 wins vs W32. Mean error 19.1% vs 24.6%.
- **fc_improved_v1 (shift-BCE + SpecAugment, W32)**: Beat F1=0.477, DB F1=0.169. Regression — shift-BCE alone doesn't help without larger context.
- **BandFlux removal (v67)**: 10 files deleted, ~2600 lines, ~24 settings.
- **NN always-on (v68)**: TFLite required dependency.

## File Reference

| File | Purpose |
|------|---------|
| `configs/frame_fc_w192.yaml` | W192 model config (current target) |
| `configs/base.yaml` | Shared audio/training/loss settings |
| `scripts/label_beats.py` | Multi-system labeling (7 systems) |
| `scripts/_allin1_helper.py` | All-In-One helper (with DBN threshold fix) |
| `scripts/_beatnet_helper.py` | BeatNet venv311 subprocess helper |
| `scripts/merge_consensus_labels_v2.py` | Consensus merging (7-system weights) |
| `scripts/prepare_dataset.py` | Dataset preprocessing |
| `scripts/export_tflite.py` | TFLite INT8 export |
| `train.py` | Training loop |
| `evaluate.py` | Offline evaluation |
