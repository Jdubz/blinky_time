# Blinky Beat Activation — ML Training Pipeline

Train a small causal CNN to produce beat activation signals for the Blinky firmware. Replaces the handcrafted BandFlux ODF with a learned activation function that feeds into the existing CBSS beat tracker.

See [docs/ML_TRAINING_PLAN.md](../docs/ML_TRAINING_PLAN.md) for the full plan, motivation, and architecture details.

## Quick Start

```bash
# Install dependencies
pip install -r requirements.txt

# Prepare dataset from existing test tracks
python scripts/prepare_dataset.py --config configs/default.yaml

# Train model
python train.py --config configs/default.yaml

# Export to TFLite INT8 C header
python scripts/export_tflite.py --config configs/default.yaml
```

## Directory Structure

```
ml-training/
├── configs/default.yaml     # Training hyperparameters (must match firmware)
├── scripts/
│   ├── download_fma.py      # Download FMA electronic subset
│   ├── label_beats.py       # Auto-label with Beat This! (GPU)
│   ├── validate_labels.py   # Cross-check labels
│   ├── prepare_dataset.py   # Audio → mel spectrograms + labels
│   └── export_tflite.py     # Keras → TFLite INT8 → C header
├── models/
│   ├── beat_cnn.py          # Causal 1D CNN definition
│   └── tcn.py               # TCN alternative
├── train.py                 # Training script
├── evaluate.py              # Offline evaluation
├── data/                    # (gitignored) Audio + labels + processed
└── outputs/                 # (gitignored) Models, checkpoints, logs
```

## Key Constraint: Feature Parity

The model input **must exactly match** the firmware's `SharedSpectralAnalysis` output:
- 16 kHz sample rate, FFT-256, hop 256, 26 mel bands (60-8000 Hz)
- All parameters defined in `configs/default.yaml`

## Hardware Target

- **XIAO nRF52840 Sense** (Cortex-M4F @ 64 MHz, 256 KB RAM, 1 MB Flash)
- Model budget: ~14 KB weights (INT8), ~8 KB tensor arena, ~3-5 ms inference
- Runtime: TFLite Micro + CMSIS-NN
