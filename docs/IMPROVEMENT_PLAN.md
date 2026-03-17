# Blinky Time - Improvement Plan

*Last Updated: March 16, 2026*

> **Historical content (v28-v64 detailed writeups, parameter sweeps, A/B test data)** archived via git history. See commit history for `docs/IMPROVEMENT_PLAN.md` prior to this date.

## Current Status

**Firmware:** v73 (SETTINGS_VERSION 73). CBSS beat tracking + Bayesian tempo fusion. Conv1D W64 NN beat/downbeat activation (always-on, TFLite required). Beat-synchronized downbeat and measure counter. ODF information gate, pulse baseline tracking, hybrid energy synthesis. AGC removed (v72) — fixed hardware gain (nRF52840: 32, ESP32-S3: 30). HeatFire hybrid audio-reactive design. 7 devices: 3 nRF52840 + 2 ESP32-S3 on blinkyhost, 1 nRF52840 tube + 1 ESP32-S3 display local.

**NN Model Status:** Conv1D W64 with Beat This! sum head deployed on all 7 devices (15.1 KB INT8, per-tensor quantization, 27ms inference measured on device). Beat F1=0.480, DB F1=0.160 (offline eval). Arena: 7340/32768 bytes. Conv1D(26→24,k=5) → Conv1D(24→32,k=5) → Conv1D(32→2,k=1). Sum head constrains downbeat ≤ beat structurally.

**Labels:** Training data upgraded to consensus_v5 (7-system: beat_this, madmom, essentia, librosa, demucs_beats, beatnet, allin1) with BPM-aware downbeat grid correction and quarantine of 1753 uncorrectable tracks. 75.3% of tracks have perfect every-4th-beat downbeat grids.

**Key constraint:** The LED visualizer runs on a single thread at 60 Hz. Total frame budget is 16.7ms. Conv1D W64 inference takes 27ms (exceeds single-frame budget but runs every frame with acceptable visual results). Mel-spectrogram CNNs require 79-98ms (too slow).

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

**Status: Conv1D W64 DEPLOYED (March 16, 2026) — Beat F1=0.480, DB F1=0.160**

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

| Window | Frames | Input dim | Coverage at 120 BPM | Beat F1 | DB F1 | Notes |
|--------|--------|-----------|---------------------|---------|-------|-------|
| 0.5s | 32 | 832 | ~1 beat | 0.491 | 0.238 | FC model (cal63) |
| 1.0s | 64 | 1,664 | ~2 beats | 0.480 | 0.160 | **Conv1D W64 DEPLOYED** — 15.1 KB INT8, 27ms |
| 1.0s | 64 | 1,664 | ~2 beats | 0.487 | 0.180 | FC W64 (marginal for downbeat) |
| 3.07s | 192 | 4,992 | ~6 beats (1.5 bars) | 0.370 | 0.145 | **FC REGRESSED** — too many flat inputs |

**Conclusion (March 15):** Wider windows are necessary for downbeat (need to see a full 4/4 bar) but FC architecture cannot exploit them. The first FC layer (4992→64) has 319K of the model's 322K total params, and must implicitly encode all temporal relationships through weight correlations. This destroys temporal locality. Conv1D with temporal pooling is the correct architecture for wide windows — preserves local patterns through convolutions, then progressively compresses time dimension via pooling before FC classification.

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

| Resource | BandFlux (removed v67) | Conv1D W64 (deployed) | Notes |
|----------|-------------------|-------------------|-------|
| ODF quality | ~28% F1 | Beat F1=0.480, DB F1=0.160 | Learned vs hand-tuned |
| Inference time | <0.1ms @ 62.5 Hz | 27ms @ 62.5 Hz | Conv1D dominates frame budget |
| Tensor arena | 0 | 7340 bytes (of 32768 allocated) | Conv1D W64 |
| Mel frame buffer | 0 | ~6.7 KB (64×26×4 bytes) | Ring buffer |
| Model flash | 0 | 15.1 KB INT8 | Per-tensor quantization |
| Downbeat | No | Yes (sum head) | Structural constraint: downbeat ≤ beat |

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
- ~~**Phase C (dual-model architecture):**~~ ABANDONED (Mar 16). Research showed every published beat/downbeat system uses a single joint model — the dual-model split underperformed the FC baseline on both tasks (OnsetNN Beat F1=0.478 < FC 0.491; RhythmNN DB F1=0.114 < FC 0.238) while using 8.5x more RAM. Reverted to single Conv1D with multi-task output.
- ~~**Phase C (revised): Single Conv1D W64 with sum head.**~~ DONE — Conv1D(26→24,k=5) → Conv1D(24→32,k=5) → Conv1D(32→2,k=1). Beat This! sum head constrains downbeat ≤ beat. W64 (1.024s, ~2 beats). 15.1 KB INT8, 27ms measured on device. Beat F1=0.480, DB F1=0.160. Deployed on all 7 devices.
- ~~**Phase D (BandFlux removal):**~~ DONE (v67) — Removed EnsembleDetector, BandFlux, EnsembleFusion, BassSpectralAnalysis, IDetector, DetectionResult. 10 files deleted, ~2600 lines, ~24 settings, ~22 KB flash, ~2 KB RAM saved. SETTINGS_VERSION 66→67.

**Research context (updated March 16, 2026):**
- ALL leading beat trackers use frame-level NNs with **single joint models**: BeatNet (CRNN), Beat This! (CNN+Transformer), madmom (BiLSTM), TCN beat tracker, BEAST (Streaming Transformer), WaveBeat (Strided Conv1D)
- **No published system splits onset from downbeat into separate models** — multi-task joint training improves both tasks via shared features, structural constraints (downbeats ⊆ beats), and regularization (Bock 2019: +5 F1 from tempo auxiliary task)
- Beat This! "sum head" technique constrains downbeat output ≤ beat output structurally
- Our Conv1D W64 with sum head follows this proven paradigm

#### Closed: Dual-Model Architecture (March 15-16, 2026)

**Attempted and abandoned.** Dual-model split (OnsetNN W8 + RhythmNN W192) was architecturally wrong:
- OnsetNN (Beat F1=0.478) < FC baseline (0.491) — W8 too short to distinguish beats from non-beats
- RhythmNN (DB F1=0.114) < FC baseline (0.238) — 16x pooled labels lost 94.6% of beat labels (bug), plus 74ms inference (over budget)
- Combined 8.5x more RAM than FC for worse results on both tasks
- Every published system uses single joint model — no literature support for the split

#### Architecture Research (March 16, 2026)

**Platform compute budgets (30ms inference):**

| Platform | Clock | Cycles/MAC | MACs budget | Model sweet spot |
|----------|-------|-----------|-------------|-----------------|
| nRF52840 | 64 MHz | ~3 (flash waits) | ~640K | 2-3 layer Conv1D, 24-32ch, W32-64, 5-15K params |
| ESP32-S3 | 240 MHz | ~1.7 (ESP-NN) | ~4M | 3-4 layer Conv1D, 48-96ch, W64-128, 20-80K params |

ESP32-S3 has ~6.5x more compute budget — platform-specific models are feasible.

**Improving downbeat without wider windows (ranked by viability):**

1. **Tempo auxiliary head during training (BEST)**: Shared backbone predicts tempo via FC branch, stripped at export. Zero firmware cost. +1-5% F1 (Bock 2019, BEAST). Code exists in `beat_fc_enhanced.py`.
2. **FiLM conditioning (LATER)**: Modulate features by tempo/phase. Train/test mismatch problem — try after beat tracking improves.
3. **Beat-synchronous mel accumulator (NOT YET)**: Depends on beat quality (~50% F1 makes snapshots noisy). Revisit above 70% beat F1.
4. **DSP features as extra channels (NO)**: Constant across Conv1D window, useless to convolutions.

**Input representation:** Mel bands confirmed correct by all SOTA systems. 62.5 Hz frame rate adequate.

#### Closed: W192 FC (March 15, 2026)

W192 FC (4992→64→32→2, 322K params, 314 KB INT8): Beat F1=0.370, DB F1=0.145 — severe regression from W32 FC (0.491/0.238). Root cause: FC flattening of 192×26=4992 inputs destroys temporal locality. The first FC layer (4992→64) contains 319K of the model's 322K total parameters and must implicitly encode all temporal relationships through raw weight correlations. Training converged to val_loss=0.497 with only 84.5% accuracy — the model failed to learn meaningful temporal patterns from the flat input. Superseded by dual-model architecture with Conv1D temporal pooling.

### Priority 2 (new): ESP32-S3 Mic Calibration and Model

**Status: Phase 1 DONE (March 15, 2026) — mic profile measured, training config created. Phase 2 (retrain) pending.**

The XIAO ESP32-S3 Sense uses a PDM microphone via ESP-IDF I2S PDM-RX. Its frequency response, noise floor, and AGC transfer function differ from the nRF52840's built-in microphone. The nRF52840 cal63 model was trained on mel spectrograms captured at `target_rms_db=-63 dB` through the nRF52840 mic chain. Running that model on ESP32-S3 audio will produce mismatched mel statistics and degraded ODF quality.

**Calibration results (March 15, 2026):**

| Parameter | nRF52840 | ESP32-S3 |
|-----------|---------|---------|
| `target_rms_db` | -63 dB | **-30 dB** |
| Operating mel mean | ~0.52 | ~0.78 |
| Best SNR gain | gain=30 (hw AGC) | gain=30 (sw gain only) |
| Band gain range | 0.95–1.04 | 0.93–1.01 (flat) |
| Noise floor range | 0.58–0.94 | 0.66–1.00 |

The ESP32-S3 operates ~0.25 higher mel mean. The software-only AGC (no hardware gain register) converges to a higher operating point (~gain=14–18 during music) vs nRF52840's hardware AGC. Best SNR at gain=30 on both units (27.8–28.7 dB avg across bands 3–25). Above gain=30, SNR degrades (software gain adds noise without pre-decimation amplification).

**Measured unit-to-unit variation:** Two ESP32-S3 units (ACM0 MAC `11:F8:10`, ACM3 MAC `12:C1:A0`) are < 1 dB apart after confirming identical firmware. The original apparent 3–4 dB divergence was entirely due to firmware version mismatch (one unit was running old firmware with a different streaming rate — 40 Hz vs 21 Hz). Normal MEMS mic manufacturing tolerance is ±1–3 dB per spec; actual measured variation is ≤1 dB between these two units.

**Artifacts:**
- Mic profile: `ml-training/data/calibration/mic_profile_esp32s3.npz`
- Gain sweeps: `data/calibration/gain_sweep_ttyACM0_esp32.npz`, `gain_sweep_ttyACM3_esp32.npz`
- Training config: `ml-training/configs/frame_fc_esp32s3.yaml` (`target_rms_db=-30`, `hw_gain_max=30`)

**Remaining steps:**

2. **Train ESP32-S3 model** — Retrain the FC beat/downbeat model with ESP32-calibrated data:
   ```bash
   python scripts/prepare_dataset.py --config configs/frame_fc_esp32s3.yaml --augment \
       --mic-profile data/calibration/mic_profile_esp32s3.npz
   python train.py --config configs/frame_fc_esp32s3.yaml
   ```
   Same architecture (32-frame window, FC [64,32] → 2 outputs). Produces `frame_beat_model_esp32s3_data.h`.

3. **Firmware model selection** — Add `#ifdef BLINKY_PLATFORM_ESP32S3` to select the ESP32-S3 model at compile time, keeping the nRF52840 model unchanged.

**Hardware gain note:** The ESP32-S3 I2S PDM-RX slot config has **no hardware gain register** — confirmed by ESP-IDF source (`soc_caps.h`: `SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER` not defined for S3, `i2s_ll.h`: no gain functions in PDM RX path). The full AGC range is applied as software post-decimation gain in `Esp32PdmMic::setGain()` / `poll()`.

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

3. **Downbeat detection quality (F1 ~0.16 offline with Conv1D W64)** — Consensus v5 labels (7-system). Conv1D W64 with sum head deployed but downbeat F1 limited. Next improvements: confidence-weighted loss, tempo auxiliary head during training.

4. **~135 BPM gravity well** — Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks. Not improved by CBSS parameter tuning (March 13 sweep). Not a tempo bin resolution issue (47 bins tested v61; ACF already evaluates at full lag resolution). Likely requires better NN ODF discrimination or Rayleigh prior adjustment.

5. **Phase alignment** — CBSS derives phase indirectly from a counter. Sharper NN beat activations give CBSS better signal for phase tracking.

6. ~~**NN inference speed (RESOLVED)**~~ — Conv1D W64 runs 27ms (exceeds single 16.7ms frame budget but acceptable in practice).

7. ~~**Per-channel quantization (RESOLVED March 12)**~~ — Fixed: `_experimental_disable_per_channel=True` in export.

## SOTA Context (March 2026)

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~48%** | Conv1D ODF + CBSS (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends (CNN, CRNN, Transformer) that require 79ms+ on our hardware. The Conv1D W64 approach follows the same paradigm (frame-level NN activation → post-processing) but uses lightweight Conv1D layers, achieving 27ms inference. The NN is the sole ODF source, providing a learned beat activation that feeds into CBSS for tempo/phase tracking. Using raw mel bands as the stable interface decouples the NN from firmware signal processing parameters.

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
- `FireDefaults` struct removed (commit 15ce5c7). Was vestigial from old heat-diffusion model — `Fire::begin()` never read it.
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

##### ~~Tier 1: High Impact, Low Effort~~ — ALL DONE

**~~1. Dynamic particle pool~~** — DONE (commit b856b98). Removed compile-time `ParticleGenerator<N>` template. Pool allocated in `begin()` from `particleDensity() * numLeds`. 32×32 display gets 768 fire particles (was capped at 64). All generators (Fire/Water/Lightning) auto-size.

**~~2. Domain-warped noise background~~** — DONE (commit 5060701). `MatrixBackground::sampleNoise()` uses Stefan Petrick domain-warp for fire style: first noise field distorts coordinates of second. Swirling lava-lamp movement. Same cost (2 noise evals/pixel).

**~~3. Multi-palette blending~~** — DONE (commit 5060701). Two palettes (warm campfire, hot white-blue) in `particleColor()`. Blended by smoothed `energy * rhythmStrength` via `paletteBias_` (low-pass, ~0.5s time constant). Quiet = warm amber, loud rhythmic = white-hot.

**~~4. Audio-driven gamma curve~~** — DONE (commit 5060701). `powf(intensity/255, gamma)` remap before palette lookup. Gamma 1.3 (cool) → 0.7 (hot) driven by `paletteBias_`. More ember glow when loud, only brightest sparks when quiet.

##### Tier 2: High Impact, Medium Effort

**~~5. Curl noise wind field~~** — DONE
Replaced fbm3D offset-pair approximation in `MatrixForceAdapter::applyWind()` with proper Bridson curl noise: finite-difference curl of a shared scalar noise field. 4 `noise3D_01` evals per particle. Divergence-free velocity field → particles swirl around each other. Applied as advection (position displacement), not force. All generators on matrix layouts benefit (Fire, Water, Lightning).

**~~6. Energy → dynamic flame height~~** — DONE
Added dynamic kill in `Fire::updateParticle()`: `flameTop = height * (1 - (0.4 + 0.6 * energy))`. Particles above this threshold are killed. Quiet music = short flame (40% height), loud = full height. Also: background intensity now modulated by energy (`0.3 + 0.7 * energy`), so ember bed brightens with volume. Matrix-only (linear layouts unaffected).

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
| `phase` | Thermal buoyancy breathing, spawn pump, velocity boost, drag, wind breathing, background brightness | — (fully utilized) |
| `pulse` | Burst spark count, velocity boost, wind gust magnitude | — (fully utilized) |
| `downbeat` | Extra sparks, spread expansion (2.5×), color temp shift, velocity burst (1.5×) | Vortex injection (#10) |
| `energy` | Spawn rate, noise speed, wind turbulence, **palette blend (#3)**, **gamma (#4)**, **flame height (#6)**, **bg brightness (#6)** | — (fully utilized) |
| `rhythmStrength` | Organic/music blend (spawn, velocity, wind, type), **palette blend (#3)** | — (fully utilized) |
| `onsetDensity` | Lifespan modulation, noise speed, wind turbulence amplitude | Reaction-diffusion rate (#9) |
| `beatInMeasure` | Beat 1/3/2/4 accent patterns, left/right spawn rocking bias | — (fully utilized) |

#### Cleanup: ~~Remove Vestigial `FireDefaults`~~ — DONE (commit 15ce5c7)

Removed `FireDefaults` struct, `PropagationModel` hierarchy, `CenterSpawnRegion`, unused `SpawnRegion` methods, `Particle::trailHeatFactor`, `ParticleFlags::EMIT_TRAIL`, `Generator::coordsToIndex/indexToCoords`, `GeneratorType::CUSTOM`. 32 files, -787 lines. Particle struct 28→24 bytes.

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
