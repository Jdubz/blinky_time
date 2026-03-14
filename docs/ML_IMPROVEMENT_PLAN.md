# ML Training Improvement Plan

## Background

Baseline model (W32 frame-level FC, 55K params, plain BCE loss) achieves:
- Beat F1: 0.49 mean / 0.54 median on 18 EDM test tracks
- Downbeat F1: 0.24 mean / 0.24 median
- **Critical issue**: 4-7x downbeat over-detection on 14/18 tracks

Research review of Beat This! (ISMIR 2024 SOTA), BeatNet+, and related work
identified high-impact improvements in two categories: training recipe changes
and architecture enhancements.

## Training Recipe Improvements

Changes to loss function, augmentation, and training strategy. Same model
architecture (FrameBeatFC 832→64→32→2, 55K params).

### 1. Shift-Tolerant BCE Loss (highest priority)

**What**: Max-pool predictions over ±3 frames (±48ms) before computing BCE at
positive target frames. Allows the model to fire slightly before/after the
annotation without penalty.

**Why**: Beat This! reports +12.6 F1 on downbeat detection. Our consensus
labels have inter-system jitter of several frames. The current loss punishes
correct predictions that are 1-2 frames off, forcing the model toward
conservative over-firing.

**Config**: `loss.type: "shift_bce"`, `loss.shift_tolerance: 3` (now default in base.yaml)

### 2. SpecAugment (online frequency/time masking)

**What**: Randomly mask 2 frequency bands (up to 4 mel bands each) and 1 time
region (up to 8 frames) per training sample. Applied online each batch.

**Why**: Prevents over-reliance on any single mel band. Forces the model to
use multiple frequency cues for beat detection. Third most impactful
augmentation in Beat This! ablation.

**Config**: `augmentation.spec_augment.enabled: true` (now default in base.yaml)

### 3. Pitch Shift Augmentation (requires data re-preparation)

**What**: Shift audio by [-5, -3, -1, +1, +3, +5] semitones using
torchaudio.functional.pitch_shift. Beat times unchanged (key-invariant).
Applied to original speed audio only.

**Why**: Second most impactful augmentation in Beat This! ablation (-4.3 F1
when removed). Forces the model to learn rhythmic patterns invariant to
key/instrumentation.

**Config**: `augmentation.pitch_shift_semitones: [-5, -3, -1, 1, 3, 5]`
**Status**: Code added to prepare_dataset.py. Requires dataset re-preparation
to take effect (~6-8 hours, dataset grows ~37%).

### 4. Knowledge Distillation (requires teacher label regeneration)

**What**: Train against Gaussian-smoothed soft targets from Beat This!
annotations alongside hard consensus labels. Blends with weight alpha.

**Why**: Softer targets reduce gradient noise from annotation jitter.

**Status**: Y_teacher_train.npy exists but is STALE (3.87M chunks vs 7.77M
in current X_train.npy). Must regenerate before use.

## Architecture Improvements

New model variants using `EnhancedBeatFC` (models/beat_fc_enhanced.py). All
features are independently toggleable. Same training interface.

### 1. Squeeze-and-Excitation Block (SE)

**What**: Learned per-band attention (26→6→26 with sigmoid). Pools over
window frames, learns which mel bands matter most.

**Why**: Aligns with design goal of triggering on kicks/snares, suppressing
hi-hats. +~340 params, negligible inference cost.

**Config**: `frame_fc_se.yaml` — `se_ratio: 4`
**Firmware**: Needs Mean + Mul ops added to resolver.

### 2. Conv1D Front-End (Hybrid)

**What**: Small causal Conv1D (26→32ch, k=5) extracts local temporal features
before the FC sees the flattened window. No BatchNorm, no residuals.

**Why**: Captures onset shapes (80ms receptive field) that flat FC misses.
Conv1D-wide eval showed near-FC accuracy at half model size.

**Config**: `frame_fc_hybrid.yaml` — `conv_channels: 32, conv_kernel: 5`
**Firmware**: Uses Conv2D + Pad ops already in resolver.

### 3. Multi-Window Dual Path

**What**: Two parallel FC paths — short window (16 frames = 256ms) for onset
sharpness, long window (48 frames = 768ms) for rhythmic context. Concatenated
before output head.

**Why**: "Low-res encoder / high-res decoder" principle — onset precision and
rhythmic structure need different temporal scales.

**Config**: `frame_fc_multiwindow.yaml` — `window_frames: 48, short_window: 16`
**Firmware**: Needs Cropping1D + Concatenation ops added to resolver.

### 4. Tempo Auxiliary Head (training only)

**What**: Third output classifying tempo into 20 bins (60-200 BPM). Discarded
at export. BPM targets derived from beat annotation inter-beat intervals.

**Why**: Bock et al. (ISMIR 2019) report +5 F1 from tempo regularization.
Hidden features learn tempo-informative representations.

**Config**: `frame_fc_enhanced.yaml` — `num_tempo_bins: 20`
**Firmware**: No changes needed (head discarded at export).

## Training Plan

### Phase 1: Recipe on Existing Data (Run 1, in progress)

Train W32 with shift_bce + SpecAugment on existing dataset.
No distillation (stale labels), no pitch shift (not in existing data).

**Output**: `outputs/fc_improved_v1/`
**Baseline comparison**: `outputs/frame_fc_cal63/eval/eval_results.json`

### Phase 2: Full Recipe with Regenerated Data (Run 2)

After Run 1 confirms gains:
1. Regenerate teacher labels (generate_teacher_labels.py)
2. Re-prepare dataset with pitch shift augmentation (prepare_dataset.py --augment)
3. Train with shift_bce + SpecAugment + distillation + pitch shift

**Output**: `outputs/fc_improved_v2/`

### Phase 3: Architecture Experiments (Run 3+)

After recipe validated, test architecture changes incrementally:
- 3a: SE block only (smallest change, isolates mel-band attention)
- 3b: Conv1D hybrid (if 3a helps)
- 3c: Full enhanced (SE + Conv1D + multi-window + tempo head)

## File Reference

| File | Purpose |
|------|---------|
| `configs/base.yaml` | Shared defaults (shift_bce, SpecAugment, pitch shift) |
| `configs/frame_fc_improved.yaml` | Phase 1-2 recipe (same FC architecture) |
| `configs/frame_fc_se.yaml` | SE-only architecture variant |
| `configs/frame_fc_hybrid.yaml` | Conv1D + FC hybrid variant |
| `configs/frame_fc_multiwindow.yaml` | Dual-path multi-window variant |
| `configs/frame_fc_enhanced.yaml` | All architecture improvements combined |
| `train.py` | Training loop (shift_bce, SpecAugment, tempo aux loss) |
| `scripts/prepare_dataset.py` | Dataset prep (pitch shift augmentation) |
| `scripts/export_tflite.py` | TFLite export (enhanced model support) |
| `models/beat_fc_enhanced.py` | Enhanced FC architecture |
