# Neural Network Beat Activation — Training Plan

*Created: March 5, 2026*

## Motivation

The handcrafted BandFlux ODF + CBSS beat tracker peaks at ~0.35 Beat F1 on mic-in-room audio. Investigation across v37-v53 conclusively shows:

- **Phase alignment** is the primary bottleneck (correct BPM at 88%, but F1 stuck at ~0.28-0.35)
- Signal chain mitigations (v47), HMM phase trackers (v46, v52), and analytical phase extraction (v42 PLP) all failed to improve F1
- **Every published system achieving >60% F1 uses a neural network activation function** (madmom, BeatNet, BTrack-NN, Beat This!)

The plan: train a small causal CNN to replace BandFlux as the onset detection function, producing a learned "beat activation" signal. This feeds directly into the existing CBSS beat tracker — only the ODF source changes. The model runs on the XIAO nRF52840 Sense (64 MHz Cortex-M4F, 256 KB RAM) via TFLite Micro + CMSIS-NN.

## Architecture Overview

```
Current:  Mel bands → BandFlux (handcrafted) → ODF → CBSS → beats
Proposed: Mel bands → Causal CNN (learned)    → ODF → CBSS → beats
                      ↑ only this changes
```

The 26 mel bands already computed in `SharedSpectralAnalysis.h` are the CNN input. The CNN outputs a beat activation value (0-1) per frame at ~62.5 Hz. Everything downstream (CBSS, Bayesian tempo, phase tracking) remains unchanged.

## Phase 1: Data Acquisition

### Training Audio Sources

| Source | Tracks | License | Notes |
|--------|--------|---------|-------|
| **FMA-medium** (electronic subset) | ~5,000 | CC | Filter `genre_top == "Electronic"`. 30s clips, 44.1 kHz |
| **MTG-Jamendo** (electronic tags) | ~8,000 | CC | Filter by electronic/dance/techno tags |
| **NCS releases** | ~500 | CC-BY | High quality EDM, free download |
| **GiantSteps Tempo** | 664 | Research | Already beat-annotated, EDM focused |
| **GiantSteps Key** | 604 | Research | EDM, useful for data diversity |
| **GTZAN** (electronic) | ~100 | Research | Small but well-known benchmark |
| **Existing test set** | 18 | Local | Already annotated in `blinky-test-player/music/edm/` |

**Target**: ~5,000-10,000 tracks. Start with FMA electronic subset + GiantSteps.

### Auto-Labeling Pipeline

Use **Beat This!** (ISMIR 2024 SOTA, 97.5-98.2% F1 on dance/EDM) to generate beat annotations:

```
Audio file → Beat This! (GPU) → beat times + downbeat times → .beats.json
```

- Run on GPU machine (10GB VRAM or future DGX Spark)
- Beat This! is open-source: `pip install beat-this`
- Output format matches existing `.beats.json` files in `blinky-test-player/music/edm/`
- **Validation**: Cross-check random 50-track subset against madmom (both must agree within 70ms)
- Existing `blinky-test-player/scripts/annotate-beats.py` uses librosa — upgrade to Beat This! for training data

## Phase 2: Feature Extraction

The model must consume the **exact same input** the firmware produces to ensure training/inference parity.

### Firmware Audio Pipeline
```
PDM 16 kHz → FFT-256 (hop 256) → 128 magnitude bins → 26 mel bands (60-8000 Hz)
```

Key parameters (from `SharedSpectralAnalysis.h`):
- Sample rate: 16,000 Hz
- FFT size: 256 (hop 256, no overlap)
- Frequency bins: 128 (0-8 kHz)
- Mel bands: 26 (60-8000 Hz, triangular filterbank)
- Frame rate: ~62.5 Hz

### Python Replication
```python
sr = 16000
n_fft = 256
hop_length = 256
n_mels = 26
fmin = 60
fmax = 8000

mel_spec = librosa.feature.melspectrogram(
    y=audio, sr=sr, n_fft=n_fft, hop_length=hop_length,
    n_mels=n_mels, fmin=fmin, fmax=fmax
)
log_mel = np.log1p(mel_spec)  # Match firmware log compression
```

### Target Labels
Per frame (~62.5 Hz): beat activation value 0-1, Gaussian-smoothed (σ=2 frames / ±32ms) around beat positions. Matches madmom's training target format.

## Phase 3: Model Architecture

### Causal 1D CNN

```
Input: 26 mel bands × T frames (causal context window)
    ↓
Conv1D(26→32, kernel=3, causal padding) + ReLU + BatchNorm
    ↓
Conv1D(32→32, kernel=3, dilation=2, causal) + ReLU + BatchNorm
    ↓
Conv1D(32→32, kernel=3, dilation=4, causal) + ReLU + BatchNorm
    ↓
Conv1D(32→1, kernel=1) + Sigmoid
    ↓
Output: beat activation (0-1) per frame
```

**Why causal**: Real-time, zero lookahead. Each output depends only on current and past frames.

**Receptive field**: Dilations [1, 2, 4] → 21 frames ≈ 336ms. Covers one full beat at 180 BPM (333ms).

### Size Budget

| Metric | Value |
|--------|-------|
| Parameters | ~3,500 ≈ 14 KB INT8 |
| RAM (activations) | ~6 KB |
| Inference time | ~3-5 ms per frame (Cortex-M4F @ 64 MHz + CMSIS-NN) |
| Frame budget | 16 ms (62.5 Hz) → ample headroom |

### Quantization
- Quantization-Aware Training (QAT) from the start via `tensorflow_model_optimization`
- Export as INT8 TFLite model (4× smaller, 2-4× faster with CMSIS-NN)

## Phase 4: Training

### Framework
TensorFlow/Keras (required for TFLite Micro export).

### Training Configuration
- Input: 26 × 128 frame chunks (~2s)
- Loss: Binary cross-entropy, weighted ~10:1 (positive:negative)
- Optimizer: Adam, lr=1e-3, cosine decay
- Batch size: 64
- Epochs: 50-100
- Validation split: 15%

### Export Pipeline
```
Keras model → QAT → TFLite INT8 → xxd -i → beat_model_data.h (C array)
```

## Phase 5: Firmware Integration

### New Files
- `blinky-things/audio/BeatActivationNN.h` — TFLite Micro inference wrapper
- `blinky-things/audio/beat_model_data.h` — Quantized model weights as C array

### Integration Point
```cpp
float odf;
if (useNNActivation) {
    odf = beatActivationNN_.infer(spectral_.getMelBands());
} else {
    odf = ensembleDetector_.getFusedStrength();  // BandFlux fallback
}
addOdfSample(odf);
```

### Memory Budget

| Component | RAM | Flash |
|-----------|-----|-------|
| Model weights | 0 (in flash) | ~14 KB |
| Tensor arena | ~8 KB | 0 |
| TFLite runtime | ~4 KB | ~40 KB |
| **Total NN overhead** | **~12 KB** | **~54 KB** |
| **Current usage** | 22 KB / 256 KB | 300 KB / 1 MB |
| **After NN** | ~34 KB (13%) | ~354 KB (35%) |

### TFLite Micro Setup
- Arduino library: `tensorflow-lite-micro` (Seeed verified port for nRF52840)
- CMSIS-NN delegate for INT8 acceleration
- Pre-allocated tensor arena (~8 KB), no dynamic allocation

## Phase 6: Validation

Follow [Testing Methodology](TESTING_METHODOLOGY.md):
1. **Tier 1** (smoke): Flash, verify beats fire, BPM plausible
2. **Tier 2** (quick A/B): 3 tracks, NN vs BandFlux
3. **Tier 3** (reliable A/B): 4 tracks × 3 runs
4. **Tier 4** (full validation): 18 tracks × 3 runs, new baseline

**Expected improvement**: BandFlux F1 ~0.28-0.35 → NN activation **0.50-0.70** F1 (based on madmom's improvement over handcrafted ODFs with same-quality tracker).

## Implementation Order

1. Create `ml-training/` folder with scaffolding (README, requirements, configs)
2. Feature extraction script — must match firmware mel pipeline exactly
3. Model definition — causal 1D CNN
4. Training script — end-to-end loop
5. Export script — Keras → TFLite INT8 → C header
6. Data download/labeling scripts — requires GPU machine
7. Offline evaluation
8. Firmware integration — `BeatActivationNN.h`, Arduino TFLite library

Steps 1-5 run locally. Steps 6-7 need GPU machine for Beat This!. Step 8 after model is validated offline.

## References

- **Beat This!** — Foscarin et al., ISMIR 2024. State-of-the-art beat/downbeat tracking.
- **madmom** — Böck et al., 2016. DBN beat tracker with RNN activation.
- **BeatNet** — Heydari & Cwitkowitz, 2021. Real-time joint beat/downbeat tracking.
- **TFLite Micro on nRF52840** — Seeed Wiki, keyword spotting example on XIAO Sense.
