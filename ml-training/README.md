# Blinky Beat Activation — ML Training Pipeline

Train a small causal CNN to produce beat activation signals for the Blinky firmware. Replaces the handcrafted BandFlux ODF with a learned activation function that feeds into the existing CBSS beat tracker.

## Quick Start

```bash
source venv/bin/activate

# Standard pipeline: prepare data → train → export → evaluate
make prepare train export eval

# Or step by step:
make prepare                    # Extract mel features + augmentation
make train                      # Train model (auto-named output dir)
make export                     # Export TFLite INT8 → C header
make eval                       # Evaluate on 18-track EDM test set
```

## Configuration

Configs use a **base + override** pattern:

```
configs/
├── base.yaml        # Shared audio/training/data settings (single source of truth)
├── default.yaml     # 3-layer baseline model (overrides model + export sections)
└── wider_rf.yaml    # 5-layer wider model (overrides model + export sections)
```

`base.yaml` contains everything that must match firmware or is shared across experiments. Model configs only specify the architecture and export budget. `load_config()` merges them automatically.

```bash
# Train with wider model
make train CONFIG=configs/wider_rf.yaml

# Train with custom run name
make train RUN_NAME=v4-augmented-final
```

## Directory Structure

```
ml-training/
├── configs/
│   ├── base.yaml               # Shared settings (audio, training, data paths)
│   ├── default.yaml            # 3-layer baseline overrides
│   └── wider_rf.yaml           # 5-layer wider model overrides
├── scripts/
│   ├── audio.py                # Shared mel pipeline (torch + numpy, single source of truth)
│   ├── prepare_dataset.py      # Audio → mel spectrograms + targets (with augmentation)
│   ├── label_beats.py          # Multi-system beat labeling (Beat This!, essentia, librosa, madmom)
│   ├── merge_consensus_labels.py # Merge per-system labels into consensus
│   ├── export_tflite.py        # PyTorch → TFLite INT8 → C header
│   ├── calibrate_mic.py        # Mic calibration pipeline
│   ├── validate_features.py    # Verify Python/firmware mel feature parity
│   ├── download_fma.py         # Download FMA electronic subset
│   ├── download_giantsteps.py  # Download GiantSteps tempo dataset
│   └── download_rir.py         # Download room impulse responses
├── tools/                      # Experimental/one-off scripts (not part of pipeline)
│   ├── ab_test_bpm.js          # BPM A/B testing on device (single track)
│   ├── ab_test_batch.cjs       # Batch BPM A/B testing (all tracks)
│   ├── gain_volume_sweep.py    # ODF discriminability measurement
│   └── capture_nn_stream.py    # NN diagnostic stream capture
├── models/
│   └── beat_cnn.py             # Causal 1D CNN model definition
├── train.py                    # Training loop (BCE/focal loss, cosine LR)
├── evaluate.py                 # Offline evaluation (beat + downbeat F1)
├── Makefile                    # Pipeline orchestration
├── data/                       # (gitignored) Processed .npy files, calibration
└── venv/                       # (gitignored) Python virtual environment
```

## Output Convention

Each training run creates a timestamped directory under `/mnt/storage/blinky-ml-data/outputs/`:

```
outputs/
├── wider_rf-20260306-173448/   # Auto-named: {config}-{timestamp}
│   ├── best_model.pt           # Best validation loss checkpoint
│   ├── final_model.pt          # Last epoch checkpoint
│   ├── model_checkpoint.pt     # Full checkpoint (state_dict + config + metadata)
│   ├── training_log.csv        # Per-epoch metrics
│   ├── beat_model_int8.tflite  # Exported INT8 model
│   └── eval/                   # Evaluation results + activation plots
│       ├── eval_results.json
│       └── plots/
└── v4-final/                   # Or use a custom name: RUN_NAME=v4-final
```

Override with `make train RUN_NAME=my-experiment` for human-readable names.

## Key Constraint: Feature Parity

The model input **must exactly match** the firmware's `SharedSpectralAnalysis::getRawMelBands()`:
- 16 kHz sample rate, Hamming window, FFT-256, hop 256
- 26 mel bands (60–8000 Hz), HTK mel scale, no normalization
- Log compression: `10*log10(x + 1e-10)`, mapped [-60, 0] dB to [0, 1]

The mel pipeline is defined once in `scripts/audio.py` and imported by all scripts.

## Hardware Target

- **XIAO nRF52840 Sense** (Cortex-M4F @ 64 MHz, 256 KB RAM, 1 MB Flash)
- 3-layer model: ~9K params, ~20 KB INT8
- 5-layer model: ~15K params, ~33 KB INT8
- 16 KB tensor arena, ~3–5 ms inference per frame
- Runtime: TFLite Micro + CMSIS-NN

## Training Results

| Version | Architecture | Beat F1 | Downbeat F1 | Notes |
|---------|-------------|---------|-------------|-------|
| v2 | 3L ch32, clean data | 0.525 | 0.256 | Baseline |
| v4 (epoch 15) | 5L ch32, augmented + mic profile | **0.715** | **0.364** | +36% beat, +42% downbeat |

## Acoustic Environment Augmentation

Designed for robustness across diverse venues:
- **Volume variation**: -18 to +6 dB
- **Pink noise**: SNR 6–20 dB (crowd noise approximation)
- **Low-pass filter**: 4 kHz cutoff (boomy warehouse)
- **Bass boost**: 60–200 Hz resonance
- **Room impulse responses**: EchoThief + synthetic (warehouse, outdoor, arena, club)
- **Mic profile**: Gain-aware noise floor from calibration data (17 gain levels × 26 bands)
