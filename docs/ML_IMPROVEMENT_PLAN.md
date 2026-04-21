# ML Training Improvement Plan

> **F1-evaluation caveat (2026-04-20).** Every F1 number in this document was measured on the 18 tracks in `blinky-test-player/music/edm/`. All 18 are inside the v27-hybrid training corpus (14 train, 4 val, 0 held out). Current F1 numbers are upper bounds — realistic eval requires a held-out EDM test split. See `docs/HYBRID_FEATURE_ANALYSIS_PLAN.md` "Training-set contamination" for action items.

## Background

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

## Active Priority: Dual-Model Architecture

**Status: TRAINING (March 15, 2026)**

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
