# Audio System Audit — 2026-04-24

Point-in-time deep audit of the entire audio analysis stack (training data prep, training loop, TFLite export, firmware audio path, validation framework) triggered by v30's on-device failure mode. Goal: surface bugs, gaps, and observability holes so the next model iteration is guided by data instead of guesswork.

## Context

**What happened.** v30 was trained on broader `onsets_consensus` labels (any musical transient, not just kicks). Val F1 was 0.31 (flat from epoch 5). On-device held-out F1 on edm/ was 0.29 with P=0.57, R=0.22. A threshold sweep across 6 values {0.05, 0.10, 0.15, 0.20, 0.25, 0.30} with all 4 devices × 18 tracks per value produced F1 in the range 0.277–0.297 — essentially flat.

Offline TFLite inference on a single track (trance-party.mp3) revealed v30's output has **half the dynamic range of v29**: std 0.15 vs 0.34, min 0.137 vs 0.004, 99th percentile 0.86 vs 0.996. The peak-picker cannot find clean local maxima in the 0.3–0.5 noisy baseline, so no threshold rescues it.

**Why we're auditing.** This failure mode was invisible to our validation harness. We had to run an offline Python inference script to diagnose it. That's unacceptable — every future model iteration should produce data that tells us directly why it succeeded or failed.

**Audit scope.** Four parallel investigations:
1. Training data prep pipeline (`prepare_dataset.py`, `generate_*.py`, `audio.py`)
2. Training loop + validation metrics (`train.py`, `models/onset_conv1d.py`, `evaluate.py`)
3. TFLite export + firmware audio path (`export_tflite.py`, `SharedSpectralAnalysis`, `AudioTracker`, `FrameOnsetNN`)
4. Validation + observability (`blinky-server/testing/*`, firmware debug streams)

---

## Executive summary

**Root cause of v30's failure** (high confidence): a combination of
1. Continuous-strength `onsets_consensus` labels produced a soft regression model that outputs continuous activations rather than bimodal peak-vs-baseline.
2. INT8 output quantization (`inference_output_type=tf.int8` with calibration on training activations) further compressed the dynamic range.
3. The firmware peak-picker, designed for v29's sharp-peak output, cannot reliably find local maxima in v30's noisy 0.3–0.5 baseline.

**Why we didn't catch it at training time:** the val metric is frame-level F1 at threshold=0.5, which measures something different from on-device peak-picked F1. We never logged activation distribution during training. We never ran offline peak-picking against the val set.

**Why we didn't catch it at validation time:** the validation framework captures `onsetTracking.{precision, recall, f1}` but not activation distribution stats. A single "activation std" number would have flagged v30 as abnormal before we burned 30 minutes on the threshold sweep.

**24 findings total** across the four audits. 6 are likely data-corruption bugs, 4 are train/inference mismatches, and 14 are observability gaps. Detailed in §Findings below.

---

## Findings

### A. Training data prep

**A1. Time-stretch truncates audio but not labels ★★ (HIGH)**
- `ml-training/scripts/prepare_dataset.py` ~line 1150
- When `speed != 1.0`, `src_beats = beat_times / speed` shifts labels earlier in the stretched timeline. If the stretched audio is shorter than the max shifted beat time, labels past the end silently get `continue`'d in `_binary_targets` (lines 269-271).
- **Blast radius:** 10–30% of augmented chunks on time-stretched variants. Silent data corruption — model trains on misaligned audio/labels.
- **Fix:** Assert `max(src_beats) * frame_rate <= len(src_audio) * frame_rate / sr` after stretch; log any dropped labels with track name.

**A2. Silent out-of-bounds label drop**
- `ml-training/scripts/prepare_dataset.py:269-271`
- `if 0 <= frame_idx < n_frames:` silently skips out-of-bounds labels. No counter, no log.
- **Blast radius:** MEDIUM. End-of-track labels vanish if audio is shorter than expected (e.g., due to A1 or MP3 decoding quirks).
- **Fix:** Count and log per track: `{audio_path.stem}: N/M labels out of bounds`.

**A3. `--exclude-dir` existence not validated**
- `ml-training/scripts/prepare_dataset.py:1245` (argparse action fixed yesterday; validation still missing)
- If a user passes `--exclude-dir /nonexistent` there's no error; `rglob("*")` silently produces nothing.
- **Blast radius:** LOW (the matching "Excluded N" log would show 0, which *should* trigger suspicion — but we just saw yesterday this is exactly what masked the original bug for a full training run).
- **Fix:** `raise FileNotFoundError` if any exclude-dir doesn't exist.

**A4. Disk precheck still too lenient (200 GB)**
- `ml-training/scripts/prepare_dataset.py:1308`
- Merge step needs final arrays (≈130 GB × 3 files) + shard intermediates (≈160 GB) + merge workspace (≈130 GB × 1.2) → ~420+ GB total required. 200 GB starting slack isn't always enough.
- **Blast radius:** HIGH. We already hit this Wednesday mid-v30 merge.
- **Fix:** Raise to 450 GB, or compute dynamically from number of shards × estimated shard size + target chunks × chunk shape × 2.2.

**A5. Label-pipeline observability — no onset-density histogram**
- Prep-time validation (lines ≈1963-2016) computes global positive ratio but no per-track density percentiles.
- Had this existed, kick_weighted's 152.8 events/track vs consensus's 75.9 would have screamed "label contamination" on the first run.
- **Fix:** Add `Onset density percentiles (events/min): p10, p50, p90 — ideal 8-16 @ 120 BPM`.

**A6. Label-pipeline observability — no strength-distribution histogram**
- `onsets_consensus` labels have continuous strengths [0, 1]. v30's key problem (continuous → compressed output) was directly traceable to strength distribution. No histogram of label strengths is logged.
- **Fix:** Log `Strength histogram: bins [0, 0.2, 0.4, 0.6, 0.8, 1.0] counts [...]` in prep validation.

**A7. Stem-bleed detection missing in kick_weighted generator**
- `ml-training/scripts/generate_kick_weighted_targets.py:128`
- RMS>0.005 check detects silent stems but not contaminated ones (50% kick + 50% bass bleed passes).
- **Fix:** Cross-band overlap check — if kick-time vs hihat-time overlap > 50%, mark `contaminated: true` in output JSON.

---

### B. Training loop & validation metrics

**B1. val F1 is measured at threshold=0.5 frame-level — NOT peak-picked ★★★ (CRITICAL)**
- `ml-training/train.py:1169-1186`
- On-device evaluation uses `_peak_pick()` with tolerance windows. Val metric uses `Y_pred[:, :, 0] > 0.5` per frame.
- **This is why v30 training completed showing val_f1=0.31 and we only discovered the on-device disaster after exporting + flashing + validating.** Frame-level F1 rewards broad plateaus; peak-picking rewards sharp peaks. These optimize different behaviors.
- **Fix:** Add `val_peak_f1` every epoch — call `evaluate._peak_pick()` at a fixed threshold (e.g. 0.3) over the val set's activation output, compute F1 with ±50ms tolerance. Log both `val_f1` (frame) and `val_peak_f1` (peak-picked, firmware-realistic).

**B2. Early stopping criterion is val_loss**
- `ml-training/train.py:1203-1216`
- `best_model.pt` is saved when `val_loss < best_val_loss`. val_loss is pos_weight-scaled, so different pos_weights across runs save different epochs.
- **Blast radius:** MEDIUM. Combined with B1, the "best" model may be worse for the real metric than several other epochs.
- **Fix:** Add `early_stopping_metric` config option (choices: `val_loss`, `val_f1`, `val_peak_f1`). Default to `val_peak_f1` once B1 lands.

**B3. No activation distribution logged during training ★★★**
- `ml-training/train.py` val loop
- No `Y_pred.min/max/std/percentiles` logged per epoch.
- **Would have caught v30 at epoch 5.** When activation std monotonically decreased or capped below 0.20, we'd know the model was collapsing toward constant output.
- **Fix:** Every epoch, log `val_activation: min=X max=X mean=X std=X p5/p50/p95=X`. Add automatic warning if std < 0.15.

**B4. No pre-sigmoid logit distribution logged**
- Related to B3 but earlier in the pipeline. Logit std tells us whether the model is learning; sigmoid-output std tells us whether predictions are bimodal.
- **Fix:** Log both. If logit std is high but sigmoid std is low, the logits are saturating — possibly due to loss or label issues.

**B5. Checkpoint-to-export path inconsistency**
- `ml-training/train.py:1229+` saves `best_model.pt` (val_loss-best) and optionally SWA-averaged `final_model.pt`.
- `ml-training/scripts/export_tflite.py` defaults to `--model outputs/best_model.pt`.
- If SWA runs and regresses peak-picking F1 while lowering loss, exported model is not the one we want — but there's no cross-check.
- **Fix:** After SWA, compute val_peak_f1 on both candidates; export the winner and log the choice.

**B6. Quantization-noise wrapper may not cover output layer**
- `ml-training/models/onset_conv1d.py:69-90, 158`
- `_QuantNoiseConv1d` wraps backbone Conv1ds. The final `output_conv` may or may not be wrapped depending on how `apply_quant_noise()` traverses the module tree.
- **Blast radius:** MEDIUM if `quant_noise` is used (it's disabled by default in v29+ configs).
- **Fix:** Explicit assertion that output_conv is wrapped; document the behavior.

---

### C. Train/inference parity (export + firmware)

**C1. TFLite output INT8 quantization compresses dynamic range ★★★ (HIGH)**
- `ml-training/scripts/export_tflite.py:359, 812`
- `inference_output_type=tf.int8` means the model's final sigmoid output is quantized to INT8 range [-128, 127] and dequantized on-device. Scale/zero-point derived from activations seen on the representative dataset.
- v30 min output 0.137 (not zero) strongly suggests asymmetric zero-point combined with representative-dataset activation range that doesn't reach zero — so the quantized output can't represent low activations.
- **Directly explains v30's compressed output distribution.**
- **Fix:** Option A — change to `inference_output_type=tf.float32` (keep INT8 weights, FP32 output; small RAM cost, no precision loss). Option B — per-channel INT8 quantization. Option C — explicitly verify representative dataset covers full expected output range including silence.
- **Critical investigation:** re-export v30 best_model with FP32 output and re-validate. If F1 recovers, v30 was a quantization victim, not a label victim.

**C2. Representative dataset mixes training + device captures**
- `ml-training/scripts/export_tflite.py:527-561`
- If device capture state (AGC gain, noise floor) differs from training data distribution, calibration is biased.
- **Fix:** Log activation std of calibration vs validation set before export; warn if ratio < 0.7.

**C3. Mel-input parity untested (automated) ★**
- Python: `ml-training/scripts/audio.py:577` (Hamming → FFT → mel filterbank → log-compress).
- Firmware: `blinky-things/audio/SharedSpectralAnalysis.{h,cpp}` `computeRawMelBands()`.
- Docs claim parity gaps 1-5 closed (HYBRID_FEATURE_ANALYSIS_PLAN.md), but **no automated test verifies byte-identical mel output on identical audio input**.
- **Fix:** Add a parity test that loads a WAV clip, runs it through both the Python and firmware (via harness v2) mel computations, and asserts max abs diff < 1e-4.

**C4. OSS_FRAME_RATE=66 Hz vs Python 62.5 Hz ★★**
- Firmware: `blinky-things/audio/AudioTracker.h:193` — `OSS_FRAME_RATE = 66.0f`.
- Python: `ml-training/scripts/audio.py:29` — `FRAME_RATE = 16000 / 256 = 62.5 Hz`.
- The 5.6% discrepancy cascades through PLP phase advance, ACF period-in-frames, tempo-adaptive cooldown, `frameDurationMs` calculations.
- **Blast radius:** HIGH for tempo/beat-grid features. Possibly contributed to the PLP gate's mis-alignment finding.
- **Fix:** Investigate whether 66 Hz is a vestige or an intentional firmware-side upsampling. Either fix the firmware constant OR document why the mismatch is intentional and compensated for.

**C5. NN 3-tap FIR smoothing before peak-picking ★**
- `blinky-things/audio/AudioTracker.cpp:170, 922` — `nnSmoothed_ = 0.23*prev2 + 0.54*current + 0.23*prev1`.
- Training evaluation in `evaluate.py:_peak_pick()` uses raw activations.
- Smoothing delays peaks by ~1 frame and blurs them. v30's already-compressed output is doubly punished.
- **Fix:** Profile on-device F1 with/without smoothing via a tunable (`nn_smoothing_taps: 3|1`). The smoothing was added to reduce INT8 jitter — it may no longer be needed if C1 is fixed.

**C6. NN input quantization rounding differs**
- `blinky-things/audio/FrameOnsetNN.h:203` — sign-aware `+0.5` rounding.
- TFLite Micro uses banker's rounding.
- 1 LSB drift; low blast radius but a parity gap we can close for free.

**C7. Input normalization strategy undocumented**
- Firmware passes raw log-mel [0, 1] (clamped). Training assumes same but no runtime check that model's expected input range matches firmware output.
- **Fix:** Include `input_min/max` in the exported TFLite metadata and assert at firmware boot.

**C8. Noise subtraction code path dormant (default off)**
- `blinky-things/audio/SharedSpectralAnalysis` — `noiseEstEnabled=false` by default. Gap-2 in HYBRID plan is still open ("A/B not validated").
- **Fix:** Either validate it and turn on, or `#ifdef` it out as dead code.

---

### D. Validation & observability

**D1. No activation distribution in validation results ★★★**
- `blinky-server/blinky_server/testing/scoring.py` computes P/R/F1 but never min/max/std of activation.
- **Would have made the entire v30 offline-inference chase unnecessary.** A single "activation std = 0.15" in the validation output would have flagged it immediately.
- **Fix:** Extend `Diagnostics` to include `activation_stats: {min, max, mean, std, p5, p50, p95}` computed from `signal_frames[].activation`.

**D2. No per-firing gate state ★**
- `TransientEvent` captures time + strength. Doesn't capture which gate (if any) would-have-suppressed this firing, or which conditions let it through.
- **Fix:** Firmware adds `uint8 gate_mask` bitfield `{by_crest_gate, by_beat_grid, by_cooldown, by_pulse_threshold, by_bass_gate, by_pattern_bias}`. Each bit set = that gate was active at firing time.
- **Impact:** zero-ambiguity attribution of "why did/didn't we fire" per event.

**D3. No per-firing spectral feature snapshot**
- When a FP fires, we don't know if it was a bass boom, synth stab, vocal consonant, or chord change.
- **Fix:** Extend `TransientEvent` with `features: dict` captured at firing moment (flatness, raw_flux, centroid, crest, rolloff, hfc). Enables offline FP-class clustering.

**D4. No per-device calibration metadata**
- 26pp recall spread across 4 physically identical devices, no captured data explains the variance.
- **Fix:** Capture `device_calibration` at test start: mic hw_gain, noise floor (mean level first 500ms pre-audio), RMS distribution percentiles over the test.

**D5. PLP lock/drift events not streamed**
- Firmware computes `plpConfidence_`, `periodicityStrength_` per frame but the validation captures only aggregate mean.
- **Fix:** Stream lock events (`beat_count`, `lock_confidence_at_lock`, `drift_detected`) as an event list. Separates "never locked" from "locked then lost."

**D6. Onset-matching tolerance hardcoded 100ms**
- Makes sensitivity to timing drift invisible.
- **Fix:** Return `f1_tol_sweeps: {50, 75, 100, 150}` per track. A model that scores F1=0.7 at 100ms but F1=0.3 at 50ms has a timing problem that's hidden today.

**D7. Latency as mean only**
- Mean ±7ms can hide multimodal distributions (e.g., some firings 100ms early, others 100ms late, mean 0).
- **Fix:** Add `latency_histogram` (10ms buckets) and latency percentiles to Diagnostics.

**D8. ACF ambiguity not captured**
- Only the winning ACF peak is returned. If two equally-strong periods compete, that's ambiguity — relevant for tempo stability — but invisible.
- **Fix:** Summarize top-5 ACF peaks as a lag-ambiguity entropy score.

**D9. Beat-count correlation with confidence missing**
- PLP confidence is per-frame; can't be folded by beat identity.
- **Fix:** Stream a monotonic `beat_count` in music_states. Enables "confidence high on beats 1,5,9 (kick pattern)" analyses offline.

**D10. Firmware exposes but doesn't stream many signals**
- `AudioTracker` has accessors for `getPlpBestSource`, `getPlpReliability`, `getBeatStability`, `getOnsetDensity` that are used interactively via SerialConsole but NOT in validation data.
- **Fix:** Add these to the music_states stream under a debug-mode flag.

---

## Fix plan

Organized by ROI. Each tier's items are independent within the tier; tier ordering reflects dependencies.

### Tier 1 — Instrument the truth (highest ROI, prevents future v30-class surprises)

These are the fixes that would have surfaced v30's failure mode at training time or first validation, saving us the entire threshold-sweep + offline-inference investigation.

| # | Fix | Effort | File(s) |
|---|-----|--------|---------|
| **T1.1** | Activation distribution in validation results (D1) | ~1 h | `blinky-server/blinky_server/testing/scoring.py` |
| **T1.2** | Activation distribution every epoch during training (B3, B4) | ~2 h | `ml-training/train.py` |
| **T1.3** | Peak-picked val F1 every epoch during training (B1) | ~3 h | `ml-training/train.py`, uses `ml-training/evaluate.py:_peak_pick` |
| **T1.4** | Per-firing gate-mask bitfield (D2) | ~3 h | `blinky-things/audio/AudioTracker.cpp`, firmware music stream, server scoring |
| **T1.5** | Per-firing spectral feature snapshot (D3) | ~2 h | firmware transient emission, server `TransientEvent` |

**Total effort:** ~11 hours. **Impact:** every future model iteration produces interpretable data at training time and validation time.

### Tier 2 — Fix bugs with active blast radius (do before next training run)

| # | Fix | Effort | File(s) |
|---|-----|--------|---------|
| **T2.1** | Time-stretch audio/label alignment assert + log (A1, A2) | ~1 h | `ml-training/scripts/prepare_dataset.py` |
| **T2.2** | Disk precheck raised + dynamic calculation (A4) | ~30 min | `ml-training/scripts/prepare_dataset.py:1308` |
| **T2.3** | `--exclude-dir` existence check (A3) | ~15 min | `ml-training/scripts/prepare_dataset.py:1245` |
| **T2.4** | INT8 output quant investigation: re-export v30 with FP32 output and re-validate (C1) | ~2 h | `ml-training/scripts/export_tflite.py`, re-flash, validate |
| **T2.5** | Representative-dataset activation std check (C2) | ~1 h | `ml-training/scripts/export_tflite.py` |

**Total effort:** ~5 hours. **Impact:** eliminates silent data corruption, prevents disk-full crashes, potentially resolves v30 without retraining.

### Tier 3 — Observability expansion (medium ROI, do after v31 result)

| # | Fix | Effort | File(s) |
|---|-----|--------|---------|
| **T3.1** | Per-device calibration metadata (D4) | ~2 h | server + firmware |
| **T3.2** | PLP lock/drift event stream (D5) | ~2 h | firmware + server |
| **T3.3** | Onset-matching tolerance sweep (D6) | ~2 h | `blinky-server/blinky_server/testing/scoring.py` |
| **T3.4** | Latency histogram (D7) | ~1 h | `blinky-server/blinky_server/testing/scoring.py` |
| **T3.5** | Onset-density + strength histograms in prep (A5, A6) | ~2 h | `ml-training/scripts/prepare_dataset.py` |
| **T3.6** | Stem-bleed detection in kick_weighted generator (A7) | ~1 h | `ml-training/scripts/generate_kick_weighted_targets.py` |
| **T3.7** | Additional firmware accessors to music_states (D10) | ~1 h | firmware music stream |

**Total effort:** ~11 hours.

### Tier 4 — Structural / deferred (track, don't immediately act)

| # | Fix | Notes |
|---|-----|-------|
| **T4.1** | OSS_FRAME_RATE 66 vs 62.5 Hz reconciliation (C4) | Needs careful study — might be intentional design with compensation elsewhere. Don't just change. |
| **T4.2** | Automated mel-input byte-identical parity test (C3) | Closes harness-v2 gap permanently. Worthwhile but nontrivial. |
| **T4.3** | NN 3-tap FIR smoothing impact study (C5) | Profile with/without once T1.4 gate-mask is in place. |
| **T4.4** | Early-stopping-metric config option (B2) | Lands with T1.3 if we make val_peak_f1 the default. |
| **T4.5** | Best/SWA model cross-check (B5) | Low priority — SWA is disabled in current configs. |
| **T4.6** | ACF ambiguity entropy (D8) | Nice-to-have for tempo diagnostics. |
| **T4.7** | Beat-count correlation stream (D9) | Advanced offline analysis enabler. |
| **T4.8** | Quantization-noise wrapper coverage (B6) | Low priority — quant_noise is disabled in current configs. |
| **T4.9** | Noise subtraction validate-or-remove (C8) | Housekeeping. |

---

## Implementation ordering

```
Day 1 (~half day):
  T2.1, T2.2, T2.3 (data-prep bug fixes — no risk, high payoff)
  T1.1          (activation stats in validation — 1 hr, unblocks every future test)

Day 1 afternoon (~half day):
  T2.4          (v30 FP32-output re-export + re-validate)
                 ← critical: may make v31 unnecessary
  T1.2          (activation stats in training — bake this in before next training run)

Day 2 (if needed, v31 or v32 training):
  T1.3          (peak-picked val F1)
  T1.4 + T1.5   (firmware-side: gate mask + per-firing features)
  Launch training with full instrumentation in place.

Later weeks:
  Tier 3 items as time allows.
  Tier 4 items queued, prioritized by what the Tier 1/2 data reveals.
```

**Key branch point:** T2.4's result decides v31's fate.
- If re-exporting v30 with FP32 output produces F1 ≥ 0.50: v30 was a quantization victim. Ship it + kill v31. Apply T1.1/T1.2 retroactively to the v30 validation for baseline.
- If v30 FP32-output F1 stays ≈ 0.30: quantization wasn't the issue. v31 with bimodal-labels hypothesis remains valid.

**T2.4 result (2026-04-24 11:17):** v30 FP32-output activation distribution is **byte-identical to INT8-output** (max_abs_diff = 0.0039 = 1 LSB, std=0.1454 in both, >0.7 coverage 5.61% in both). Quantization is NOT the cause of v30's compressed output — the weights themselves produce this distribution. Tested on trance-party.mp3 using v30 best_model.pt, built with representative dataset from processed_v31 (identical mel features, labels-only differ). Finding: **v31's bimodal-labels hypothesis is the correct intervention.** Do not abort v31 training.

**v31 result (2026-04-24 21:47): bimodal-labels hypothesis was WRONG.** v31 collapsed harder than v30 — std=0.0888 vs v30's 0.15, output range [0.15, 0.86] never touching 0 or 1. The T2.5 representative-dataset check correctly flagged this offline before any device cycle. See addendum below for root cause.

---

## Measurement — how we know each fix worked

| Fix | Success criterion |
|-----|-------------------|
| T1.1 | Validation result JSON contains `diagnostics.activation_stats = {min, max, mean, std, p5, p50, p95}` for every device×track |
| T1.2 | Training log has `val_activation: min=X max=X std=X pN=X` every epoch |
| T1.3 | Training log has `val_peak_f1` every epoch; visibly correlates with eventual on-device F1 on subsequent export+validate |
| T1.4 | Per-firing JSON includes `gate_mask` byte; offline analysis can answer "what fraction of FPs were let through by the beat-grid gate" |
| T1.5 | Per-firing JSON includes `features` dict; we can cluster FPs by flatness/crest/hfc offline |
| T2.1 | Prep log includes `"N labels dropped in track X (audio truncated by time-stretch)"` lines; 0 for clean runs |
| T2.2 | Prep aborts early with clear message when projected usage > free space |
| T2.4 | v30 re-export F1 either ≥0.50 (quant was cause) or ≈0.30 (quant wasn't); data resolves the open question |

---

## What NOT to fix (and why)

- **Don't change OSS_FRAME_RATE** without investigation (T4.1). If it's intentional and compensated elsewhere, blind reconciliation introduces bugs.
- **Don't rewrite peak-picking** to handle compressed outputs. Fix the output, not the picker.
- **Don't extend shape features to NN inputs** — gate-b proved this doesn't help. Re-evaluate only after clean labels (T3.5/T3.6).
- **Don't pursue multi-channel instrument model** (#72) until we have a stable single-channel baseline that hits P≥0.50 R≥0.50 on edm/.

---

## Tracking

Related tasks (see `TaskList`): #77–#88 (created alongside this document).

This doc is a point-in-time snapshot. As fixes land, update the "Success criterion" column with observed values; don't rewrite findings (they're history).

## Implementation status — 2026-04-24 evening

All Tier 1 and Tier 2 items landed in one session, plus several Tier 3 items completed while waiting on v31 training.

| # | Task | Status |
|---|------|--------|
| T2.1 | Time-stretch alignment assert + drop counter | ✓ landed (`prepare_dataset.py`) |
| T2.2 | Disk precheck 50→450 GB + clearer error | ✓ landed |
| T2.3 | `--exclude-dir` existence validation + zero-match warning | ✓ landed |
| T2.4 | v30 FP32-output re-export investigation | ✓ complete — **quantization ruled out** as v30 cause |
| T2.5 | Activation-distribution sanity check at export time | ✓ landed (`export_tflite.py`) |
| T1.1 | Activation stats in validation `diagnostics` | ✓ landed (`scoring.py`, `types.py`) |
| T1.2 | Training-time activation stats + collapse warning | ✓ landed (`train.py`) |
| T1.3 | Peak-picked val F1 via mir_eval.onset | ✓ landed (`train.py`) |
| T1.4 | Per-firing gate-mask bitfield | ✓ landed (firmware b145 + server) |
| T1.5 | Per-firing spectral feature snapshot | ✓ landed (firmware b145 + server) |
| A5 | Onset-density estimate in prep validation | ✓ landed |
| A6 | Label-strength histogram in prep validation | ✓ landed |
| D6 | Tolerance sweep | ✓ already present (f1_50/70/100/150ms) |
| D7 | Latency histogram in validation diagnostics | ✓ landed |
| Tier 3 remaining | per-device calibration, PLP lock events, extra firmware streams | deferred (task #87 rolled up the remainder) |
| Tier 4 | structural items (OSS_FRAME_RATE, mel parity test, smoothing study) | deferred (task #88) |

Firmware bumped to b145. Model interface unchanged — existing TFLite models remain compatible. Python schemas gained optional fields (backward-compatible with pre-b145 firmware).

**Net effect.** Every future training run logs per-epoch activation distribution (min/max/mean/std/percentiles) + peak-picked F1 with automatic collapse warnings. Every future validation run includes per-firing gate mask + spectral snapshot + activation-distribution stats + latency histogram in its JSON output. Prep flags time-stretch misalignment (≥0.5% labels dropped), strength-continuity risk (<50% extremes), and abnormal onset density (>12/s) automatically.

**The entire v30 offline-inference chase would now be unnecessary** — the compressed-activation signature would appear directly in the standard validation output's `diagnostics.activationStats` field.

---

## Addendum: v31 post-mortem and v32 plan (2026-04-24 22:30)

v31 trained for 60 epochs and finished plateaued at val_loss=1.088 / val_F1=0.329 — barely above the trivial baseline. Export + T2.5 check showed activation std=0.089, *worse* than v30's 0.15. The bimodal-labels hypothesis was wrong, and not in a near-miss way: it made the problem worse.

### What actually went wrong

The `hard_binary_threshold: 0.05` in v31 only affected neighbor-weighted secondary frames, not the underlying detector noise. The unique `Y_train` values stored on disk (`[0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.4, 0.6, 0.8, 1.0]`) decompose as:

| Strength on disk | Source | systems agreeing |
|---|---|---|
| 0.05 | neighbor of 0.20 center | 1 sys |
| 0.10 | neighbor of 0.40 center | 2 sys |
| 0.15 | neighbor of 0.60 center | 3 sys |
| 0.20 (center) | 1-system detection | 1 sys |
| 0.20 (neighbor) | neighbor of 0.80 center | 4 sys |
| 0.25 | neighbor of 1.00 center | 5 sys |
| 0.40 | 2-system center | 2 sys |
| 0.60 | 3-system center | 3 sys |
| 0.80 | 4-system center | 4 sys |
| 1.00 | 5-system center | 5 sys |

`> 0.05` keeps every center plus most neighbors. v31's training set was effectively the union of all five detectors' detections — exactly the same as v30, just thresholded into 0/1 at training time.

### Per-system signal quality (the real problem)

Measured mel-diff signal-to-baseline ratio at each label, ±3-frame window, 20-track sample:

| min systems agreeing | events | per-strength signal | cumulative signal | density |
|---|---|---|---|---|
| 1/5 only | 345 | **0.43×** | 1.21× | 28/min |
| 2/5 only | 585 | **0.46×** | 1.30× | 46/min |
| 3/5 only | 1082 | **1.65×** | 1.52× | 95/min |
| 4/5 only | 396 | 1.11× | 1.40× | 35/min |
| 5/5 only | 833 | 1.54× | 1.54× | 78/min |
| reference: kick_weighted (v29) | — | 1.45× | — | — |
| reference: top-20% mel-diff (self-sup) | — | 14.4× | — | — |

**1- and 2-system "consensus" detections have signal *below random* (0.43×, 0.46×).** The label flags frames where mel-energy change is *less* than at random points in the audio. These aren't noisy onsets — they're anti-correlated with actual energy events. Most likely chord changes / harmonic onsets that single non-perceptual detectors agree on but mel features cannot see.

**The 4-system bucket (1.11×) is anomalously weak** — a separate finding worth investigating but not blocking. Possibly the merge algorithm chains harmonic detections across the 70ms tolerance window when 4 systems vote.

### Root cause restated

v31 (and v30) collapsed because **76% of their positive labels (1- and 2-system consensus events) are sub-perceptual in mel-only features**. The model regresses toward predicting the mean. Sharper labels (binarization) don't help; the underlying labels are wrong about which frames are onsets the model can learn.

v29's `kick_weighted` labels worked at 1.45× because kicks/snares have unambiguous low-frequency energy spikes — even with demucs stem-bleed contamination, the labeled frames coincide with mel-energy events.

### v32 plan — `min_systems: 3` filter at prep time

The fix is at prep, not training time. `prepare_dataset.py` now supports a `labels.min_systems` config option that filters consensus detections by their `systems` field before generating frame targets. v32 uses `min_systems: 3`, giving:

- per-strength signal: ≥1.65× (vs v31's effective 1.21×, vs v29's 1.45×)
- positive frame ratio after 3-frame plateau: ~0.165 (vs v31's 0.20)
- density: 207 events/min (vs v31's 282 — sparser but cleaner)
- broader event coverage than kick_weighted (all percussive + strongly-confirmed harmonic onsets, not just kicks/snares)

The v32 config also opts into T4.4's `early_stopping_metric: val_peak_f1`. v31 demonstrated the val_loss-best epoch is not the on-device-realistic-best epoch.

### Lessons reified into Tier 1 instrumentation

The audit's Tier 1 work caught v31 offline (T2.5 representative-dataset activation std warning fired before any device flash). Without it we'd have wasted an export+flash+validate cycle to discover the same thing on hardware. That's the system working correctly. What it didn't catch — and shouldn't be expected to — is *why* the labels failed. That required dropping below the model boundary and measuring label-vs-features correlation directly, which is now part of the v32 prep precheck planning (queue: per-strength signal logging in prep validation output, on next training cycle).

### Implementation status — addendum

| # | Task | Status |
|---|------|--------|
| `min_systems` config option in `prepare_dataset.py` | new | ✓ landed (2026-04-24 22:30) |
| v32 config (`conv1d_w16_onset_v32_mel_only.yaml`) | new | ✓ landed |
| T4.4 — `early_stopping_metric` config | audit Tier 4 | ✓ landed earlier today |
| Per-strength signal logging in prep validation | new | TODO before v32 launch |
| Investigate 4-system signal anomaly (1.11×) | new | TODO post-v32 |
