# Blinky Time - Improvement Plan

*Last Updated: March 13, 2026*

> **Historical content (v28-v64 detailed writeups, parameter sweeps, A/B test data)** archived via git history. See commit history for `docs/IMPROVEMENT_PLAN.md` prior to this date.

## Current Status

**Firmware:** v70 (SETTINGS_VERSION 70). CBSS beat tracking + Bayesian tempo fusion. Frame-level FC NN beat/downbeat activation (always-on, TFLite required). Beat-synchronized downbeat and measure counter. Onset snap hysteresis and PLL warmup relaxation. Dimension-independent generator params (v69). All v65 rhythm params persisted (v70).

**NN Model Status:** Frame-level FC model deployed on all 3 devices (56.8 KB INT8, per-tensor quantization, ~3ms inference). **Mel calibration DONE** — cal63 model trained with corrected target_rms_db=-63 dB. On-device A/B test (6 tracks) shows ~50% stronger ODF activations (mean 0.30 vs 0.20), BPM accuracy improved on 4/6 tracks, and functional downbeat detection (max 0.37-0.57 vs 0.00 on old model). Cal63 deployed on ACM0; ACM1/ACM2 still on old model. W64 model (64-frame window, 109K params, ~106 KB INT8) training in progress.

**Key constraint:** The LED visualizer runs on a single thread at 60 Hz. Total frame budget is 16.7ms. Audio processing + FFT + detection + CBSS + generator + LED output consume ~6-7ms, leaving **~10ms for any NN inference**. Mel-spectrogram CNNs can't fit this budget, but FC layers can (~60-200µs).

**Mel-spectrogram model history (CLOSED — all too slow):**

| Model | Architecture | Size | Offline F1 | Inference | Status |
|-------|-------------|------|:----------:|:---------:|--------|
| v4 | 5L ch32 standard | 33.3 KB | 0.717 | 79ms | Too slow (12 Hz) |
| v6-restart | 5L ch32 standard | 33.3 KB | 0.727 | 79ms | Too slow (12 Hz) |
| v7-melfixed | 7L ch32 standard | 46.3 KB | **0.787** | >79ms | Best F1, too slow |
| v8 | 7L ch48 standard | ~68 KB | 0.821 | N/A | Heap exhaustion |
| v9 DS-TCN 24ch | 5L DS-TCN | 26.5 KB | TBD | **98ms** | Too slow (10 Hz) |

The v9 DS-TCN was designed to be faster via depthwise separable convolutions (2.7× fewer MACs), but 8 INT8 ADD ops from residual connections cost ~5ms each (36ms total from requantization overhead), making it slower than the standard conv models.

## Active Priorities

### Priority 1: Frame-Level NN Model Improvement

**Status: DEPLOYED, CALIBRATION NEEDED (March 12, 2026)**

**Problem:** The NN model is deployed and producing non-zero output on device, but beat activations are weak (max ~0.26 at 120 BPM). Root cause: firmware mel values (mean ~0.52) are lower than training data (mean ~0.86) due to AGC level mismatch. The previous deterministic pipeline (BandFlux + CBSS) achieved only ~28% beat F1. Core failures:
- **Octave errors**: ACF/comb filters have strong sub-harmonic peaks → half/double-time lock (135 BPM gravity well). Hand-tuned octave checks help but are brittle.
- **Phase drift**: CBSS derives phase from a counter. Slight BPM error accumulates phase offset. PLL correction is conservative to avoid jitter.
- **False beats during breakdowns**: CBSS forces beats when no onset is detected, maintaining phase through silence but triggering false visuals.
- **Slow tempo adaptation**: Bayesian fusion with conservative transitions takes 4-8 seconds to lock a new tempo after a transition.
- **Downbeat detection**: F1 ~0.33 offline. No reliable spectral downbeat detection.

Mel-spectrogram CNN models that could improve ODF quality require 79-98ms inference — 8-10× over budget. However, **FC layers on the same mel input are 100-1000× cheaper** than convolutions.

**Previous approach (beat-synchronous hybrid, ABANDONED March 11):** A beat-rate FC classifier on accumulated spectral summaries. Abandoned because:
1. **Circular dependency**: If CBSS is unreliable (~28% F1), features extracted at beat boundaries are noisy. The NN can't reliably correct the tracker that produced its inputs.
2. **No discriminative signal**: Label quality analysis showed Cohen's d < 0.13 between downbeat and non-downbeat beat-level features. A linear classifier gains 0% over majority-class baseline. Per-beat spectral summaries don't carry downbeat information.
3. **Outlier approach**: ALL leading beat/downbeat trackers (BeatNet, Beat This!, madmom, TCN) use frame-level NNs. Only Krebs 2016 used beat-level features — with bidirectional GRU (impossible in real-time), two feature streams, 4 subdivisions, and DBN post-processing. Even then, F1 drops from 90.4% to 77.3% with estimated beats, showing high sensitivity to beat tracker errors.

**New approach: Frame-level FC on raw mel frames.** Process a sliding window of raw mel frames through FC layers to produce per-frame beat and downbeat activations. This matches the proven paradigm used by all successful systems, avoids the circular dependency, and is computationally feasible because FC inference is fast (~60-200µs vs 79-98ms for convolutions).

**Architecture: Frame-Level FC Beat Activation**

```
SharedSpectralAnalysis (already runs every frame, no additional cost)
     │
     ├── rawMelBands_ (26 bands, 62.5 Hz, pre-compression, pre-whitening)
     │         │
     │    Mel frame ring buffer (last N frames, ~N×26 floats)
     │         │
     │    FrameBeatNN inference (every Kth frame, ~15.6 Hz):
     │         │  Input:  N frames × 26 mel bands = N×26 floats (flattened)
     │         │  Model:  FC hidden layers → 2 outputs
     │         │  Output: [beat_activation, downbeat_activation]
     │         │
     │         ├── beat_activation → replaces BandFlux as ODF for CBSS
     │         │     Higher quality ODF → better tempo estimation → better phase
     │         │
     │         └── downbeat_activation → AudioControl.downbeat
     │               Smoothed and thresholded for bar boundary detection
     │
     ├── NN beat activation → CBSS → beat detection (replaces BandFlux ODF)
     │
     └── AudioControl (all fields, with NN-driven ODF + downbeat)
```

**Why frame-level FC works on Cortex-M4F:**

The mel-spectrogram CNN was slow (79-98ms) because of Conv2D operations — CMSIS-NN still requires 37ms for convolutions, plus overhead from Pad, SpaceToBatch, residual Add requantization. FC layers have none of this overhead:

| Approach | Input size | Rate | Inference | CPU |
|----------|-----------|------|-----------|-----|
| Mel CNN (conv, CLOSED) | 128×26 = 3,328 | 62.5 Hz | 79-98ms | >100% (impossible) |
| Beat-sync FC (ABANDONED) | 4×79 = 316 | ~2 Hz | 83µs | <0.1% |
| **Frame-level FC (every 4th frame)** | **N×26** | **15.6 Hz** | **~60-200µs** | **~0.1-0.3%** |
| Frame-level FC (every frame) | N×26 | 62.5 Hz | ~60-200µs | ~0.4-1.2% |

All frame-level FC options are well within the 10ms per-frame budget.

**Key design decisions:**

1. **Raw mel bands as stable interface.** Same principle as beat-sync approach — uses `rawMelBands_` (pre-compression, pre-whitening), decoupled from 47+ tunable firmware parameters. Only depends on 8 fundamental constants (sample rate, FFT size, hop, mel bands, mel range, mel scale, log compression, window) that never change.

2. **NN replaces BandFlux as ODF source.** The beat activation output feeds directly into CBSS as a higher-quality ODF signal. BandFlux is obsolete and being removed from the codebase.

3. **No circular dependency.** Feature extraction (raw mel frames) is independent of CBSS. The NN produces the ODF that CBSS consumes — a clean feedforward pipeline, not a feedback loop.

4. **CBSS remains for tempo/phase tracking.** The NN provides a better activation signal, but CBSS still handles tempo estimation (ACF + Bayesian fusion) and phase tracking (counter-based beats). This is how all leading systems work: NN activation → post-processing for tempo/phase.

5. **Downbeat comes free.** Second output head trained on frame-level downbeat labels. All leading systems (madmom, BeatNet, Beat This!) produce both beat and downbeat activations from the same model.

**Context window sizing:**

At 120 BPM, one beat = 0.5s = ~31 frames at 62.5 Hz. The context window should capture at least one full beat interval. Options to explore:

| Window | Frames | Input dim | Coverage at 120 BPM |
|--------|--------|-----------|---------------------|
| 0.5s | 32 | 832 | ~1 beat |
| 0.75s | 48 | 1,248 | ~1.5 beats |
| 1.0s | 64 | 1,664 | ~2 beats |

Larger windows give more context for downbeat detection but increase model size. Start with 32 frames (0.5s) and expand if needed.

**Model architecture (initial):**

```
Input: 32 × 26 = 832 floats (flattened)
  → FC 832 → 64 (ReLU)
  → FC 64 → 32 (ReLU)
  → FC 32 → 2 (Sigmoid: beat_activation, downbeat_activation)
Output: [beat_prob, downbeat_prob] per frame
```

~56K params, ~56 KB INT8. Fits in flash budget (1 MB total, ~260 KB base firmware). Tensor arena ~8-16 KB. If too large, reduce hidden layers or window size.

**Projected resource usage:**

| Resource | BandFlux (obsolete) | Frame-level FC NN (current) | Notes |
|----------|-------------------|-------------------|-------|
| ODF quality | ~28% F1 | Target >50% F1 | Learned vs hand-tuned |
| Inference time | <0.1ms @ 62.5 Hz | ~3ms @ 62.5 Hz | Both well within budget |
| Tensor arena | 0 | ~2 KB | Minimal |
| Mel frame buffer | 0 | ~3.3 KB (32×26×4 bytes) | Ring buffer |
| Model flash | 0 | 56.8 KB INT8 | Per-tensor quantization |
| Downbeat | No | Yes | New capability |

**Training data and labels:**

Frame-level labels already exist from the mel-CNN work:
- Raw audio files (`/mnt/storage/blinky-ml-data/audio/`, ~7000 tracks)
- 4-system consensus beat/downbeat labels (frame-level, `/mnt/storage/blinky-ml-data/labels/consensus_v2/`)
- Mel extraction pipeline (`scripts/audio.py`) — already firmware-matched
- Audio augmentation (gain, noise, RIR, time-stretch) from `prepare_dataset.py`
- Mic calibration profiles (gain-aware augmentation)

The training pipeline from `prepare_dataset.py` → `train.py` produces frame-level mel spectrograms and frame-level beat/downbeat targets. The main change is replacing the CNN model with an FC model that operates on a sliding window of mel frames.

**Firmware changes:**

1. **Mel frame ring buffer (~50 lines)** — Simple circular buffer of raw mel frames. SharedSpectralAnalysis already computes rawMelBands_ every frame; just store the last N frames. Replaces SpectralAccumulator (which accumulated between beats).

2. **FrameBeatNN (~150 lines, replaces BeatSyncNN)** — TFLite Micro FC inference. Input: flattened mel frame window. Output: beat_activation + downbeat_activation. Runs every Kth frame (K=4 for 15.6 Hz). Much simpler than BeatActivationNN (no sliding mel buffer management, no multi-channel output).

3. **AudioController.cpp (~30 lines changed)** — Replace BandFlux ODF with NN beat activation. NN downbeat output feeds `control_.downbeat`. Fallback to BandFlux when NN not compiled.

**Phased implementation:**

- ~~**Phase A (beat activation only):**~~ DONE — FC model deployed, beat+downbeat activation working on all 3 devices.
- ~~**Phase B (mel calibration):**~~ DONE — calibrated `target_rms_db` from -35 to -63 dB (mel mean 0.52, matching firmware AGC). Cal63 model trained on corrected data. On-device A/B (6 tracks): mean ODF 0.30 vs 0.20 (+50%), BPM accuracy improved 4/6, downbeat activations now functional (max 0.37-0.57 vs 0.00).
- **Phase C (model architecture iteration):** W64 model training in progress (64-frame window, 109K params, [64,32] hidden, ~106 KB INT8). Conv1D wide evaluated (beat F1=0.500, DB F1=0.217). Configs for 16/32/48/64 frames ready.
- ~~**Phase D (BandFlux removal):**~~ DONE (v67) — Removed EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult. 10 files deleted, ~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved. SETTINGS_VERSION 66→67.

**Research context:**
- ALL leading beat trackers use frame-level NNs: BeatNet (CRNN), Beat This! (CNN+Transformer), madmom (BiLSTM), TCN beat tracker
- Our innovation: using FC instead of CNN/RNN to fit Cortex-M4F compute budget, while following the same frame-level activation → post-processing paradigm
- No published TinyML beat tracking on Cortex-M class hardware exists (as of March 2026)

### Priority 2 (new): ESP32-S3 Mic Calibration and Model

**Status: PLANNED**

The XIAO ESP32-S3 Sense uses an MSM381ACT PDM microphone via ESP-IDF I2S PDM-RX. Its frequency response, noise floor, and AGC transfer function differ from the nRF52840's built-in microphone. The nRF52840 cal63 model was trained on mel spectrograms captured at `target_rms_db=-63 dB` through the nRF52840 mic chain. Running that model on ESP32-S3 audio will produce mismatched mel statistics and degraded ODF quality.

**Required steps:**

1. **Measure ESP32-S3 mic profile** — Record `target_rms_db` on the ESP32-S3 the same way cal63 was measured for nRF52840: play calibration tones, capture raw mel values via `stream` command, find the RMS dB level where firmware mel mean ≈ training mean (~0.52 normalized).

2. **Capture training data** — The ESP32-S3 mic has a different frequency response. Ideally record a subset of the training corpus through the actual ESP32-S3 mic (or model the transfer function via impulse response measurement and apply it to existing recordings).

3. **Train ESP32-S3 model** — Retrain the FC beat/downbeat model with ESP32-S3 mel statistics. Use the same architecture (32-frame window, FC [64,32] → 2 outputs) but with ESP32-calibrated data. Produce a separate `.tflite` / `.h` model artifact for the ESP32-S3 build target.

4. **Firmware model selection** — Use `#ifdef BLINKY_PLATFORM_ESP32S3` to select the ESP32-S3 model at compile time, keeping the nRF52840 model unchanged.

**Hardware gain note:** The ESP32-S3 I2S PDM-RX slot config has **no hardware gain register** — `amplify_num`, `sd_scale`, `hp_scale` are all absent from `i2s_pdm_rx_slot_config_t` on this SoC (those fields exist only on other ESP32 variants). The full AGC range is applied as software gain in `Esp32PdmMic::setGain()` / `poll()`. The mic profile calibration must account for this — the software gain path has different noise characteristics than the nRF52840 hardware AGC.

### ~~Priority 2 (old): BandFlux Removal~~ — COMPLETED (v67)

**Status: COMPLETED — March 12, 2026**

Removed all BandFlux/EnsembleDetector code. SharedSpectralAnalysis promoted to direct AudioController ownership. Pulse detection inlined from EnsembleFusion (ODF threshold + tempo-adaptive cooldown). Non-NN fallback: `mic_.getLevel()` as simple energy ODF. See git log for details.

### ~~Priority 3: CBSS ODF Contrast~~ — COMPLETED (v66)

**Status: COMPLETED — cbssContrast=2.0 is now the default**

A/B tested cbssContrast=1.0 vs 2.0 (BTrack-style ODF squaring). Results: 10 wins, 6 losses, 2 ties across 3 devices × 18 tracks. Mean BPM error 12.4 vs 12.6. Octave errors unchanged (9 vs 9). Default updated to 2.0 in v66.

### Future: Heydari 1D State Space

**Status: RESEARCH ONLY**

Heydari et al. (ICASSP 2022) — 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. ~860 states fits our memory budget. Could replace CBSS if feature-level NN improvements are insufficient.

## Current Bottlenecks

1. ~~**Mel level mismatch (RESOLVED March 13)**~~ — Fixed by retraining with `target_rms_db=-63` (cal63 model). On-device ODF activations now ~50% stronger (mean 0.30 vs 0.20). Deployed on ACM0.

2. ~~**CBSS parameter re-tuning (RESOLVED March 13)**~~ — Swept `cbssthresh` (0.5-2.0) and `cbsscontrast` (1.0-3.0) across 18 tracks on all 3 devices with cal63 ODF. Neither showed significant improvement over current defaults. Ratio-based params (cbssTightness, onsetSnapWindow, adaptiveTightness) confirmed self-compensating. No changes needed.

3. **Downbeat detection quality (F1 ~0.24 offline)** — Consensus v3 labels now use AND-merge (require 2+ system agreement), eliminating 65% of noisy single-system downbeat labels. Cal63 trained on v3. On-device downbeat activations are now functional (max 0.37-0.57), but offline F1 remains limited by label quality ceiling.

4. **~135 BPM gravity well** — Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks. Not improved by CBSS parameter tuning (March 13 sweep). Not a tempo bin resolution issue (47 bins tested v61; ACF already evaluates at full lag resolution). Likely requires better NN ODF discrimination or Rayleigh prior adjustment.

5. **Phase alignment** — CBSS derives phase indirectly from a counter. Sharper NN beat activations give CBSS better signal for phase tracking.

6. ~~**NN inference speed (RESOLVED)**~~ — Frame-level FC runs ~3ms, well within 10ms budget.

7. ~~**Per-channel quantization (RESOLVED March 12)**~~ — Fixed: `_experimental_disable_per_channel=True` in export.

## SOTA Context (March 2026)

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~28%** | NN ODF + CBSS (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends (CNN, CRNN, Transformer) that require 79ms+ on our hardware. The frame-level FC approach follows the same paradigm (frame-level NN activation → post-processing) but uses FC layers instead of convolutions, achieving ~3ms inference. The NN is now the sole ODF source (BandFlux is obsolete and being removed), providing a learned beat activation that feeds into CBSS for tempo/phase tracking. Using raw mel bands as the stable interface decouples the NN from firmware signal processing parameters.

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Multi-factorial (data bias, prior, comb harmonics) | **Medium** | Not CBSS params or bin count. Rayleigh prior or NN ODF quality. |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | P1 NN ODF (sharper beat activations improve CBSS phase) |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** | Acceptable for visuals |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** | Organic mode is correct |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** | Energy-reactive acceptable |

## Closed Investigations (v28-v65)

All items below were A/B tested and showed zero or negative benefit, or proven infeasible. Removed from firmware in v64 unless noted.

- **Mel-spectrogram NN models (v4-v9)**: All architectures (standard conv, BN-fused, DS-TCN) exceed 79ms inference on Cortex-M4F @ 64 MHz. The v9 DS-TCN (designed for speed) measured 98ms due to INT8 ADD requantization overhead from residual connections. No mel-spectrogram CNN architecture can fit the 10ms per-frame budget. Superseded by frame-level FC approach (~60-200µs). NN always compiled in since v68 (ENABLE_NN_BEAT_ACTIVATION ifdef removed).

- **Beat-synchronous hybrid corrector (March 10-11, 2026)**: FC model on accumulated spectral summaries at beat rate (~2 Hz). Phase A (downbeat-only) achieved val_F1=0.548 but label analysis revealed fundamental problems: Cohen's d < 0.13 between downbeat/non-downbeat features, linear classifier gains 0% over baseline, only 51.6% of tracks have clean period-4 downbeats. Circular dependency: unreliable CBSS (~28% F1) produces noisy beat boundaries → noisy features → unreliable correction. ALL leading algorithms use frame-level NNs; only Krebs 2016 used beat-level (with bidirectional GRU, impossible in real-time). Pivot to frame-level FC avoids circular dependency and matches proven paradigm. Code retained: SpectralAccumulator.h, BeatSyncNN.h, models/beat_sync.py, scripts/beat_feature_extractor.py, scripts/export_beat_sync.py — but not actively used.

- **Full Python signal chain simulator (March 2026)**: Originally proposed reimplementing the complete BandFlux → OSS → ACF → comb filter → Bayesian fusion → CBSS pipeline in Python (~2000 lines). No longer needed — frame-level FC approach uses frame-level labels directly, no CBSS simulation required.

- **Forward filter** (v57-v60): Full 6-param sweep, 7/18 octave errors vs CBSS 4/18. Half-time bias fundamental.
- **Spectral noise subtraction** (v56): Baseline wins 13/18. Code retained, default OFF.
- **Template+subbeat** (v50): No net benefit (baseline 10 wins, subbeat 8).
- **Tempo bins 20→47** (v61): No improvement. Gravity well not a bin count issue.
- **Focal loss** (v5): Identical to v4.
- **HMM phase tracker** (v37, v46): Bernoulli obs model fails on mic audio. CBSS retained.
- **PLP phase extraction** (v42): OSS too noisy. Redundant with onset snap.
- **Signal chain decompression** (v47): BandFlux self-normalizes. Not the F1 bottleneck.
- **Particle filter** (v38-39): Improved BPM but not F1. Phase is the bottleneck.
- **Adaptive tightness, Percival harmonic, bidirectional snap** (v44-45): Marginal or no benefit.

## Visualizer Improvements

### Fire Generator Enhancement

**Status: PARTIALLY COMPLETE — audio reactivity done, visual depth improvements remain**

The fire generator (`Fire.h/cpp`) is a particle system with 3 spark types (FAST_SPARK, SLOW_EMBER, BURST_SPARK), thermal buoyancy, simplex noise wind via `ForceAdapter`, and audio-reactive spawn/velocity modulation. It inherits from `ParticleGenerator<64>` and renders to `PixelMatrix` via additive blending. All physics parameters are stored as dimension-normalized fractions and scaled at use-time via `scaledVelMin()`, `scaledSpread()`, etc. — no per-device tuning needed.

**Code review findings (March 15, 2026):**
- All 7 AudioControl signals are already consumed (phase, pulse, downbeat, energy, rhythmStrength, onsetDensity, beatInMeasure). The original plan items 1-4 are implemented.
- `FireDefaults` struct in `DeviceConfig.h` (baseCooling, sparkHeatMin, etc.) is **vestigial** — leftover from the old heat-diffusion model. `Fire::begin()` never reads `config.fireDefaults`. Should be removed in a future cleanup.
- Background noise (`MatrixBackground`) iterates every pixel with 2 simplex noise evals per pixel. At 32×32 = 1024 pixels this is ~2048 noise calls/frame — still under 1ms at 240 MHz (ESP32-S3) but worth monitoring.

**Scaling constraint: particle pool size.** `ParticleGenerator<64>` hard-caps at 64 particles (`uint8_t` template, max 255). `scaledMaxParticles()` caps at `min(64, 0.75 * numLeds)`. On the 32×32 display (1024 LEDs), 64 particles cover 6% of pixels. The background noise layer fills the remaining 94%. This is the primary visual bottleneck for larger matrices — the fire looks sparse because the particle layer can't fill the frame.

#### Completed (items from original plan)

All originally planned audio coupling is implemented in `Fire.cpp`:

- **Phase-driven thermal buoyancy breathing** — `updateParticle()` lines 300-331: `phaseMod = 0.5 + 0.5 * phasePulse`, applied to `scaledThermalForce()`. Also phase-driven drag (1.5% extra off-beat).
- **Downbeat dramatic effects** — `spawnParticles()` lines 138-143: `downbeatSpreadMult_` (2.5× → 1.0 over 0.5s), `downbeatColorShift_` (white/blue tint, 0.5s decay), `downbeatVelBoost_` (1.5× velocity, 0.3s decay). `particleColor()` applies the tint.
- **Onset density → particle character** — `spawnTypedParticle()` line 237: `densityLifeMult = 1.5 - 0.75 * densityNorm`. Also drives noise speed in `generate()` lines 64-66 and wind turbulence line 85.
- **Beat-in-measure accent patterns** — `spawnParticles()` lines 146-159: beat 3 at 50%, beats 2/4 at 25%, odd/even left/right rocking bias via `spawnBias_`.

#### Remaining Improvements

Ranked by visual impact. All items use only `width_`, `height_`, `numLeds_`, `traversalDim_`, `crossDim_` for scaling — no device-specific constants.

##### Tier 1: High Impact, Low Effort

**1. Increase particle pool capacity**
`ParticleGenerator<64>` is the bottleneck. On a 32×32 matrix, 64 particles at 6% coverage looks sparse. Change `Fire : ParticleGenerator<64>` to `ParticleGenerator<N>` where N scales with display size.
- Template parameter is `uint8_t` (max 255). For 1024 LEDs at 0.75 coverage, 255 is the practical max.
- RAM cost: 28 bytes/particle. 64→192 adds 3.6 KB. ESP32-S3 has 512 KB SRAM (vs nRF52840 256 KB), ample headroom. nRF52840 devices (max 128 LEDs) would still cap at 64-96 via `scaledMaxParticles()`.
- Options: (a) conditional compile `ParticleGenerator<192>` on ESP32-S3, (b) raise to 192 globally (nRF52840 can absorb 5.4 KB), or (c) make it a runtime-resizable pool (more complex).
- `scaledMaxParticles()` already handles the scaling: `min(POOL_CAP, 0.75 * numLeds)`. Just raise the pool cap.

**2. Self-modulating noise background (Stefan Petrick technique)**
Current `MatrixBackground` uses 2-octave simplex noise with threshold and height falloff. Replace with domain-warped noise: use output of one noise field to distort coordinates of a second.
```
n1 = SimplexNoise::noise3D(x * 0.1, y * 0.1, t * 0.02)
n2 = SimplexNoise::noise3D(x * 0.2 + n1 * 2.0, y * 0.2 + n1 * 1.5, t * 0.03)
```
Dramatically more organic ember bed with swirling, lava-lamp-like movement. On 32×32: 2048 noise calls (same as current 2-octave), negligible CPU increase. On 16×8: 256 calls. Scales naturally with `width_` × `height_` loop in `MatrixBackground::render()`.
- Modify: `MatrixBackground::sampleNoise()` (replace second octave with domain warp)
- Audio coupling already in place: `beatBrightness` and `noiseTime` modulation from `Fire::generate()`

**3. Multi-palette blending**
Current `particleColor()` uses a single hardcoded 6-stop palette. Define 2-3 palettes and interpolate between them by audio state.
- **Warm** (default): black → deep red → red → orange → yellow-orange → bright yellow (current)
- **Hot** (high energy + rhythm): black → red → orange → white → pale blue (intense)
- **Cool** (low energy/ambient): black → dark red → deep orange (embers only)
- Blend factor: `energy * rhythmStrength` (both 0-1, already available in `audio_`)
- Interpolation: per-stop RGB lerp between palette A and B. No HSV conversion needed — adjacent fire colors are already hue-monotonic.
- Transition smoothing: low-pass the blend factor (~0.5s time constant) to avoid jarring shifts. Same pattern as `downbeatColorShift_` decay.
- Modify: `Fire::particleColor()`, add `paletteBias_` member with exponential smoothing in `spawnParticles()`

**4. Intensity-to-palette gamma curve**
Apply nonlinear gamma before palette lookup in `particleColor()`: `index = pow(intensity/255.0, gamma) * 255`.
- Audio-driven gamma: high energy → gamma < 1.0 (more visible ember glow, brighter midtones). Low energy → gamma > 1.0 (only brightest sparks visible, dark base).
- Reduces banding on larger matrices (32×32 has 1024 pixels to fill smooth gradients).
- 1 `powf()` call per particle per frame. Trivial cost.
- Modify: `Fire::particleColor()` (add gamma remap before palette lookup)

##### Tier 2: High Impact, Medium Effort

**5. Curl noise wind field**
Current wind: `ForceAdapter` applies simplex noise per-particle for turbulence. Replace with curl noise (Bridson, SIGGRAPH 2007) — take the curl of a scalar noise field to get a divergence-free velocity field.
```
curl_x = (noise(x, y+eps, t) - noise(x, y-eps, t)) / (2*eps)
curl_y = -(noise(x+eps, y, t) - noise(x-eps, y, t)) / (2*eps)
```
Particles swirl around each other instead of being pushed independently. 4 noise evals per particle per frame. At 192 particles: 768 calls. At 64: 256 calls. Well under 1ms on either platform.
- Modify: `MatrixForceAdapter::applyWind()` or add `CurlNoiseForceAdapter`. Audio coupling: `noiseTime_` already modulates time, wind variation already scales by `densityNorm` and `phasePulse`.
- Biggest visual win on 32×32 where particle paths are long enough to see the swirling motion.

**6. Energy → dynamic flame height**
`energy` drives spawn rate and noise speed but not flame extent. Map it to the kill boundary:
- Low energy: particles killed at `0.4 * height_` (short flame, mostly embers)
- High energy: particles reach full `height_` (tall roaring fire)
- Formula: `killY = height_ * (0.4 + 0.6 * energy)` (already 0-1 normalized)
- Also modulate `backgroundIntensity`: `0.05 + 0.2 * energy` (brighter ember bed when loud)
- Requires: custom `BoundaryBehavior` that checks Y against dynamic threshold, or modify `KillBoundary` to accept a dynamic limit. `KillBoundary` currently checks `y < 0 || y >= height_` — change to `y < killY` for fire.

**7. Spawn-on-death cascading embers**
When a high-intensity spark dies (boundary kill or max age), spawn 1-2 dim child embers with reduced velocity and longer lifespan. "Shower of sparks" effect.
- Child sparks: intensity 30-60 (deep red range), lifespan 1.5× parent, velocity 0.3× parent + random spread, type SLOW_EMBER.
- **Pool exhaustion guard**: max 2 child spawns per frame, only when `pool_.getActiveCount() < scaledMaxParticles() * 0.8`. Prevents cascade from starving beat-triggered spawns.
- Modify: `ParticlePool::updateAll()` callback in `ParticleGenerator::updateParticles()` — when particle dies, call `Fire::onParticleDeath()`. Or add a `deathCallback` to the pool template.
- More impactful with larger pool (#1 above) — 64 particles is too tight for cascading.

##### Tier 3: Medium Impact, Creative

**8. Ember pulsing**
Slow embers don't fade linearly — they pulse. Per-particle sinusoidal modulation in `renderParticle()`:
`renderIntensity = intensity * (0.7 + 0.3 * sin(age * freq + phase_offset))`
Requires adding a `phase_offset` field to `Particle` (or derive from spawn position). Creates "breathing coals" effect. Zero spawn/kill cost — purely cosmetic per-frame modulation.
- Can use existing `p->x + p->y` as phase seed (deterministic, no extra storage).

**9. Reaction-diffusion flame base (FitzHugh-Nagumo)**
Run a 1D reaction-diffusion system along the bottom row to modulate spawn intensity. Creates naturally-forming "flame tongues" that split, merge, and oscillate.
- Buffer: `float activator_[width_]` + `float inhibitor_[width_]` — 256 bytes at width=32, 128 bytes at width=16.
- Cost: ~width multiply-adds per frame. At 32-wide: negligible.
- Audio coupling: `energy` → reaction rate (faster = more tongue formation), `pulse` → inject activation spike at random position.
- Modify: `Fire::spawnParticles()` — multiply `spawnProb` by `activator_[x]` for each spawn position.

**10. Vortex filaments at flame base**
2 counter-rotating virtual vortices that alternate activation. Particles get rotational velocity kicks, creating S-curve flame shapes.
- Per vortex per particle: 1 sqrt + 1 division (distance and angle). At 192 particles × 2 vortices: 384 sqrt calls. ~0.1ms.
- Downbeat trigger: inject temporary vortex for 1s (mushroom-cloud bloom). Dramatic on 32×32 where the bloom has room to develop.
- Modify: `Fire::updateParticle()` — add vortex velocity contribution when `downbeatVelBoost_ > 0`.

#### Audio Signal → Fire Parameter Mapping (Current State)

| Signal | Current Implementation | Remaining Opportunities |
|--------|----------------------|------------------------|
| `phase` | Thermal buoyancy breathing (0.5-1.0×), spawn rate pump, velocity boost, drag modulation, wind breathing, background brightness | — (fully utilized) |
| `pulse` | Burst spark count, velocity boost, wind gust magnitude | — (fully utilized) |
| `downbeat` | Extra sparks, spread expansion (2.5×), color temp shift, velocity burst (1.5×) | Vortex injection (#10) |
| `energy` | Spawn rate, noise speed, wind turbulence | Flame height (#6), background brightness (#6), palette blend (#3), gamma (#4) |
| `rhythmStrength` | Organic/music mode blend (spawn, velocity, wind, type selection) | — (fully utilized) |
| `onsetDensity` | Lifespan modulation, noise speed, wind turbulence amplitude | Reaction-diffusion rate (#9) |
| `beatInMeasure` | Beat 1/3/2/4 accent patterns, left/right spawn rocking bias | — (fully utilized) |

#### Cleanup: Remove Vestigial `FireDefaults`

`FireDefaults` in `DeviceConfig.h` (lines 56-64) contains 7 fields from the old heat-diffusion fire model: `baseCooling`, `sparkHeatMin`, `sparkHeatMax`, `sparkChance`, `audioSparkBoost`, `coolingAudioBias`, `bottomRowsForSparks`. The current particle-based `Fire` class never reads these — `Fire::begin()` only calls `ParticleGenerator::begin(config)` which reads `MatrixConfig` fields. `FireDefaults` is set in all 4 device config headers and persisted via `DeviceConfigLoader`, but serves no purpose.

Remove: `FireDefaults` struct, `DeviceConfig.fireDefaults` field, all device config `.fireDefaults = { ... }` blocks, `DeviceConfigLoader` fire fields. ~40 lines across 7 files.

#### References

- [Bridson SIGGRAPH 2007: Curl-Noise for Procedural Fluid Flow](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf)
- [Stefan Petrick: Self-Modulating Noise Fire Effect](https://gist.github.com/StefanPetrick/819e873492f344ebebac5bcd2fdd8aa8)
- [Inigo Quilez: Domain Warping](https://iquilezles.org/articles/warp/) — Noise domain warping for organic patterns
- [FastLED Fire2012 (Mark Kriegsman)](https://github.com/FastLED/FastLED/blob/master/examples/Fire2012/Fire2012.ino)
- [Fabien Sanglard: How DOOM Fire Was Done](https://fabiensanglard.net/doom_fire_psx/)
- [Andrew Chan: Simulating Fluids, Fire, and Smoke in Real-Time](https://andrewkchan.dev/posts/fire.html)

### Lightning → Plasma Globe Redesign

**Status: PLANNED**

The current Lightning generator produces stationary Bresenham line bolts that flash on and fade out in ~0.3s. This creates a strobe-like effect that is visually harsh at low resolutions. The goal is to replace it with a **plasma globe** aesthetic: persistent, flowing tendrils of light that sweep slowly and organically, always-on and never flashing.

**Why the current Lightning doesn't work:**
- **Zero motion**: Particles have `vx=vy=0`, bolts appear and die in place
- **Fast fade**: 30 intensity/frame = gone in ~8 frames (~130ms)
- **Discrete events**: beat → spawn bolt → fade → nothing. No continuity between events
- **MAX blending**: Creates harsh, clipped brightness peaks
- A plasma globe is the opposite: continuous, flowing, always-on

**Architecture: Extend Generator directly, NOT ParticleGenerator.** Plasma is a continuous field, not discrete particles. No particle pool, no spawn/kill lifecycle.

#### Layer 1: Plasma Background (Demoscene Sine-Wave Plasma)

Sum 3-4 sine waves at different frequencies/phases across the LED field. Map summed value through a purple/violet palette. Very cheap (~1 multiply + lookup per pixel), always-on dim glow.
- Audio: `energy` modulates brightness, `phase` shifts sine offsets for breathing

#### Layer 2: Noise-Field Tendrils (Simplex-Noise-Steered Paths)

3-6 persistent tendrils, each a path from center outward:
- At each step, sample `noise3D(x, y, time)` to determine direction bias
- Tendrils sweep slowly, guided by noise field evolution
- Brightness gradient: white core → lavender → deep violet → black
- ~60-120 `noise3D` calls per frame, well under 1ms on Cortex-M4F

#### Tier 1: Core Plasma (Biggest Visual Impact)

**1. Sine-wave plasma background with violet palette**
Sum 3-4 sine waves with different spatial frequencies and time-varying phases to produce a slowly-shifting organic glow across all pixels. Map through a 4-stop palette: black → deep violet → purple → magenta. ~1 multiply + 1 lookup per pixel.

**2. Noise-steered tendrils from center (3-4 initially)**
Each tendril: start at center, step outward. At each step, sample `noise3D(x, y, t)` to bias direction. Tendrils persist across frames (state: just angle + length), creating smooth sweeping motion. Core brightness white, fading to violet at tips.

**3. Core-to-edge brightness gradient**
Radial falloff from center outward. Center is always bright (white/lavender), edges dim (deep violet). Combined with tendril rendering, this creates the characteristic plasma globe depth.

#### Tier 2: Audio Reactivity (All Smooth, Never Flash)

**4. Phase-locked tendril breathing**
Tendril brightness and length pulse with `phase`. On-beat (phase=0): tendrils extend to full length, peak brightness. Off-beat: retract slightly, dim. Creates a rhythmic "pumping" without any flash.

**5. Energy → overall brightness modulation**
`energy` scales background plasma brightness and tendril intensity together. Low energy: dim ambient glow. High energy: vivid, saturated plasma. Always smooth — energy is already a slow-moving signal.

**6. Pulse → tendril extension**
On transient (`pulse`): tendrils momentarily reach further outward, with smooth ease-out return over ~0.3s. Creates "reaching" effect on kicks/snares without strobing.

#### Tier 3: Polish

**7. Mutual tendril repulsion**
Tendrils that are too close angularly repel each other, maintaining visual spread. Simple pairwise angular distance check, add small angular velocity away from neighbors. Prevents clustering on one side.

**8. Downbeat color warmth shift**
On bar 1 (`downbeat > 0.5`): core shifts slightly warm (white → pink/magenta tint), smooth decay back to white over 0.5s. Subtle but marks musical structure.

**9. Onset density → tendril count adaptation**
`onsetDensity` modulates active tendril count (3-6). Sparse ambient music → 3 lazy tendrils. Dense dance music → 5-6 active tendrils. Gradual transitions over 2-3 seconds, never instant add/remove.

#### Audio Signal → Plasma Parameter Mapping

| Signal | Plasma Parameter | Behavior |
|--------|-----------------|----------|
| `energy` | Background brightness + tendril intensity | Higher energy = brighter overall glow |
| `phase` | Noise time offset + sine phase shift | Breathing sync — tendrils pulse in phase with beat |
| `pulse` | Tendril length extension | Transient → tendrils reach further momentarily |
| `rhythmStrength` | Blend organic↔music mode | Low: slow random drift. High: phase-locked breathing |
| `onsetDensity` | Tendril count (3-6) | Sparse music → fewer tendrils. Dense → more |
| `downbeat` | Color warmth shift | Bar 1 → white core shifts slightly warm/pink |
| `beatInMeasure` | Tendril rotation bias | Different beats favor different angular sectors |

**Key principle: Nothing flashes or strobes.** Audio modulates continuous parameters (brightness, speed, spread, length) — never triggers discrete spawn/kill events.

#### Resource Budget

| Resource | Current Lightning | Plasma Globe | Notes |
|----------|------------------|-------------|-------|
| RAM | ~2 KB (40 particles) | ~600 bytes (tendril state + noise scratch) | 70% reduction |
| CPU | <1ms | ~1-1.5ms (noise3D + sine lookups) | Comparable |
| Flash | Minimal | Minimal | No model/tables needed |

#### Color Palette

4-stop gradient for tendril/background rendering:
- Stop 0: Black (0, 0, 0) — beyond tendril reach
- Stop 1: Deep violet (40, 0, 80) — distant plasma
- Stop 2: Lavender (140, 80, 200) — mid tendril
- Stop 3: White (255, 240, 255) — tendril core / center

#### References

- [Stefan Petrick: Noise-Field Fire/Plasma](https://gist.github.com/StefanPetrick/819e873492f344ebebac5bcd2fdd8aa8) — Self-modulating noise for organic flow
- [Demoscene Plasma Tutorial (Lode Vandevenne)](https://lodev.org/cgtutor/plasma.html) — Classic sine-sum plasma technique
- [Simplex Noise (Stefan Gustavson)](http://staffwww.itn.liu.se/~stegu/simplexnoise/simplexnoise.pdf) — Efficient 3D noise for tendril steering

### Water Generator Enhancement

**Status: PLANNED**

The water generator uses a 30-particle rain system with simplex noise background. Drops spawn from the top edge (matrix) or random positions (linear), fall under gravity, and splash radially on impact. The background is a thresholded noise field with blue/green coloring. Audio reactivity drives spawn rate via phase breathing and beat-triggered wave bursts.

**Current weaknesses:**
- **Sparse** — 30 particles on 128 LEDs creates rain, not ocean
- **No surface simulation** — no visible waterline, no wave motion, no body of water
- **No ripples** — drops splash radially but no expanding ring patterns
- **Fixed color** — no depth gradient, no mood shifts, no bioluminescence
- **Background disconnected** — noise layer doesn't interact with particles
- **Wind is dead** — `windBase=0`, no audio-driven gusts
- **No foam/whitecaps** — wave crests have no visual distinction
- **`beatInMeasure` and `onsetDensity` unused**

**Proposed architecture: Layered water system.** Replace single-layer particle rain with stacked composited layers for a richer scene. Keep ParticleGenerator base class (particles still useful for rain/splashes/bioluminescence).

#### Tier 1: High Impact, Low Effort

**1. Two-buffer ripple simulation**
Classic demoscene water algorithm. Two `int16` buffers, 5 operations per cell per frame. Audio events inject impulses, ripples propagate and interfere automatically. This single addition transforms sparse "rain" into "living water surface."
```
new[x] = ((prev[x-1] + prev[x+1]) >> 1) - current[x]
new[x] -= new[x] >> 5  // ~3% damping
swap(prev, current)
```
- Audio: `pulse` → drop injection, `beat` → larger impulse, `downbeat` → edge wave sweep
- Cost: 512 bytes RAM, ~2K cycles/frame

**2. Depth-gradient base coloring (matrix)**
Row-dependent base color before particles/noise: pale cyan at surface → turquoise → deep blue → near-black at bottom. Creates immediate sense of water depth with zero CPU cost (per-row tint lookup).
```
Row 0 (surface): (140, 220, 255) — pale cyan
Row 2-3:         (0, 180, 220)   — turquoise
Row 5-6:         (0, 30, 120)    — deep blue
Row 7 (bottom):  (0, 10, 60)     — near-black
```

**3. Bioluminescence on audio events**
Blue-green glow (RGB ≈ 0, 80-255, 100-200) triggered by `pulse`. Glow appears at random position, persists 0.5-1s with exponential decay. Maps perfectly to transients — water "lights up" on kicks/snares without strobing.
- Audio: `pulse` → spawn glow, `energy` → glow brightness, `downbeat` → large area glow burst
- Cost: 128-byte glow buffer, ~500 cycles/frame

**4. Phase-driven wave speed breathing**
Modulate noise time advancement with `phase`: `noiseTime += baseSpeed * (0.5 + 0.5 * phaseToPulse())`. Water flows faster on-beat, slower off-beat. Creates rhythmic breathing without flash. Currently noise speed only varies by energy (0.012-0.05).

#### Tier 2: High Impact, Medium Effort

**5. Gerstner wave surface (matrix)**
3 summed trochoidal waves create rolling ocean surface with sharp crests and flat troughs. On 16×8: illuminate the row nearest the surface height with max brightness, exponential falloff below. Surface highlights at crests where slope changes sign. Sharp crests + flat troughs = unmistakably "water" even at low res.
```
x_disp = (steepness/k) * cos(k*x0 - w*t)
y_disp = (steepness/k) * sin(k*x0 - w*t)
```
- Audio: `energy` → wave amplitude, `phase` → wave speed, `downbeat` → inject large swell
- Cost: ~50K cycles/frame (6-8 trig calls per LED)

**6. Foam/whitecap at wave crests**
When surface height or turbulence (large neighbor height differences) exceeds threshold, blend toward white. Foam persists via separate buffer with slow decay (~1s half-life). Drifts with surface motion.
- Audio: `energy` → foam threshold (more energy = more foam), `rhythmStrength` → foam regularity
- Cost: 128 bytes RAM, ~500 cycles/frame

**7. Domain-warped noise background**
Replace current simplex noise with domain-warped noise: `noise(x + noise(x,y,t), y, t)`. Creates organic swirling currents and eddies instead of drifting blobs. Dramatically more organic.
- Audio: `energy` → warp amplitude, `phase` → time offset
- Cost: 2-3× current noise cost (~100K cycles), still under 0.2% CPU

**8. Onset density → rain intensity mapping**
`onsetDensity` (currently unused) drives rain character:
- Sparse (0-1/s): 1-2 drops/sec, gentle ripples, calm water
- Moderate (2-4/s): 5-10 drops/sec, overlapping ripples
- Dense (4-6/s): 15+ drops/sec, chaotic interference, foam activation
- Auto-matches rain to music density without manual tuning

#### Tier 3: Medium Impact, Creative

**9. Caustic overlay (matrix only)**
Animated Voronoi noise or `abs(noise1 + noise2)^0.5` creates underwater "swimming light" pattern. Applied as additive cyan/blue layer under particles. Per-cell: 9 neighbor checks × (2 sin + 1 sqrt + compare).
- Audio: `energy` → caustic brightness, `phase` → animation speed
- Cost: ~100-140K cycles/frame (~0.2% CPU)

**10. Multi-palette color system**
3 palettes via Inigo Quilez cosine formula `color(t) = a + b * cos(2π(c*t + d))`, blend by audio state:
- **Deep ocean** (low energy): dark blues, near-black
- **Tropical** (moderate energy): turquoise, bright cyan
- **Storm/moonlit** (high energy + high rhythm): silver/white highlights, dark base
- Blend factor: `energy × rhythmStrength`

**11. Ridged noise wave crests**
`1.0 - abs(noise3D(x, y, t))` creates sharp bright ridges with smooth dark valleys — looks like wave crests catching light. Can replace or blend with the current thresholded noise background.

**12. Drop trails (motion blur)**
Render falling drops as 2-3 LEDs with decreasing brightness along velocity vector. Makes individual drops visible at low resolution and adds sense of speed. Just render at `(x, y)`, `(x-vx*dt, y-vy*dt)`, `(x-2*vx*dt, y-2*vy*dt)` with 100%, 50%, 25% intensity.

**13. Beat-driven swell accumulator**
On each beat, add 0.1 to a "swell" variable that decays at 0.02/frame. Swell increases wave height and brightness. Multiple beats accumulate into growing swell that subsides between phrases. Makes musical sections feel like rising/falling seas.

#### Audio Signal → Water Parameter Mapping

| Signal | Current Use | Proposed New Mappings |
|--------|-------------|----------------------|
| `energy` | Weak spawn rate modifier (0.5-1.0×) | Wave amplitude, caustic brightness, foam threshold, color temperature, rain intensity |
| `phase` | Spawn breathing (0.4-1.0×) | Wave speed breathing, noise time modulation, background brightness |
| `pulse` | Beat → wave burst, organic transient drops | Ripple buffer injection, bioluminescence spawn, swell accumulation |
| `rhythmStrength` | Organic↔music blend | Foam regularity, rain pattern (random vs phase-locked), color palette blend |
| `downbeat` | Extra wave drops | Edge wave sweep, large bioluminescence burst, swell spike, palette warmth shift |
| `onsetDensity` | *Unused* | Rain intensity/character, turbulence level, ripple spawn rate |
| `beatInMeasure` | *Unused* | Wave direction bias (left/right alternation), accent ripple size |

**Key principle: Water absorbs energy into continuous flow.** Unlike fire (discrete sparks) or plasma (continuous tendrils), water should respond through amplitude and speed modulation — bigger waves, faster flow, more interference — not through discrete flashes.

#### Resource Budget

| Component | RAM | CPU (128 LEDs) |
|-----------|-----|----------------|
| Ripple buffers (2 × 128 × int16) | 512 bytes | ~2K cycles |
| Foam buffer (128 × uint8) | 128 bytes | ~500 cycles |
| Bioluminescence buffer (128 × uint8) | 128 bytes | ~500 cycles |
| Gerstner waves (3 waves) | — | ~50K cycles |
| Domain-warped noise | — | ~100K cycles |
| Caustics (Voronoi) | — | ~140K cycles |
| Particle pool (existing, 30) | ~600 bytes | ~5K cycles |
| **Total new** | **~1.5 KB** | **~0.25% CPU** |

All layers combined use under 0.3% CPU at 60 Hz. Enormous headroom on the nRF52840.

#### References

- [Demoscene 2D Water Effect (Hugo Elias)](https://web.archive.org/web/20160418004149/http://freespace.virgin.net/hugo.elias/graphics/x_water.htm) — Two-buffer ripple simulation
- [Catlike Coding: Flow / Waves](https://catlikecoding.com/unity/tutorials/flow/waves/) — Gerstner wave implementation
- [Inigo Quilez: Domain Warping](https://iquilezles.org/articles/warp/) — Noise domain warping for organic patterns
- [Inigo Quilez: Cosine Palettes](https://iquilezles.org/articles/palettes/) — Parametric color gradients
- [Lode Vandevenne: Plasma Tutorial](https://lodev.org/cgtutor/plasma.html) — Sine-sum interference patterns
- [The Book of Shaders: Voronoi](https://thebookofshaders.com/12/) — Animated Voronoi for caustics

## Design Philosophy

See **[VISUALIZER_GOALS.md](VISUALIZER_GOALS.md)** for the guiding philosophy. Key principle: visual quality over metric perfection. Low F1 on ambient/trap/machine-drum may represent correct visual behavior (organic mode fallback). False positives are the #1 visual problem.

## Key References

**Beat tracking (general):**
- BEAST: Streaming Transformer (ICASSP 2024)
- BeatNet+: CRNN + particle filter (TISMIR 2024, Heydari et al.)
- RNN-PLP-On: Real-time PLP beat tracking (TISMIR 2024, Meier/Chiu/Muller)
- Novel-1D: 1D state space with jump-back reward (ICASSP 2022, Heydari et al.)
- Percival 2014: Enhanced ACF + pulse train evaluation (IEEE/ACM TASLP)
- Krebs/Bock/Widmer 2015: Efficient state-space for joint tempo-meter (ISMIR)
- Davies 2010: Beat Critic octave error identification (ISMIR)
- Scheirer 1998: Comb filter bank tempo estimation

**Frame-level / embedded NN (Priority 1 references):**
- BeatNet (Heydari et al. 2021): CRNN frame-level activation + particle filter post-processing. Proven frame-level paradigm.
- Beat This! (2024): CNN + Transformer, frame-level mel activation. SOTA offline.
- madmom (Böck et al. 2016): Bidirectional LSTM on mel spectrogram, frame-level beat/downbeat activation.
- Krebs/Böck/Widmer 2016: Beat-synchronous downbeat RNN — the only beat-level approach, but requires bidirectional GRU + DBN. F1 drops from 90.4% → 77.3% with estimated beats.
- Gkiokas et al. 2017: CNN Beat Activation Function for dancing robot on ARM Cortex-A8. Practical embedded beat tracking with HW constraints.
- No published TinyML beat tracking on Cortex-M class hardware exists (as of March 2026).

## Architecture References

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | Design philosophy — visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioController + CBSS architecture |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference + test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator design patterns |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history |
| `blinky-test-player/NEXT_TESTS.md` | Current testing priorities |
