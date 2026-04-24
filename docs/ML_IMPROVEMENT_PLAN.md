# ML Training Improvement Plan

> **2026-04-24 audit**: v30's on-device failure triggered a deep audit of the entire audio analysis stack. 24 findings across training data prep, training loop, TFLite export, firmware audio path, and validation observability. Detailed in `docs/AUDIO_SYSTEM_AUDIT_2026_04_24.md`. Fix plan tracked as tasks #77-#88. Key items that would have caught v30 automatically: activation-distribution logging (training + validation), peak-picked val F1, FP32 output quantization investigation.

> **Corpus framing (2026-04-23).** The **primary** evaluation corpus is now `blinky-test-player/music/edm/` (18 tracks, deployment-representative: afrobeat, amapiano, breakbeat, dnb, dubstep, garage, reggaeton, techno, trance). The **secondary** corpus is `blinky-test-player/music/edm_holdout/` (25 GiantSteps LOFI tracks — hard/adversarial content where VISUALIZER_GOALS §5 says organic mode is the correct response).
>
> edm/ tracks were byte-identical with training audio through v29. v30 is the first model prepared with both `--exclude-dir ../edm` **and** `--exclude-dir ../edm_holdout`, making both corpora clean held-out sets. A single-valued `--exclude-dir` argparse bug (fixed 2026-04-23) previously silently dropped one of the two exclusions.
>
> **Pre-v30 F1 numbers on edm/ are training-contaminated upper bounds.** The v29 "ungated baseline on edm/" recorded in this doc (P=0.55, F1=0.547) is the cleanest pre-fix deployment-representative number we have; interpret with a contamination discount until v30 results replace it.

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
