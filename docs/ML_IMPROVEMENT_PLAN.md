# ML Training Improvement Plan

## Background

Baseline model (W32 frame-level FC, 55K params, plain BCE loss) achieves:
- Beat F1: 0.49 mean / 0.54 median on 18 EDM test tracks
- Downbeat F1: 0.24 mean / 0.24 median
- **Critical issue**: 4-7x downbeat over-detection on 14/18 tracks

Research review of Beat This! (ISMIR 2024 SOTA) and related work identified
proven training recipe improvements and speculative architecture changes.

## Training Recipe (proven, all applied together)

These are well-established techniques from Beat This! ablation studies. They're
independent, don't interact negatively, and are now the defaults in base.yaml.

1. **Shift-tolerant BCE loss** — ±48ms annotation jitter tolerance. Beat This!
   reports +12.6 F1 on downbeat detection. Directly addresses our over-detection
   problem. `loss.type: "shift_bce"`, `loss.shift_tolerance: 3`

2. **SpecAugment** — online freq/time masking (2 freq masks, 1 time mask per
   batch). Third most impactful augmentation in Beat This! ablation.
   `augmentation.spec_augment.enabled: true`

3. **Pitch shift augmentation** — ±5 semitones via torchaudio. Second most
   impactful augmentation (-4.3 F1 when removed). Requires data re-prep (~6-8h).
   `augmentation.pitch_shift_semitones: [-5, -3, -1, 1, 3, 5]`

4. **Knowledge distillation** — Gaussian-smoothed soft targets alongside hard
   labels. Reduces gradient noise. Teacher labels must be regenerated (stale).

## Training Plan

### Step 1: Improved recipe on existing data (in progress)

W32 with shift_bce + SpecAugment. No data re-prep needed.
Skipping distillation (stale labels) and pitch shift (not in existing data).

- **Output**: `outputs/fc_improved_v1/`
- **Baseline**: `outputs/frame_fc_cal63/eval/eval_results.json`
- **Success**: Beat F1 > 0.55, downbeat over-detection < 3x

### Step 2: Full recipe with augmented data (if Step 1 shows gains)

1. Re-prep dataset with pitch shift (`prepare_dataset.py --augment`)
2. Regenerate teacher labels (`generate_teacher_labels.py`)
3. Train with all four improvements

- **Output**: `outputs/fc_improved_v2/`

### Step 3: Architecture (only if recipe plateau is unsatisfying)

Architecture changes (SE, Conv1D hybrid, multi-window, tempo head) are
implemented in `models/beat_fc_enhanced.py` with configs ready, but are
speculative — no evidence they help our specific problem yet. Try SE-only
first (smallest change, ~340 extra params) before anything bigger.

## File Reference

| File | Purpose |
|------|---------|
| `configs/frame_fc_improved.yaml` | Improved recipe config |
| `configs/frame_fc_se.yaml` | SE-only architecture (if needed) |
| `configs/frame_fc_enhanced.yaml` | All architecture changes (if needed) |
| `models/beat_fc_enhanced.py` | Enhanced architecture implementation |
