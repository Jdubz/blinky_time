# Blinky Beat Activation — ML Training Pipeline

Train a small causal CNN to produce beat activation signals for the Blinky firmware. Replaces the handcrafted BandFlux ODF with a learned activation function that feeds into the existing CBSS beat tracker.

See [docs/ML_TRAINING_PLAN.md](../docs/ML_TRAINING_PLAN.md) for the full plan, motivation, and architecture details.

## Quick Start

```bash
# Activate venv
source venv/bin/activate

# 1. Label existing test tracks with Beat This! (GPU)
python scripts/label_beats.py --audio-dir ../blinky-test-player/music/edm \
  --output-dir /mnt/storage/blinky-ml-data/labels/edm-test

# 2. Download room impulse responses for augmentation
python scripts/download_rir.py --output-dir /mnt/storage/blinky-ml-data/rir

# 3. Prepare dataset (with acoustic environment augmentation)
python scripts/prepare_dataset.py --config configs/default.yaml \
  --audio-dir ../blinky-test-player/music/edm \
  --labels-dir /mnt/storage/blinky-ml-data/labels/edm-test \
  --augment

# 4. Train model (CPU is fine for this tiny model)
CUDA_VISIBLE_DEVICES="" python train.py --config configs/default.yaml

# 5. Export to TFLite INT8 C header
CUDA_VISIBLE_DEVICES="" python scripts/export_tflite.py --config configs/default.yaml

# 6. Evaluate on test tracks
CUDA_VISIBLE_DEVICES="" python evaluate.py --config configs/default.yaml \
  --audio-dir ../blinky-test-player/music/edm
```

## Full Dataset Pipeline

```bash
# Download FMA electronic subset (~5,000 tracks, 22 GB archive)
python scripts/download_fma.py --output-dir /mnt/storage/blinky-ml-data/audio/fma

# Auto-label all tracks with Beat This!
python scripts/label_beats.py --audio-dir /mnt/storage/blinky-ml-data/audio/fma \
  --output-dir /mnt/storage/blinky-ml-data/labels/fma

# Prepare with augmentation (generates ~10x variants per track)
python scripts/prepare_dataset.py --config configs/default.yaml --augment \
  --rir-dir /mnt/storage/blinky-ml-data/rir/processed
```

## Directory Structure

```
ml-training/
├── configs/default.yaml     # Training hyperparameters (must match firmware)
├── scripts/
│   ├── download_fma.py      # Download FMA electronic subset
│   ├── download_rir.py      # Download/generate room impulse responses
│   ├── label_beats.py       # Auto-label with Beat This! (GPU)
│   ├── validate_features.py # Verify Python/firmware feature parity
│   ├── prepare_dataset.py   # Audio -> mel spectrograms + targets (with augmentation)
│   └── export_tflite.py     # Keras -> TFLite INT8 -> C header
├── models/
│   └── beat_cnn.py          # Causal 1D CNN definition
├── train.py                 # Training script
├── evaluate.py              # Offline evaluation (mir_eval beat F1)
├── data/                    # (gitignored) Processed .npy files
└── outputs/                 # (gitignored) Models, checkpoints, logs
```

## Acoustic Environment Augmentation

Designed for robustness across diverse venues:
- **Volume variation**: -18 to +6 dB (near-speaker to far-from-speaker)
- **Pink noise**: SNR 6-20 dB (crowd noise approximation)
- **Low-pass filter**: 4 kHz cutoff (boomy warehouse)
- **Bass boost**: 60-200 Hz resonance (warehouse/club)
- **Room impulse responses**: Real RIRs from EchoThief + synthetic (warehouse, outdoor, arena, club)

## Key Constraint: Feature Parity

The model input **must exactly match** the firmware's `SharedSpectralAnalysis` output:
- 16 kHz sample rate, Hamming window, FFT-256, hop 256
- 26 mel bands (60-8000 Hz), HTK mel scale, no normalization
- Log compression: `10*log10(x + 1e-10)`, mapped [-60, 0] dB to [0, 1]

Use `scripts/validate_features.py` to verify parity against firmware serial output.

## Hardware Target

- **XIAO nRF52840 Sense** (Cortex-M4F @ 64 MHz, 256 KB RAM, 1 MB Flash)
- Model: ~9K params, ~20 KB INT8, ~8 KB tensor arena, ~3-5 ms inference
- Runtime: TFLite Micro + CMSIS-NN

## Notes

- **TensorFlow GPU**: CuDNN version mismatch with PyTorch's CUDA 12.4 install. Use `CUDA_VISIBLE_DEVICES=""` for TF training (model is tiny, CPU is ~6s/epoch on 18K chunks).
- **Beat This! GPU**: Works fine with PyTorch CUDA. ~0.7s/track on RTX 3080.
- **madmom**: Broken on Python 3.12 (collections.MutableSequence removed). Not needed — mir_eval provides beat evaluation metrics.
