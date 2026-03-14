# ML Training Improvement Plan

## Background

Baseline model (W32 frame-level FC, 55K params, plain BCE loss) achieves:
- Beat F1: 0.49 mean / 0.54 median on 18 EDM test tracks
- Downbeat F1: 0.24 mean / 0.24 median
- **Critical issue**: 4-7x downbeat over-detection on 14/18 tracks

Root cause analysis found two problems: (1) training recipe uses plain BCE
which punishes correct predictions offset by 1-2 frames, and (2) downbeat
consensus comes from only 2 of 4 systems (essentia and librosa provide NO
downbeats), giving only 40.8% inter-system agreement.

## Labeling Pipeline Improvements

### Current systems (4)

| System | Beats | Downbeats | Architecture | Weakness |
|--------|-------|-----------|-------------|----------|
| Beat This! | yes | yes | TCN+Transformer | — |
| madmom | yes | yes | RNN+DBN | Python 3.11 only |
| essentia | yes | **no** | signal processing | No downbeats |
| librosa | yes | **no** | onset energy | No downbeats, weakest accuracy |

### New systems (3)

| System | Beats | Downbeats | Architecture | Why add |
|--------|-------|-----------|-------------|---------|
| demucs_beats | yes | yes | Demucs drum separation → Beat This! on drum stem | Independent errors from percussion-only analysis. Published +4% downbeat (ISMIR 2022) |
| beatnet | yes | yes | CRNN + particle filter | Architecturally distinct from all others. Provides meter inference |
| allin1 | yes | yes | NN + structure analysis | Structure-aware (verse/chorus). Trained on Harmonix human annotations |

This gives **5 systems with downbeat capability** (vs current 2), which should
dramatically improve downbeat consensus quality.

### Implementation

All three new systems are added to `scripts/label_beats.py`:
- **demucs_beats**: Runs in main venv (Python 3.12). Uses Demucs for drum
  separation on GPU, then Beat This! on the drum stem. ~5-8s/track on GPU.
- **beatnet**: Runs via venv311 subprocess (`_beatnet_helper.py`). CRNN +
  particle filter in offline mode. pyaudio mocked (only needed for streaming).
- **allin1**: Runs via venv311 subprocess (`_allin1_helper.py`). Also provides
  structural segments (intro/verse/chorus) as bonus metadata.

Usage: `python scripts/label_beats.py --systems beatnet,allin1,demucs_beats`

Labels are additive — new systems produce new `.beats.json` files alongside
existing ones. No need to re-run beat_this/essentia/librosa/madmom.

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

### Step 2: Expanded consensus labels

Run new labeling systems (beatnet, allin1, demucs_beats) on all ~7000 tracks.
Merge into consensus_v5 with 5 downbeat-capable systems. Re-prep dataset.

### Step 3: Full recipe with expanded labels + augmented data

1. Re-prep dataset with pitch shift + consensus_v5 labels
2. Regenerate teacher labels
3. Train with all improvements

- **Output**: `outputs/fc_improved_v2/`

### Step 4: Architecture (only if recipe plateau is unsatisfying)

Architecture changes (SE, Conv1D hybrid, multi-window, tempo head) are
implemented in `models/beat_fc_enhanced.py` with configs ready, but are
speculative. Try SE-only first (~340 extra params) before anything bigger.

## File Reference

| File | Purpose |
|------|---------|
| `scripts/label_beats.py` | Multi-system labeling (7 systems) |
| `scripts/_beatnet_helper.py` | BeatNet venv311 subprocess helper |
| `scripts/_allin1_helper.py` | All-In-One venv311 subprocess helper |
| `scripts/merge_consensus_labels_v2.py` | Consensus merging (update for new systems) |
| `configs/frame_fc_improved.yaml` | Improved recipe config |
| `models/beat_fc_enhanced.py` | Enhanced architecture (if needed) |
