# Beat-Synchronous Hybrid NN (Archived)

**Status: Abandoned (March 2026)**

This directory contains the training pipeline for the beat-synchronous FC
classifier (downbeat detection at beat rate ~2 Hz). The approach was prototyped
and evaluated (val_F1=0.548) but abandoned due to:

1. **Circular dependency**: Relies on CBSS beat boundaries (~28% F1) for
   feature extraction, making NN accuracy bounded by CBSS accuracy.
2. **Negligible discriminative power**: Cohen's d < 0.13 between downbeat
   and non-downbeat accumulated spectral features.
3. **Misalignment with proven approaches**: All leading beat trackers
   (BeatNet, Beat This!, madmom) use frame-level NNs, not beat-level.

The project has pivoted to **frame-level FC** (FrameBeatNN): sliding window
of raw mel frames -> FC -> beat/downbeat activation per frame.

## Contents

- `train_beat_sync.py` - PyTorch trainer
- `train_beat_sync_pipeline.sh` - End-to-end pipeline script
- `scripts/beat_feature_extractor.py` - Beat-level feature extraction
- `scripts/export_beat_sync.py` - TFLite INT8 export with z-score folding
- `models/beat_sync.py` - BeatSyncClassifier model definition
- `configs/` - Training configs (beat_sync.yaml, beat_sync_wide.yaml, beat_sync_8beat.yaml)

## Reusable Components

- `fold_normalization_into_fc1()` in export_beat_sync.py — folds z-score
  normalization into first FC layer weights. Worth reusing for frame-level FC.
- Beat-level augmentation (jitter, noise, drop/insert) may inform frame-level
  augmentation design.
