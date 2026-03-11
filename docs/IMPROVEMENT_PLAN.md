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
- **Downbeat detection**: F1 ~0.33 offline. Current firmware samples the mel CNN downbeat head (BeatActivationNN channel 1 EMA at beat time) when `nnBeatEnabled=true`, but quality remains low. With NN disabled or low confidence, behavior falls back to counting beats modulo 4. No beat-synchronous spectral analysis.

Mel-spectrogram NN models that could improve ODF quality require 79-98ms inference — 8-10× over budget.

**Approach:** Accumulate spectral summaries between beats, run a hybrid classifier at beat time (~2 Hz) that both **corrects the deterministic pipeline** and **provides new capabilities** (downbeat, meter). The NN validates and corrects CBSS rather than passively layering on top of it.

**Architecture: Beat-Synchronous Spectral Accumulator + Hybrid Corrector**

```
SharedSpectralAnalysis (already runs every frame, no additional cost)
     │
     ├── rawMelBands_ (26 bands, 62.5 Hz, pre-compression, pre-whitening)
     │         │
     │    SpectralAccumulator (NEW, ~300 bytes firmware)
     │         │  Between beats: running sum + sum² + count + max of raw mel bands
     │         │  At beat fire: avg_mel[26], peak_mel[26], std_mel[26], duration → 79 floats
     │         │
     │    Beat history buffer (last 4-8 beats):
     │         │  beat_features[8][79] ≈ 2.5 KB
     │         │
     │    BeatSyncNN inference (at beat fire only, ~2 Hz):
     │         │  Input:  4 beats × 79 features = 316 floats
     │         │  Model:  316 → 48 → 24 → N outputs (FC layers)
     │         │  ~10,000 params, ~10 KB INT8, <0.5ms inference
     │         │
     │         ├── CORRECTION outputs → feed back into AudioController:
     │         │     beat_confidence  → suppress false beats (breakdowns)
     │         │     tempo_factor     → correct octave errors (×2, ×0.5, ×1)
     │         │     phase_offset     → nudge beat timing
     │         │
     │         └── NEW CAPABILITY outputs → AudioControl:
     │               downbeat_prob   → AudioControl.downbeat
     │               (beats_per_bar  → future: governs beatInMeasure wrapping, deferred)
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

**Per-beat feature vector (79 floats, raw mel only):**

| Feature | Size | Computation |
|---------|------|-------------|
| `avg_mel` | 26 | Mean raw mel bands over frames since last beat |
| `peak_mel` | 26 | Max raw mel bands over frames since last beat |
| `std_mel` | 26 | Std dev of raw mel bands (spectral stability measure) |
| `duration_frames` | 1 | Number of frames since last beat (encodes tempo implicitly) |

**Critical design decision: raw mel bands only.** The feature vector uses `rawMelBands_` (post-FFT, post-noise-subtraction, pre-compression, pre-whitening) — NOT `melBands_` (which passes through the soft-knee compressor and per-bin adaptive whitening). This decouples the NN from **47+ tunable firmware parameters** (compressor threshold/ratio/knee, whitening alpha, BandFlux gamma/band weights/thresholds, cooldown timing, etc.). Changes to the audio processing chain do NOT require retraining. Only the mel filterbank itself (FFT size, hop, sample rate, mel scale, band count) would require retraining — and these are fundamental constants that never change.

Previous design included `odf_stats[3]` (odf_mean, odf_max, onset_ratio) derived from BandFlux. These were removed because BandFlux output depends on 16+ tunable parameters (gamma, bandWeights, threshold, minOnsetDelta, etc.) — any firmware tweak would invalidate the training data. The `std_mel` feature replaces ODF stats with a parameter-free spectral variability measure that captures similar information (high std = transient activity, low std = sustained/silence).

**Model outputs (hybrid — correction + new capabilities):**

*Correction outputs (feed back into AudioController):*
- **Beat confidence (0-1)**: How likely this beat was a true beat. Low values during breakdowns suppress forced beats and reduce `rhythmStrength`. Addresses false beat problem.
- **Tempo factor (3-class softmax: p_half, p_correct, p_double)**: Octave correction signal. The label distribution is tri-modal ({0.5, 1.0, 2.0}), making this a classification problem rather than regression. A 3-class softmax with a confidence gate (apply correction only when max-class probability > 0.85) gives cleaner gradients and simpler hysteresis than continuous regression. The NN learns this from spectral patterns — at the correct tempo, beats alternate kick-heavy and snare-heavy spectral profiles. At half-time, every beat looks the same. Addresses octave error and 135 BPM gravity well.
- **Phase offset (-0.5 to 0.5)**: Fractional beat correction. If the spectral onset peak is consistently offset from the beat anchor, nudge the PLL. Addresses phase drift.

*New capability outputs:*
- **Downbeat probability (0-1)**: Identifies bar boundaries from spectral content changes (harmony shifts, bass patterns). Drives `AudioControl.downbeat`. Addresses downbeat F1 ~0.33.
- **Beats per bar (optional, deferred)**: Currently 4/4 assumed — `AudioControl.beatInMeasure` wraps at 4. A future NN output could learn 3/4, 6/8 from beat-level spectral patterns, changing the wrap modulus. Not in initial phases — 4/4 covers >95% of target content (EDM, pop, rock).

**How correction outputs integrate with CBSS:**
1. **Beat confidence < threshold** → AudioController reduces `rhythmStrength`, suppresses visual pulse. Does NOT stop the beat counter (CBSS continues tracking internally so it can recover quickly).
2. **Tempo factor != 1.0** → AudioController applies `switchTempo(bpm * tempoFactor)` with hysteresis to prevent oscillation: (a) require N consecutive beats (e.g. 3) with consistent tempo_factor class before triggering, (b) cooldown period after `switchTempo()` (e.g. 4 beats) during which tempo_factor is ignored, (c) replaces the existing shadow CBSS octave checker (which uses beat strength ratios) since the NN has spectral evidence.
3. **Phase offset** → Added to PLL's proportional correction term. Bounded by existing PLL clamp (±T/4 after warmup). Replaces or supplements the onset snap heuristic.

**Training the correction outputs requires CBSS simulation** (partial):
- Beat confidence: Label = 1.0 if CBSS beat aligns with ground truth (±70ms), 0.0 otherwise. Requires simulating beat timing from the ODF — but only simple CBSS, not full Bayesian fusion.
- Tempo factor: Label = 3-class one-hot (half/correct/double) derived from ground_truth_bpm / cbss_bpm ratio. Requires a rough BPM estimate from ACF — simple autocorrelation, not full fusion.
- Phase offset: Label = signed distance from CBSS beat to nearest ground truth beat, in fractions of beat period.

This means we DO need a **lightweight CBSS/ACF simulator** — not the full signal chain, but enough to produce approximate beat times and BPM from the ODF. ~300-500 lines of Python (ACF + simple peak-picking + counter-based beat detection), not 2000+ lines.

**Projected resource usage:**

| Resource | Mel-spectrogram NN (current) | BeatSyncNN (proposed) | Savings |
|----------|-----|-----|---------|
| Inference time | 79-98ms @ 62.5 Hz | <0.5ms @ ~2 Hz | **99.5%** |
| Tensor arena | 96 KB | ~4 KB | **96%** |
| Context buffer | 13-27 KB (128-256 frames × 26) | ~2.5 KB (8 beats × 79) | **91%** |
| Model flash | 26-46 KB | ~10 KB | **70%** |
| Op resolver | 14 ops (Conv2D, Pad, SpaceToBatch, ...) | 2 ops (FullyConnected, Logistic) | **86%** |

**Firmware changes (3 files):**

1. **`SpectralAccumulator` (NEW, ~100 lines)** — Running raw mel band sum/sum²/count/max between beats. Computes avg, peak, std at beat fire. Reset after extraction. ~300 bytes state (26 bands × 3 accumulators × 4 bytes + count).

2. **`BeatActivationNN.h` (MODIFY)** — Replace sliding mel-spectrogram window + per-frame CNN inference with beat history buffer + FC inference at beat time. Dramatically simpler: remove 96 KB arena, 256-frame context buffer, quantization loop. Add ~4 KB arena, 8-beat feature buffer, beat-time trigger.

3. **`AudioController.cpp` (MODIFY, ~40 lines)** — Each frame: `accumulator_.accumulate(rawMelBands_)`. At beat fire: `accumulator_.getFeatures(out)`, slide into history, run NN, reset accumulator. Apply correction outputs:
   - `beat_confidence` → modulate `rhythmStrength` and pulse suppression
   - `tempo_factor` → call `switchTempo(bpm * factor)` when != 1.0, with hysteresis
   - `phase_offset` → add to PLL proportional correction term
   - `downbeat_prob` → feed into `control_.downbeat` and `control_.beatInMeasure`

The CBSS pipeline itself is unchanged, but AudioController now has a feedback path from NN outputs that corrects tempo, phase, and confidence at each beat.

#### Feature Stability and Progressive Simplification

**The coupling problem:** If NN inputs depend on hand-tuned processing stages (compressor, whitening, BandFlux), every firmware parameter change invalidates the model and requires reprocessing training data and retraining. This is the #1 practical risk for maintainability.

**Solution: Raw mel bands as the stable interface.** `rawMelBands_` in `SharedSpectralAnalysis` are computed directly from the FFT magnitude spectrum via the mel filterbank — no compressor, no whitening, no thresholds. The only parameters that affect them are fundamental constants that never change:

| Parameter | Value | Ever changes? |
|-----------|-------|:---:|
| Sample rate | 16 kHz | No |
| FFT size | 256 | No |
| Hop size | 256 | No |
| Mel bands | 26 | No |
| Mel range | 60-8000 Hz | No |
| Mel scale | HTK | No |
| Log compression | `10*log10(x+1e-10)` | No |
| Window | Hamming (alpha=0.54) | No |

Everything downstream — compressor threshold/ratio/knee, whitening alpha, BandFlux gamma/band weights/thresholds, cooldown timing, onset delta — is **invisible to the NN**. The training mel extraction pipeline (`scripts/audio.py`) already matches these constants exactly.

**Parameters that become redundant with a trained NN (progressive removal candidates):**

The deterministic pipeline has **50+ hand-tuned parameters** across BandFlux, EnsembleFusion, tempo prior, octave disambiguation, and CBSS. Many exist to compensate for each other's limitations. If the NN provides reliable beat confidence, tempo correction, and phase correction, many become unnecessary:

| Category | Parameters | Count | NN replaces? |
|----------|-----------|:-----:|:---:|
| BandFlux detection | gamma, bandWeights[7], threshold, minOnsetDelta, onsetDeltaDecay | ~11 | Phase B: beat_confidence makes threshold tuning less critical |
| EnsembleFusion | minConfidence, cooldownMs, noiseGateLevel, noiseGateDecay | 4 | Phase B: NN confidence supersedes post-hoc gating |
| Tempo prior | center, width, strength, enabled | 4 | Phase C: tempo_factor replaces prior-based disambiguation |
| Octave disambiguation | densityOctave, densityMinPerBeat, densityMaxPerBeat, octaveCheck, octaveCheckBeats, octaveScoreRatio | 6 | Phase C: tempo_factor replaces heuristic octave checks |
| CBSS beat detection | cbssAlpha, cbssTightness, cbssContrast, beatConfidenceDecay, tempoSnapThreshold | 5 | Phase C: phase_offset + tempo_factor make CBSS tuning less sensitive |
| Onset snap / PLL | snaphyst, pllwarmup, proportionalGain, integralGain | 4 | Phase C: phase_offset replaces snap heuristic |
| **Total removable** | | **~34** | Progressive, A/B tested |

**Progressive simplification roadmap:**
1. **Phase A**: Train NN with full pipeline intact. BandFlux + CBSS still does all beat detection. NN adds downbeat only.
2. **Phase B**: NN provides beat_confidence. A/B test disabling: EnsembleFusion confidence gate, noise gate, BandFlux minOnsetDelta filter. If NN compensates → remove.
3. **Phase C**: NN provides tempo_factor + phase_offset. A/B test disabling: tempo prior, octave disambiguation, onset snap, shadow CBSS checker. If NN compensates → remove.
4. **Phase D (speculative)**: If NN correction is robust enough, consider replacing CBSS beat detection entirely with NN-driven beat decisions. The deterministic pipeline reduces to: raw mel → accumulate → NN → beat timing + downbeat + meter. ~10 parameters instead of 50+.

**The circular dependency:** The NN runs at beat fire — but CBSS fires the beats. If we simplify CBSS too aggressively, beat timing degrades, which degrades NN inputs. Two escape hatches:

1. **Simple onset trigger (preferred):** Replace CBSS with a trivial spectral flux threshold as the beat trigger. 1-2 parameters (threshold, minimum interval). The NN then classifies each trigger as real beat/false positive and provides timing correction. This breaks the dependency on all CBSS/fusion parameters while keeping beat-rate inference.

2. **Fixed-rate inference (fallback):** Run NN at fixed 4 Hz (every 250ms) regardless of beat detection. Slightly higher compute (~2× vs beat-rate) but completely decoupled from the deterministic pipeline. The NN output includes "was this a beat?" as an additional binary output.

Either approach allows aggressive simplification in Phase D without the NN depending on the system it's trying to replace.

#### Training Data Strategy

**Dataset size:** Tiny. ~400 tracks × ~200 beats/track = ~80K training examples. Each example is ~316 floats (4 beats × 79). **Total: ~100 MB** (vs 64 GB for mel-spectrogram chunks). Processes in minutes on GPU.

**Retraining requirement: NONE for audio chain changes.** Because the feature vector uses only raw mel bands (pre-compression, pre-whitening), changes to BandFlux parameters, compressor settings, whitening alpha, cooldown timing, etc. do NOT affect training data. Only changes to the 8 fundamental mel constants (sample rate, FFT size, hop, band count, mel range, mel scale, log compression, window) would require retraining — and these are architectural constants that have never changed.

**Reusable assets (no changes needed):**
- Raw audio files (`/mnt/storage/blinky-ml-data/audio/`, ~400 tracks)
- 4-system consensus beat/downbeat labels (`/mnt/storage/blinky-ml-data/labels/consensus_v2/`)
- Mel extraction pipeline (`scripts/audio.py`, `firmware_mel_spectrogram_torch`) — already firmware-matched
- Audio augmentation (gain, noise, RIR, time-stretch) from `prepare_dataset.py`
- Mic calibration profiles (gain-aware augmentation)

**Previous "full signal chain simulator" plan (RESCOPED):** The earlier plan called for reimplementing the complete BandFlux → OSS → ACF → comb filter → Bayesian fusion → CBSS pipeline in Python (~2000 lines). The correction outputs require a **lightweight** subset: simple spectral flux ODF + simple ACF + counter-based beat detection (~300-500 lines). This is enough to produce approximate beat times and BPM estimates so we can compute correction labels (beat_confidence, tempo_factor, phase_offset).

**New components to build:**

1. **Simple spectral flux ODF (~50 lines)** — Half-wave rectified difference of consecutive raw mel frames. Computed directly from already-extracted mel spectrograms (which `prepare_dataset.py` already produces) — no need to go back to raw audio. No BandFlux-specific parameters (gamma, band weights, thresholds). Parameter-free.

2. **Lightweight CBSS simulator (~300 lines)** — Simple ACF peak-picking → BPM estimate → counter-based beat prediction. Does NOT need comb filter bank, Bayesian fusion, onset snap, PLL, or octave checks. Just enough to produce "where a simple tracker would place beats" for computing correction labels.

3. **Beat-aligned feature extractor (~200 lines)** — Segment raw mel spectrograms by ground truth beat times, compute per-beat summaries (avg_mel, peak_mel, std_mel, duration_frames), group into sequences of 4-8 beats.

4. **Correction label generator (~100 lines)** — Compare CBSS-simulated beats against ground truth to produce per-beat labels: beat_confidence (aligned?), tempo_factor (octave error?), phase_offset (signed distance to nearest GT beat).

5. **`BeatSyncClassifier` model (~80 lines PyTorch)** — FC with multi-head output: correction outputs (beat_confidence, tempo_factor, phase_offset) + capability outputs (downbeat_prob). Add to `models/beat_cnn.py`.

6. **Modified training/export pipeline** — New Dataset class for beat-level features, multi-output loss (weighted combination of correction + capability losses), beat-level evaluation metrics (downbeat F1, octave correction accuracy, phase error reduction), simplified TFLite export.

**Training pipeline:**
1. Process training audio through existing mel extraction (GPU, existing code) — raw mel bands, no compression/whitening
2. Compute simple spectral flux ODF from raw mel frames (new, parameter-free)
3. Run lightweight CBSS simulator → approximate beat times + BPM (new)
4. Segment raw mel spectrograms by simulated beats → per-beat feature vectors (new)
5. Compute correction labels by comparing simulated vs ground truth beats (new)
6. Group into sequences of 4-8 beats with correction + downbeat labels (new)
7. Apply augmentation at audio level (existing gain/noise/RIR/time-stretch code)
8. Train BeatSyncClassifier in PyTorch (multi-output loss)
9. Export to TFLite INT8 (<10 KB model)
10. Deploy in modified BeatActivationNN

**Phased implementation:**
- **Phase A (downbeat only):** Train with ground truth beat times as input segmentation. Downbeat output only (no correction). Validates the feature pipeline and output heads. No CBSS simulator needed. **Caveat:** At inference time, features are segmented by CBSS-predicted beats (not GT), so the model sees systematically offset spectral windows. Phase A validates the architecture, not full system accuracy — expect noisier downbeat output on-device than offline eval suggests.
- **Phase B (+ beat confidence):** Add beat_confidence output. Simulated beats vs ground truth produces confidence labels. Lightweight CBSS simulator needed.
- **Phase C (+ tempo/phase correction):** Add tempo_factor and phase_offset outputs. Full correction feedback loop. Requires tuning the AudioController integration (hysteresis, clamp bounds, correction rates).

**Firmware validation:** Use existing `stream nn` command + `capture_nn_stream.py` to confirm Python mel bands match firmware. Additionally, compare Python CBSS simulator beat times against firmware beat times on reference tracks (via `stream on` beat events) to validate the simulator produces comparable error patterns.

#### Follow-up: ACF Features as Additional Input

**Status: DEFERRED** (address if tempo_factor correction from spectral features alone proves insufficient)

The beat-sync model's `tempo_factor` output learns octave errors from spectral patterns (kick/snare alternation, harmonic structure). If this isn't enough to solve the 135 BPM gravity well, add ACF-derived features to the per-beat feature vector:
- Top 3 ACF peak lags + normalized correlation values (6 floats)
- Comb filter bank peak BPM + confidence (2 floats)
- Current Bayesian posterior entropy (1 float)

These are already computed in AudioController and would add only 9 floats to the feature vector (79 → 88). The SpectralAccumulator would snapshot them at beat time. The model could then learn "the ACF says 68 BPM but the spectral pattern says alternating kick/snare → double it to 136 BPM." Same architecture, just richer input.

**Note:** ACF features re-introduce coupling to ~5 AudioController parameters (acfLagRange, combFilterBins, bayesianPrior). This is acceptable because these are stable architectural parameters, not tuning knobs that change frequently. But defer this until raw-mel-only proves insufficient.

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

**Key insight:** SOTA systems achieve 75-80% F1 with strong neural frontends that require 79ms+ on our hardware. The beat-synchronous approach (Priority 1) sidesteps this by running at beat rate (~2 Hz) on pre-accumulated raw spectral summaries. The NN both **corrects** the deterministic pipeline (beat confidence, tempo, phase) and **adds capabilities** it cannot provide (downbeat, meter). Using raw mel bands as the stable interface decouples the NN from 47+ tunable firmware parameters, enabling progressive simplification of the hand-tuned pipeline as the NN proves it can compensate.

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
