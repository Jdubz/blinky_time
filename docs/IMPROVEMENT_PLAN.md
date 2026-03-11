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

### Priority 1: Beat-Synchronous Spectral NN

**Status: DESIGN PHASE (March 10, 2026)**

**Problem:** The deterministic pipeline (BandFlux + CBSS) achieves only ~28% beat F1 in mic-in-room conditions. Core failures:
- **Octave errors**: ACF/comb filters have strong sub-harmonic peaks → half/double-time lock (135 BPM gravity well). Hand-tuned octave checks help but are brittle.
- **Phase drift**: CBSS derives phase from a counter. Slight BPM error accumulates phase offset. PLL correction is conservative to avoid jitter.
- **False beats during breakdowns**: CBSS forces beats when no onset is detected, maintaining phase through silence but triggering false visuals.
- **Slow tempo adaptation**: Bayesian fusion with conservative transitions takes 4-8 seconds to lock a new tempo after a transition.
- **Downbeat detection**: F1 ~0.33 offline. No spectral content analysis — purely counting beats modulo 4.

Mel-spectrogram NN models that could improve ODF quality require 79-98ms inference — 8-10× over budget.

**Approach:** Accumulate spectral summaries between beats, run a hybrid classifier at beat time (~2 Hz) that both **corrects the deterministic pipeline** and **provides new capabilities** (downbeat, meter). The NN validates and corrects CBSS rather than passively layering on top of it.

**Architecture: Beat-Synchronous Spectral Accumulator + Hybrid Corrector**

```
SharedSpectralAnalysis (already runs every frame, no additional cost)
     │
     ├── melBands_ (26 bands, 62.5 Hz)
     │         │
     │    SpectralAccumulator (NEW, ~200 bytes firmware)
     │         │  Between beats: running sum + count + max of mel bands per frame
     │         │  At beat fire: avg_mel[26], peak_mel[26], odf_stats[3] → 55 floats
     │         │
     │    Beat history buffer (last 4-8 beats):
     │         │  beat_features[8][55] ≈ 1.8 KB
     │         │
     │    BeatSyncNN inference (at beat fire only, ~2 Hz):
     │         │  Input:  4 beats × 55 features = 220 floats
     │         │  Model:  220 → 32 → 16 → N outputs (FC layers)
     │         │  ~7,500 params, ~8 KB INT8, <0.5ms inference
     │         │
     │         ├── CORRECTION outputs → feed back into AudioController:
     │         │     beat_confidence  → suppress false beats (breakdowns)
     │         │     tempo_factor     → correct octave errors (×2, ×0.5, ×1)
     │         │     phase_offset     → nudge beat timing
     │         │
     │         └── NEW CAPABILITY outputs → AudioControl:
     │               downbeat_prob   → AudioControl.downbeat
     │               meter           → AudioControl.beatInMeasure
     │
     ├── BandFlux ODF → CBSS → beat detection (NN corrections applied)
     │
     └── AudioControl (all fields, with NN-corrected phase/tempo + downbeat/meter)
```

**Why beat-synchronous, not per-frame:**
- Runs at ~2 Hz (at beat fire) instead of 62.5 Hz → 30x less inference work
- Can use richer inputs per inference (220 floats vs 3,328 for mel CNN)
- Even 10,000 params at 2 Hz is trivial CPU
- Mel bands are already computed every frame by SharedSpectralAnalysis — zero additional feature cost
- Krebs et al. (2016) demonstrated one 180-dim feature vector per beat is sufficient for downbeat tracking

**Per-beat feature vector (55 floats):**

| Feature | Size | Computation |
|---------|------|-------------|
| `avg_mel` | 26 | Mean mel bands over frames since last beat |
| `peak_mel` | 26 | Max mel bands over frames since last beat |
| `odf_mean` | 1 | Mean spectral flux since last beat |
| `odf_max` | 1 | Peak spectral flux since last beat |
| `onset_ratio` | 1 | Fraction of frames above onset threshold |

**Model outputs (hybrid — correction + new capabilities):**

*Correction outputs (feed back into AudioController):*
- **Beat confidence (0-1)**: How likely this beat was a true beat. Low values during breakdowns suppress forced beats and reduce `rhythmStrength`. Addresses false beat problem.
- **Tempo factor**: Octave correction signal. Trained to output ~1.0 when tempo is correct, ~2.0 when half-time, ~0.5 when double-time. The NN learns this from spectral patterns — at the correct tempo, beats alternate kick-heavy and snare-heavy spectral profiles. At half-time, every beat looks the same. Addresses octave error and 135 BPM gravity well.
- **Phase offset (-0.5 to 0.5)**: Fractional beat correction. If the spectral onset peak is consistently offset from the beat anchor, nudge the PLL. Addresses phase drift.

*New capability outputs:*
- **Downbeat probability (0-1)**: Identifies bar boundaries from spectral content changes (harmony shifts, bass patterns). Addresses downbeat F1 ~0.33.
- **Meter estimate**: Currently 4/4 assumed. Could learn 3/4, 6/8 from beat-level spectral patterns.

**How correction outputs integrate with CBSS:**
1. **Beat confidence < threshold** → AudioController reduces `rhythmStrength`, suppresses visual pulse. Does NOT stop the beat counter (CBSS continues tracking internally so it can recover quickly).
2. **Tempo factor != 1.0** → AudioController applies `switchTempo(bpm * tempoFactor)` with existing Bayesian posterior nudge (`tempoNudge`). More reliable than the current shadow CBSS octave checker because the NN sees spectral evidence, not just beat strength ratios.
3. **Phase offset** → Added to PLL's proportional correction term. Bounded by existing PLL clamp (±T/4 after warmup). Replaces or supplements the onset snap heuristic.

**Training the correction outputs requires CBSS simulation** (partial):
- Beat confidence: Label = 1.0 if CBSS beat aligns with ground truth (±70ms), 0.0 otherwise. Requires simulating beat timing from the ODF — but only simple CBSS, not full Bayesian fusion.
- Tempo factor: Label = ground_truth_bpm / cbss_bpm (clipped to {0.5, 1.0, 2.0}). Requires a rough BPM estimate from ACF — simple autocorrelation, not full fusion.
- Phase offset: Label = signed distance from CBSS beat to nearest ground truth beat, in fractions of beat period.

This means we DO need a **lightweight CBSS/ACF simulator** — not the full signal chain, but enough to produce approximate beat times and BPM from the ODF. ~300-500 lines of Python (ACF + simple peak-picking + counter-based beat detection), not 2000+ lines.

**Projected resource usage:**

| Resource | Mel-spectrogram NN (current) | BeatSyncNN (proposed) | Savings |
|----------|-----|-----|---------|
| Inference time | 79-98ms @ 62.5 Hz | <0.5ms @ ~2 Hz | **99.5%** |
| Tensor arena | 96 KB | ~4 KB | **96%** |
| Context buffer | 13-27 KB (128-256 frames × 26) | ~1.8 KB (8 beats × 55) | **93%** |
| Model flash | 26-46 KB | ~8 KB | **75%** |
| Op resolver | 14 ops (Conv2D, Pad, SpaceToBatch, ...) | 2 ops (FullyConnected, Logistic) | **86%** |

**Firmware changes (3 files):**

1. **`SpectralAccumulator` (NEW, ~80 lines)** — Running mel band sum/count/max between beats. Reset at beat fire. ~200 bytes state.

2. **`BeatActivationNN.h` (MODIFY)** — Replace sliding mel-spectrogram window + per-frame CNN inference with beat history buffer + FC inference at beat time. Dramatically simpler: remove 96 KB arena, 256-frame context buffer, quantization loop. Add ~4 KB arena, 8-beat feature buffer, beat-time trigger.

3. **`AudioController.cpp` (MODIFY, ~40 lines)** — Each frame: `accumulator_.accumulate(melBands, odf)`. At beat fire: `accumulator_.getFeatures(out)`, slide into history, run NN, reset accumulator. Apply correction outputs:
   - `beat_confidence` → modulate `rhythmStrength` and pulse suppression
   - `tempo_factor` → call `switchTempo(bpm * factor)` when != 1.0, with hysteresis
   - `phase_offset` → add to PLL proportional correction term
   - `downbeat_prob` → feed into `control_.downbeat` and `control_.beatInMeasure`

The CBSS pipeline itself is unchanged, but AudioController now has a feedback path from NN outputs that corrects tempo, phase, and confidence at each beat.

#### Training Data Strategy

**Dataset size:** Tiny. ~400 tracks × ~200 beats/track = ~80K training examples. Each example is ~220 floats. **Total: ~70 MB** (vs 64 GB for mel-spectrogram chunks). Processes in minutes on GPU.

**Reusable assets (no changes needed):**
- Raw audio files (`/mnt/storage/blinky-ml-data/audio/`, ~400 tracks)
- 4-system consensus beat/downbeat labels (`/mnt/storage/blinky-ml-data/labels/consensus_v2/`)
- Mel extraction pipeline (`scripts/audio.py`, `firmware_mel_spectrogram_torch`) — already firmware-matched
- Audio augmentation (gain, noise, RIR, time-stretch) from `prepare_dataset.py`
- Mic calibration profiles (gain-aware augmentation)

**Previous "full signal chain simulator" plan (RESCOPED):** The earlier plan called for reimplementing the complete BandFlux → OSS → ACF → comb filter → Bayesian fusion → CBSS pipeline in Python (~2000 lines). The correction outputs require a **lightweight** subset: BandFlux ODF + simple ACF + counter-based beat detection (~300-500 lines). This is enough to produce approximate beat times and BPM estimates so we can compute correction labels (beat_confidence, tempo_factor, phase_offset).

**New components to build:**

1. **Python BandFlux ODF (~100 lines)** — Compute spectral flux from mel spectrograms. Simple: log-compress, band-weight, half-wave rectify.

2. **Lightweight CBSS simulator (~300 lines)** — Simple ACF peak-picking → BPM estimate → counter-based beat prediction. Does NOT need comb filter bank, Bayesian fusion, onset snap, PLL, or octave checks. Just enough to produce "where CBSS would place beats" for computing correction labels.

3. **Beat-aligned feature extractor (~200 lines)** — Segment mel spectrograms by ground truth beat times, compute per-beat summaries (avg_mel, peak_mel, odf_stats), group into sequences of 4-8 beats.

4. **Correction label generator (~100 lines)** — Compare CBSS-simulated beats against ground truth to produce per-beat labels: beat_confidence (aligned?), tempo_factor (octave error?), phase_offset (signed distance to nearest GT beat).

5. **`BeatSyncClassifier` model (~80 lines PyTorch)** — FC with multi-head output: correction outputs (beat_confidence, tempo_factor, phase_offset) + capability outputs (downbeat_prob). Add to `models/beat_cnn.py`.

6. **Modified training/export pipeline** — New Dataset class for beat-level features, multi-output loss (weighted combination of correction + capability losses), beat-level evaluation metrics (downbeat F1, octave correction accuracy, phase error reduction), simplified TFLite export.

**Training pipeline:**
1. Process training audio through existing mel extraction (GPU, existing code)
2. Compute BandFlux-style ODF from mel spectrograms (new, ~20 lines NumPy)
3. Run lightweight CBSS simulator → approximate beat times + BPM (new)
4. Segment mel spectrograms by simulated beats → per-beat feature vectors (new)
5. Compute correction labels by comparing simulated vs ground truth beats (new)
6. Group into sequences of 4-8 beats with correction + downbeat labels (new)
7. Apply augmentation at audio level (existing gain/noise/RIR/time-stretch code)
8. Train BeatSyncClassifier in PyTorch (multi-output loss)
9. Export to TFLite INT8 (<8 KB model)
10. Deploy in modified BeatActivationNN

**Phased implementation:**
- **Phase A (downbeat only):** Train with ground truth beat times as input segmentation. Downbeat output only (no correction). Validates the architecture and training pipeline. No CBSS simulator needed.
- **Phase B (+ beat confidence):** Add beat_confidence output. Simulated beats vs ground truth produces confidence labels. Lightweight CBSS simulator needed.
- **Phase C (+ tempo/phase correction):** Add tempo_factor and phase_offset outputs. Full correction feedback loop. Requires tuning the AudioController integration (hysteresis, clamp bounds, correction rates).

**Firmware validation:** Use existing `stream nn` command + `capture_nn_stream.py` to confirm Python mel bands match firmware. Additionally, compare Python CBSS simulator beat times against firmware beat times on reference tracks (via `stream on` beat events) to validate the simulator produces comparable error patterns.

#### Follow-up: ACF Features as Additional Input

**Status: DEFERRED** (address if tempo_factor correction from spectral features alone proves insufficient)

The beat-sync model's `tempo_factor` output learns octave errors from spectral patterns (kick/snare alternation, harmonic structure). If this isn't enough to solve the 135 BPM gravity well, add ACF-derived features to the per-beat feature vector:
- Top 3 ACF peak lags + normalized correlation values (6 floats)
- Comb filter bank peak BPM + confidence (2 floats)
- Current Bayesian posterior entropy (1 float)

These are already computed in AudioController and would add only 9 floats to the feature vector (55 → 64). The SpectralAccumulator would snapshot them at beat time. The model could then learn "the ACF says 68 BPM but the spectral pattern says alternating kick/snare → double it to 136 BPM." Same architecture, just richer input.

**Research context:**
- Krebs/Böck/Widmer 2016: Beat-synchronous downbeat RNN. Processes one 180-dim vector per beat (~2 Hz). Key insight: per-beat features are sufficient for meter tracking.
- Gkiokas et al. 2017: CNN Beat Activation Function for dancing robot on ARM Cortex-A8. Practical embedded beat tracking with HW constraints.
- No published TinyML beat tracking on Cortex-M class hardware exists (as of March 2026).

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

1. **Deterministic pipeline accuracy (~28% F1)** — BandFlux + CBSS is unreliable in mic-in-room conditions. Root causes: octave errors, phase drift, false beats during breakdowns, slow tempo adaptation. Priority 1 hybrid corrector (Phase B/C) addresses all four via NN feedback at beat time.

2. **Downbeat detection quality (F1 ~0.33)** — Only 2/4 consensus systems provide downbeat labels. No spectral analysis at beat time. Priority 1 (Phase A) provides learned downbeat classification from spectral content.

3. **~135 BPM gravity well** — Multi-factorial: training data BPM bias (33.5% at 120-140), Bayesian prior, comb filter harmonic structure, ACF sub-harmonic peaks. Priority 1 tempo_factor output (Phase C) learns octave correction from spectral patterns. ACF features can be added if spectral-only is insufficient.

4. **Phase alignment** — CBSS derives phase indirectly from a counter. All SOTA systems achieving >60% F1 use explicit phase tracking. Priority 1 phase_offset output (Phase C) provides learned phase correction.

5. **NN inference speed (RESOLVED)** — Mel-spectrogram models require 79-98ms at 62.5 Hz (8-10× over budget). Beat-synchronous approach runs <0.5ms at ~2 Hz, well within budget.

## SOTA Context (March 2026)

| System | Year | Beat F1 | Architecture | Notes |
|--------|:----:|:-------:|-------------|-------|
| BEAST | 2024 | **80.0%** | Streaming Transformer (9 layers, 1024 dim) | SOTA, too large for MCU |
| BeatNet+ | 2024 | ~78% | CRNN + 2-level particle filter (1500 particles) | Microphone-capable |
| Novel-1D | 2022 | 76.5% | 1D state space (jump-back reward) | 30x faster than 2D |
| RNN-PLP | 2024 | 74.7% | RNN + PLP oscillator bank | Zero-latency, lightweight |
| BTrack | 2012 | ~55% | ACF + CBSS (our baseline architecture) | Embedded-friendly |
| **Blinky (ours)** | 2026 | **~28%** | NN ODF + CBSS (mic-in-room, nRF52840) | No comparable embedded NN system exists |

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends that require 79ms+ on our hardware. The beat-synchronous approach (Priority 1) sidesteps this by running at beat rate (~2 Hz) on pre-accumulated spectral summaries, focusing on downbeat/meter — the areas where our deterministic pipeline has no solution — rather than trying to replace the ODF.

## Known Limitations

| Issue | Root Cause | Visual Impact | Next Step |
|-------|-----------|---------------|-----------|
| ~135 BPM gravity well | Multi-factorial (data bias, prior, comb harmonics) | **Medium** | P1 tempo_factor correction (Phase C) |
| Phase alignment limits F1 | CBSS derives phase indirectly | **High** | P1 phase_offset correction (Phase C) |
| Run-to-run variance | Room acoustics, ambient noise, AGC state | Requires 5+ runs | -- |
| DnB half-time detection | Both librosa and firmware detect ~117 vs ~170 | **None** | Acceptable for visuals |
| deep-ambience low F1 | Soft ambient onsets below threshold | **None** | Organic mode is correct |
| trap-electro low F1 | Syncopated kicks challenge causal tracking | **Low** | Energy-reactive acceptable |

## Closed Investigations (v28-v65)

All items below were A/B tested and showed zero or negative benefit, or proven infeasible. Removed from firmware in v64 unless noted.

- **Mel-spectrogram NN models (v4-v9)**: All architectures (standard conv, BN-fused, DS-TCN) exceed 79ms inference on Cortex-M4F @ 64 MHz. The v9 DS-TCN (designed for speed) measured 98ms due to INT8 ADD requantization overhead from residual connections. No mel-spectrogram architecture can fit the 10ms per-frame budget. Superseded by beat-synchronous approach (<0.5ms at ~2 Hz). NN=1 build flag and BeatActivationNN.h retained for beat-sync model.

- **Full Python signal chain simulator (March 2026)**: Originally proposed reimplementing the complete BandFlux → OSS → ACF → comb filter → Bayesian fusion → CBSS pipeline in Python (~2000 lines). Rescoped to lightweight CBSS simulator (~300 lines): BandFlux ODF + simple ACF + counter-based beat detection. Only needed for Phase B/C correction labels, not Phase A downbeat-only.

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
