# Neural Network Beat Activation — Training Plan

*Created: March 5, 2026*
*Updated: March 5, 2026 — scaffolding complete, firmware integration done, labeling tool research done*

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

Use **Beat This!** (ISMIR 2024 SOTA, 97.5-98.2% F1 on dance/EDM) as primary labeler, cross-validated with **essentia** and **BeatNet**:

```
Audio file → Beat This! (GPU) → beat times + downbeat times → .beats.json
           → essentia (CPU)   → beat times + BPM              (cross-validation)
           → BeatNet (CPU)    → beat times + meter positions   (cross-validation)
```

### Labeling Tool Status (Tested March 5, 2026)

| Tool | Version | Beat F1 | Downbeats | Meter | Deterministic | Speed | Status |
|------|---------|---------|-----------|-------|:---:|-------|--------|
| **Beat This!** | 0.1 | ~97% (SOTA) | Yes | No | Yes (bit-identical CPU/GPU) | ~0.7s/track GPU, ~2s CPU | **Primary labeler** |
| **essentia** | 2.1b6 | ~75-80% | No | No | Yes (bit-identical) | ~2.5s/track CPU | **Cross-validation** |
| **BeatNet** | 1.1.1 | ~75-80% | Yes | Yes (1,2,3,4) | Yes (offline+DBN) | ~5s/track CPU | Needs Python 3.11 (madmom dep) |
| librosa | 0.11.0 | ~60% | No | No | Yes (bit-identical) | ~0.2s/track CPU | Too inaccurate for labeling |
| madmom | 0.16.1 | ~88% | Yes | Yes | Yes (DBN mode) | ~3s/track CPU | Broken on Python 3.12 |

### Cross-Validation Findings

**Tested on 18 EDM test tracks (March 5):**
- BPM agreement (within 5%): **17/18 tracks** (94%) between at least 2 of 3 tools
- Beat timing: Beat This! and essentia align closely (F1=0.948 on techno, mean offset 20ms)
- librosa has systematic ~30ms offset vs Beat This! and different phase grid (F1=0.008 despite same beat count)
- **Octave ambiguity**: DnB/breakbeat tracks show tool disagreement — Beat This! tracks at full tempo (170+ BPM), librosa/essentia at half-time feel (~85 BPM). Both are musically valid interpretations
- `edm-trap-electro` is the only complete disagreement (BT=500 BPM broken, lib=112, ess=139)

### Cross-Validation Strategy

1. **Primary labels**: Beat This! (3 checkpoints: final0, final1, final2 — all agree within 40ms)
2. **Quality score per track**: Pairwise F1 between Beat This! and essentia (±70ms tolerance)
3. **Flag outliers**: Tracks where BT-essentia F1 < 0.8 get manual review or exclusion
4. **BeatNet** (requires pyenv Python 3.11 venv): provides meter positions (1,2,3,4) for time signature detection and outlier identification (non-4/4)

### Outstanding: Cross-Validation Pipeline

- [ ] Install pyenv + Python 3.11 venv for BeatNet/madmom
- [ ] Build `scripts/cross_validate_labels.py` — runs all tools, computes agreement, flags outliers
- [ ] Filter GiantSteps labels (21% wrong tempo vs ground truth BPM annotations)
- [ ] Re-label existing 18-track test set with Beat This! (currently uses librosa with different timing)

### Labeling Detail Level

Beat This! outputs **beats and downbeats only** — no subdivision (8th notes, 16th notes). Activations are 0.000 between beats (tested: zero signal at midpoints). Downbeat detection uses binary strength: 1.0 = downbeat, 0.7 = regular beat, matched via 35ms proximity to Beat This! downbeat output.

- Existing `blinky-test-player/scripts/annotate-beats.py` uses librosa — produces different timing and continuous onset strength; **not compatible** with Beat This! labels
- Beat This! label format: `.beats.json` with `{hits: [{time, expectTrigger, strength}]}`

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
Conv1D(32→2, kernel=1) + Sigmoid    (2 channels when downbeat enabled)
    ↓
Output: beat activation (0-1) per frame, channel 0 = beat, channel 1 = downbeat
```

**Why causal**: Real-time, zero lookahead. Each output depends only on current and past frames.

**Receptive field**: Dilations [1, 2, 4] → 15 frames = 240ms. Covers one full beat at 250 BPM.

### Size Budget

| Metric | Value |
|--------|-------|
| Parameters | ~9,150 ≈ 20 KB INT8 (with downbeat head) |
| RAM (activations) | ~8 KB |
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

## Phase 5: Firmware Integration — DONE (v54)

### Implemented Files
- `blinky-things/audio/BeatActivationNN.h` — TFLite Micro inference wrapper with multi-output support
- `blinky-things/audio/beat_model_data.h` — Quantized model weights as C array (placeholder, needs trained model)
- `blinky-things/audio/SharedSpectralAnalysis.h/cpp` — Added `getRawMelBands()` (no compressor/whitening, matches training)
- `blinky-things/audio/AudioControl.h` — Added `float downbeat` field (24 bytes, 6 floats)
- `blinky-things/audio/AudioController.cpp` — NN integration: uses raw mel bands, bypasses ODF smoothing, passes through downbeat activation

### Integration Point (implemented in AudioController.cpp)
```cpp
float odf;
if (useNNActivation) {
    odf = beatActivationNN_.infer(spectral_.getRawMelBands());  // raw = no compressor/whitening
    control.downbeat = beatActivationNN_.getLastDownbeat();       // multi-output support
} else {
    odf = ensembleDetector_.getFusedStrength();  // BandFlux fallback
}
addOdfSample(odf);  // NN bypasses 5-point MA smoothing (already learned)
```

### BeatActivationNN Features
- Auto-detects output channels from model shape (1 = beat only, 2 = beat + downbeat)
- `extractOutput(frame, channel)` for per-channel access
- `hasDownbeatOutput()` runtime query
- Compile-time toggle: `ENABLE_NN_BEAT_ACTIVATION` (requires Arduino_TensorFlowLite library)

### Memory Budget (measured v54)

| Component | RAM | Flash |
|-----------|-----|-------|
| Model weights | 0 (in flash) | ~20 KB |
| Tensor arena | ~8 KB | 0 |
| TFLite runtime | ~4 KB | ~40 KB |
| **Total NN overhead** | **~12 KB** | **~60 KB** |
| **NN build (measured)** | 22 KB / 256 KB | 312 KB / 1 MB |
| **Non-NN build (measured)** | 22 KB / 256 KB | 301 KB / 1 MB |

### TFLite Micro Setup
- Arduino library: `Arduino_TensorFlowLite` 2.4.0-ALPHA (Seeed verified port for nRF52840)
- CMSIS-NN delegate for INT8 acceleration
- Pre-allocated tensor arena (~8 KB), no dynamic allocation
- Fixed: `InitializeTarget()` removed (was clobbering Serial baud rate)

## Phase 6: Validation

Follow [Testing Methodology](TESTING_METHODOLOGY.md):
1. **Tier 1** (smoke): Flash, verify beats fire, BPM plausible
2. **Tier 2** (quick A/B): 3 tracks, NN vs BandFlux
3. **Tier 3** (reliable A/B): 4 tracks × 3 runs
4. **Tier 4** (full validation): 18 tracks × 3 runs, new baseline

**Expected improvement**: BandFlux F1 ~0.28-0.35 → NN activation **0.50-0.70** F1 (based on madmom's improvement over handcrafted ODFs with same-quality tracker).

## Implementation Status

| Step | Description | Status |
|------|-------------|--------|
| 1 | ml-training/ scaffolding (README, requirements, configs) | **Done** |
| 2 | Feature extraction (`prepare_dataset.py`) — firmware mel parity | **Done** (incl. spectral conditioning augmentation) |
| 3 | Model definition (`beat_cnn.py`) — causal 1D CNN | **Done** (beat + downbeat heads) |
| 4 | Training script (`train.py`) — end-to-end loop | **Done** (multi-output, weighted BCE) |
| 5 | Export script (`export_tflite.py`) — Keras → TFLite INT8 → C header | **Done** (preserves downbeat channel) |
| 6 | Data download/labeling scripts | **Done** (download_fma, download_giantsteps, download_rir, label_beats) |
| 7 | Offline evaluation (`evaluate.py`) | **Done** (beat + downbeat F1, activation plots) |
| 8 | Firmware integration — `BeatActivationNN.h`, AudioController | **Done** (v54, compiles, needs trained model) |
| 9 | Cross-validation pipeline | **Not started** (tools researched, needs implementation) |
| 10 | Download training data + label with Beat This! | **Not started** |
| 11 | Train model on real data | **Not started** |
| 12 | Deploy trained model to firmware and A/B test | **Not started** |

### Immediate Next Steps

1. **Install pyenv + Python 3.11 venv** for BeatNet/madmom cross-validation
2. **Build cross-validation script** (`scripts/cross_validate_labels.py`)
3. **Download FMA electronic subset** (~5,000 tracks, 22 GB)
4. **Label with Beat This!** + cross-validate with essentia
5. **Re-run `prepare_dataset.py`** — current processed data lacks downbeat targets and spectral conditioning
6. **Train model** and iterate on architecture/hyperparameters
7. **Deploy to firmware** and run Tier 2-4 validation

## References

- **Beat This!** — Foscarin et al., ISMIR 2024. State-of-the-art beat/downbeat tracking.
- **madmom** — Böck et al., 2016. DBN beat tracker with RNN activation.
- **BeatNet** — Heydari & Cwitkowitz, 2021. Real-time joint beat/downbeat tracking.
- **TFLite Micro on nRF52840** — Seeed Wiki, keyword spotting example on XIAO Sense.
