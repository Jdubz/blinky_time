# Blinky Time - Improvement Plan

*Last Updated: March 10, 2026*

> **Historical content (v28-v64 detailed writeups, parameter sweeps, A/B test data)** archived via git history. See commit history for `docs/IMPROVEMENT_PLAN.md` prior to this date.

## Current Status

**Firmware:** v65 (SETTINGS_VERSION 64, new v65 tunable parameters). CBSS beat tracking + Bayesian tempo fusion. BandFlux Solo detector. Beat-synchronized downbeat and measure counter. Onset snap hysteresis and PLL warmup relaxation.

**NN Model Status:** All mel-spectrogram NN models are too slow for real-time use. The system is a single-threaded LED visualizer requiring 30-60 fps. With ~16.7ms per frame at 60fps, only ~10ms is available for inference after other processing. No mel-spectrogram model has achieved <79ms on Cortex-M4F @ 64 MHz.

**Key constraint:** The LED visualizer runs on a single thread at 60 Hz. Total frame budget is 16.7ms. Audio processing + FFT + detection + CBSS + generator + LED output consume ~6-7ms, leaving **~10ms for any NN inference**. Mel-spectrogram models (26 bands × 128 frames = 3,328 inputs) cannot fit this budget on Cortex-M4F.

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

### Priority 1: Feature-Level NN Architecture

**Status: DESIGN PHASE (March 10, 2026)**

**Problem:** Mel-spectrogram models process 3,328+ inputs and require 79-98ms inference — 8-10× over budget. The signal chain already extracts rich features (onset strength, autocorrelation, comb filter state, spectral flux) that are discarded before the NN sees them.

**Approach:** Train a tiny model on pre-extracted features from the existing signal chain instead of raw mel spectrograms. The model assists and corrects the real-time algorithms rather than replacing them.

**Input features (candidate set, ~30-64 floats):**

| Feature | Size | Source | Rate |
|---------|------|--------|------|
| OSS buffer (recent) | 16-32 floats | AudioController circular buffer | 60 Hz |
| ACF output | 8-16 floats | Autocorrelation (top lags + scores) | 2 Hz |
| Comb filter bank | 20 floats | Bayesian tempo posterior | 2 Hz |
| Current BPM + confidence | 2 floats | AudioController state | 60 Hz |
| Spectral band energies | 4-8 floats | SharedSpectralAnalysis | 60 Hz |

**Output targets:**
- Beat activation (0-1): confirms or corrects CBSS beat placement
- Downbeat activation (0-1): identifies bar boundaries
- Tempo correction signal: nudges BPM when ACF is ambiguous

**Projected inference:** With ~64 input features → 1-2 hidden layers (16-32 units) → 2-3 outputs, total <500 parameters, <5,000 MACs. Estimated **<1ms inference** on Cortex-M4F. Well within 10ms budget.

**Three candidate architectures:**

1. **1D TCN on ODF time series** — Process 32-64 recent OSS samples through 2-3 conv layers (k=3, 8-16ch). Runs every frame at 60 Hz. Learns temporal onset patterns that predict beats. ~2,000 params, ~2ms projected.

2. **Beat-synchronous downbeat classifier (Krebs 2016 style)** — Accumulate features between beats, run inference only at beat time (~2 Hz). Input: spectral profile at beat + inter-beat statistics + position-in-measure history. Very MCU-friendly since it runs at beat rate, not frame rate. ~500 params, <1ms per beat.

3. **Learned ACF refinement** — Post-process the raw ACF output (140 lags) through a small FC network to disambiguate harmonics. Runs at ACF update rate (2 Hz). Could directly replace the hand-tuned octave discrimination logic. ~1,000 params, <1ms per update.

**Approach (2) is most promising** for downbeat detection — Krebs et al. (2016) demonstrated that processing one feature vector per beat (not per frame) is sufficient for downbeat tracking, achieving strong results with a 180-dimensional input at ~2 Hz inference rate.

**Training data strategy:**

Existing mel-spectrogram training data (64 GB, 3.8M chunks of 128×26) is **not reusable** — wrong feature format. Reusable assets:
- Raw audio files (`/mnt/storage/blinky-ml-data/audio/`, ~400 tracks)
- 4-system consensus beat/downbeat labels (`/mnt/storage/blinky-ml-data/labels/consensus_v2/`)
- Mic calibration profiles (gain-aware augmentation)

New data pipeline requires two components:

1. **Python signal chain simulator** (MUST BUILD) — Reimplement BandFlux → OSS → ACF → comb filter → CBSS in Python. Process all training audio offline. This is the only practical path for bulk data generation (thousands of tracks). No firmware signal chain simulation exists today — `prepare_dataset.py` only does mel extraction.

2. **Firmware feature exporter** (MUST BUILD) — Add serial commands to dump OSS buffer history (360 frames), ACF top lags + scores, comb filter 20-bin posterior, CBSS state. The current `stream nn` command exports mel+onset+BPM+phase per frame but NOT internal buffers. Needed to validate Python simulator matches real firmware behavior.

For the beat-synchronous downbeat approach (candidate 2), the simulator only needs OSS + spectral profile at beat times — a simpler subset than the full chain.

**Training pipeline:**
1. Build Python signal chain simulator (BandFlux, OSS, ACF, comb filter bank, CBSS)
2. Validate simulator against firmware feature exporter on reference tracks
3. Process all training audio through simulator → extract feature-level inputs
4. Align with ground truth beat/downbeat labels (existing 4-system consensus)
5. Train in PyTorch
6. Export to TFLite INT8 (<2 KB model size)
7. Deploy alongside existing signal chain

**Key advantage:** The existing signal chain does heavy lifting (FFT, spectral analysis, onset detection, autocorrelation, CBSS). The NN only needs to learn patterns in already-extracted features, making the task dramatically simpler than end-to-end mel-spectrogram processing.

**Research context:**
- Krebs, Böck & Widmer (2016): Beat-synchronous RNN for downbeat detection. Processes one 180-dim feature vector per beat (~2 Hz). State of the art for meter tracking. Directly applicable to our beat-rate inference approach.
- Gkiokas et al. (2017): CNN Beat Activation Function for dancing robot on ARM Cortex-A8. Demonstrates practical embedded beat tracking with architectural constraints.
- No published TinyML beat tracking system on Cortex-M class hardware exists. Blinky appears to be the first.

**Changes deferred from v9 DS-TCN (applicable to future mel-spectrogram models if revisited):**
- `beat_cnn.py`: Set `bias=False` on `DSConvBlock.pw_conv` and `DSTCNBeatCNN.input_conv` (redundant before BatchNorm). Also update `export_tflite.py`.

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

1. **NN inference speed** — No mel-spectrogram model fits within the 10ms inference budget. Feature-level approach (Priority 1) is the path forward.

2. **~135 BPM gravity well** — Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks. Tested 47 bins in v61 — no improvement. NOT a bin count issue. A learned ACF refinement model (Priority 1, approach 3) could help.

3. **Phase alignment** — Correct BPM doesn't translate to correct beat placement. CBSS derives phase indirectly. All systems achieving >60% F1 use explicit phase tracking (HMM, PLP oscillator, or particle cloud). Feature-level NN could learn phase correction patterns.

4. **Downbeat detection quality** — Only 2/4 consensus systems provide downbeat labels. Current downbeat F1 ~0.33 offline. Beat-synchronous classifier (Priority 1, approach 2) is the most promising path.

## SOTA Context (March 2026)

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~28%** | NN ODF + CBSS (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends. Our gap is primarily in ODF quality, not the beat tracking backend. However, all SOTA neural frontends require 79ms+ inference on our hardware — the feature-level approach (Priority 1) aims to close this gap using pre-extracted features within the 10ms budget.

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Multi-factorial (data bias, prior, comb harmonics) | **Medium** | Improved NN ODF may help |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | Open research question |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** | Acceptable for visuals |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** | Organic mode is correct |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** | Energy-reactive acceptable |

## Closed Investigations (v28-v65)

All items below were A/B tested and showed zero or negative benefit, or proven infeasible. Removed from firmware in v64 unless noted.

- **Mel-spectrogram NN models (v4-v9)**: All architectures (standard conv, BN-fused, DS-TCN) exceed 79ms inference on Cortex-M4F @ 64 MHz. The v9 DS-TCN (designed for speed) measured 98ms due to INT8 ADD requantization overhead from residual connections. No mel-spectrogram architecture can fit the 10ms inference budget. NN=1 build flag and BeatActivationNN.h retained for future feature-level models.

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

**Feature-level / embedded NN (Priority 1 references):**
- Krebs/Böck/Widmer 2016: Beat-synchronous downbeat RNN — processes one 180-dim vector per beat (~2 Hz), not per frame. Key insight for our beat-rate inference approach.
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
