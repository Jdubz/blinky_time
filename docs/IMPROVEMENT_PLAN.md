# Blinky Time - Improvement Plan

*Last Updated: March 11, 2026*

> **Historical content (v28-v64 detailed writeups, parameter sweeps, A/B test data)** archived via git history. See commit history for `docs/IMPROVEMENT_PLAN.md` prior to this date.

## Current Status

**Firmware:** v65 (SETTINGS_VERSION 64, new v65 tunable parameters). CBSS beat tracking + Bayesian tempo fusion. BandFlux Solo detector. Beat-synchronized downbeat and measure counter. Onset snap hysteresis and PLL warmup relaxation.

**NN Model Status:** Mel-spectrogram CNN models (v4-v9) are too slow (79-98ms). Beat-synchronous FC hybrid abandoned (circular dependency, no discriminative signal). Pivoting to **frame-level FC**: sliding window of raw mel frames → FC layers → beat + downbeat activation. ~60-200µs inference, well within 10ms frame budget.

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

### Priority 1: Frame-Level FC Beat/Downbeat Activation

**Status: DESIGN PHASE (March 11, 2026)**

**Problem:** The deterministic pipeline (BandFlux + CBSS) achieves only ~28% beat F1 in mic-in-room conditions. Core failures:
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

2. **NN replaces BandFlux as ODF source.** The beat activation output feeds directly into CBSS as a higher-quality ODF signal. BandFlux becomes the fallback when NN is not compiled in.

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

| Resource | BandFlux (current) | Frame-level FC NN | Notes |
|----------|-------------------|-------------------|-------|
| ODF quality | ~28% F1 | Target >50% F1 | Learned vs hand-tuned |
| Inference time | <0.1ms @ 62.5 Hz | ~60-200µs @ 15.6 Hz | Both negligible |
| Tensor arena | 0 | ~8-16 KB | Modest |
| Mel frame buffer | 0 | ~3.3 KB (32×26×4 bytes) | Ring buffer |
| Model flash | 0 | ~20-60 KB INT8 | Depends on architecture |
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

- **Phase A (beat activation only):** Train FC on frame-level beat labels. NN output replaces BandFlux as CBSS ODF. A/B test vs BandFlux. No downbeat yet — validates the approach.
- **Phase B (+ downbeat):** Add downbeat output head. Train jointly on beat + downbeat labels. Evaluate downbeat F1 improvement.
- **Phase C (progressive simplification):** If NN ODF is reliable, A/B test removing hand-tuned BandFlux parameters (gamma, band weights, threshold, cooldown). Each parameter group removed independently with A/B validation.

**Research context:**
- ALL leading beat trackers use frame-level NNs: BeatNet (CRNN), Beat This! (CNN+Transformer), madmom (BiLSTM), TCN beat tracker
- Our innovation: using FC instead of CNN/RNN to fit Cortex-M4F compute budget, while following the same frame-level activation → post-processing paradigm
- No published TinyML beat tracking on Cortex-M class hardware exists (as of March 2026)

### Priority 2: v65 Parameter Calibration

**Status: NOT STARTED**

New v65 parameters need sweep testing:

| Parameter | Default | Range | Command |
|-----------|---------|-------|---------|
| `snaphyst` | 0.8 | 0.0-1.0 | `set snaphyst` |
| `dbema` | 0.3 | 0.05-0.9 | `set dbema` |
| `dbthresh` | 0.5 | 0.1-0.9 | `set dbthresh` |
| `dbdecay` | 0.85 | 0.5-0.99 | `set dbdecay` |
| `pllwarmup` | 5 | 0-20 | `set pllwarmup` |

### Priority 3: CBSS ODF Contrast (cbssContrast=2.0)

**Status: NOT TESTED**

BTrack applies power-law contrast (squaring) to the ODF before CBSS. Sharpens beat peaks relative to non-beat frames. Our `cbssContrast` parameter exists (AudioController.h) but defaults to 1.0 (no contrast).

**Test plan:**
1. A/B test: `cbssContrast=1.0` vs `cbssContrast=2.0` (BTrack-style)
2. Standard 18-track EDM test set, 3 devices
3. Duration 35s, settle 12s
4. If 2.0 wins, sweep [1.5, 2.0, 2.5, 3.0]

```bash
cd blinky_time/blinky-test-player && NODE_PATH=node_modules node ../ml-training/tools/ab_test_multidev.cjs \
  --baseline "cbsscontrast=1.0" --candidate "cbsscontrast=2.0" \
  --tracks ../music/edm/track_manifest.json --duration 35 --settle 12
```

### Future: Heydari 1D State Space

**Status: RESEARCH ONLY**

Heydari et al. (ICASSP 2022) — 1D probabilistic state space with "jump-back reward" achieves 76.5% F1 online with 30x speedup over 2D joint models. ~860 states fits our memory budget. Could replace CBSS if feature-level NN improvements are insufficient.

## Current Bottlenecks

1. **Deterministic pipeline accuracy (~28% F1)** — BandFlux + CBSS is unreliable in mic-in-room conditions. Root causes: octave errors, phase drift, false beats during breakdowns, slow tempo adaptation. Priority 1 frame-level FC provides a learned ODF that should improve all four via better activation quality feeding into CBSS.

2. **Downbeat detection quality (F1 ~0.33)** — Only 2/4 consensus systems provide downbeat labels. Priority 1 Phase B provides learned downbeat activation from frame-level mel context.

3. **~135 BPM gravity well** — Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks. A learned ODF that cleanly marks real beats (and not sub-harmonics) should reduce octave ambiguity in ACF/CBSS.

4. **Phase alignment** — CBSS derives phase indirectly from a counter. A higher-quality NN ODF with sharper, more accurate beat activations gives CBSS better signal for phase tracking.

5. **NN inference speed (RESOLVED)** — Mel-spectrogram CNN models require 79-98ms at 62.5 Hz (8-10× over budget). Frame-level FC runs ~60-200µs at 15.6 Hz, well within budget.

## SOTA Context (March 2026)

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~28%** | NN ODF + CBSS (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends (CNN, CRNN, Transformer) that require 79ms+ on our hardware. The frame-level FC approach (Priority 1) follows the same paradigm (frame-level NN activation → post-processing) but uses FC layers instead of convolutions, achieving ~60-200µs inference — 400-1600× faster. The NN replaces BandFlux as the ODF source, providing a learned beat activation that feeds into CBSS for tempo/phase tracking. Using raw mel bands as the stable interface decouples the NN from 47+ tunable firmware parameters.

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Multi-factorial (data bias, prior, comb harmonics) | **Medium** | P1 NN ODF (cleaner activations reduce ACF ambiguity) |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | P1 NN ODF (sharper beat activations improve CBSS phase) |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** | Acceptable for visuals |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** | Organic mode is correct |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** | Energy-reactive acceptable |

## Closed Investigations (v28-v65)

All items below were A/B tested and showed zero or negative benefit, or proven infeasible. Removed from firmware in v64 unless noted.

- **Mel-spectrogram NN models (v4-v9)**: All architectures (standard conv, BN-fused, DS-TCN) exceed 79ms inference on Cortex-M4F @ 64 MHz. The v9 DS-TCN (designed for speed) measured 98ms due to INT8 ADD requantization overhead from residual connections. No mel-spectrogram CNN architecture can fit the 10ms per-frame budget. Superseded by frame-level FC approach (~60-200µs). NN=1 build flag retained for frame-level FC model.

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
