# Audio-Reactive Architecture (AudioTracker)

## Overview

AudioTracker provides unified audio analysis and rhythm tracking for LED effects. It combines microphone input processing with ACF tempo estimation and PLP (Predominant Local Pulse) phase/pattern extraction to output an `AudioControl` struct with 6 parameters.

**Current Version:** AudioTracker v93 (April 2026). Multi-source ACF + PLP with direct pattern interpolation.
**Period Detection:** Multi-source ACF (spectral flux + bass energy + NN onset, lags 20-80, ~4ms) → parabolic interpolation → bar multipliers (2×/3×/4×) → epoch-fold variance scoring with sqrt(multiplier) penalty. **Pattern quality is the objective — the system selects whichever period produces the best visual pattern. Half/double time matches are correct if they capture more rhythmic structure.**
**Phase/Pattern:** Epoch-fold ungated spectral flux (ossLinear_) at detected period → direct pattern interpolation (v91 refactor, cosine OLA removed). **Robust epoch-fold (b119+):** NN-confidence-weighted epochs, per-bin reliability (CV-based), Winsorized mean (outlier epoch rejection), cross-correlation with NN fold for pattern validation. Pattern quality is NN-independent. SuperFlux 3-wide max filter on spectral flux. plpNovGain=1.0, plpVarianceSens=0. Reliability metrics: plpMeanReliability_, plpNNAgreement_ exposed in debug stream.
**Onset Detection:** FrameOnsetNN (Conv1D W16, 16.5 KB INT8, ~7ms nRF52840). Single output: onset activation. Detects acoustic onsets (kicks/snares + broader transients), not metrical beats. **Local-maxima peak-picking** (b127+): `updatePulseDetection()` uses local-maxima peak-picking on NN activation (prevSignal > prev AND prevSignal > next AND > pulseOnsetFloor). Bass-band energy gate (50% threshold increase when bass ratio low). PLP pattern bias (30% threshold increase at off-beat positions, scaled by confidence). pulseOnsetFloor=0.30 (sweep-optimized). PLP beat-grid AND-gate `beatGridPatternMin` defaulted to **0.0 since b149** (was 0.4 in b142–b148; persist_raw analysis 2026-04-25 showed it suppressed 65–84% of true onsets — gtPatternCorr 0.86–0.99 confirmed PLP is reliable as PERIOD tracker but broken as PER-FRAME gate). v32 deployed (b149): on-device F1≈0.47 on edm_holdout (no-gate), 4 pp below madmom RNN's 0.51 ceiling on the same corpus. Also serves as one of 3 ACF sources. **On-device activations are NOT flat** — acoustic chain (speaker→room→mic) creates contrast clean audio lacks (dynRange=0.734 vs offline 0.125). Non-NN fallback: mic level.
**AGC:** Removed (v72). Hardware gain fixed at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization is sole dynamic range system.
**Primary Test Metrics:** plpAtTransient (pattern-onset alignment), plpAutoCorr (pattern periodicity), plpPeakiness (pattern structure). BPM accuracy uses octave-tolerant scoring.

**Evolution:**
- **v1 (2024)**: PLL-based phase tracking (unreliable with noisy transients)
- **v2 (December 2024)**: Single-hypothesis autocorrelation (robust but slow to adapt)
- **v3 (January 2026)**: Multi-hypothesis tracking (complex, phase jitter from competing corrections)
- **v4 (February 2026)**: CBSS beat tracking (deterministic phase, counter-based beats)
- **v5 (March 2026)**: AudioTracker — ACF+Comb+PLL, replaces AudioController CBSS. ~400 lines, ~10 params (vs ~2100 lines, ~56 params). No CBSS, no Bayesian fusion.
- **v6 (March 2026)**: PLP (Predominant Local Pulse) — replaced PLL with Fourier tempogram. Soft blend, cold-start template seeding, beat stability gated learning. PLL proven ineffective (phase consistency ~0.04).
- **v7 (March 2026)**: Pattern slot cache — 4-slot LRU of 16-bin PLP pattern digests for instant section recall. v77 pattern memory replaced. Fisher's g removed.
- **v8 (March 2026, deployed)**: Multi-source ACF replaces Fourier tempogram (75ms → ~4ms). Parabolic interpolation on ACF peaks. Bar multipliers (2×/3×/4×) with sqrt penalty. Pattern-quality-driven period selection — octave matches are valid.

---

## Output: AudioControl Struct

Generators receive a single struct with 6 parameters:

```cpp
struct AudioControl {
    float energy;         // Audio energy level (0-1)
    float pulse;          // Transient intensity (0-1)
    float phase;          // Beat phase position (0-1)
    float plpPulse;       // PLP pattern value at current phase (0-1)
    float rhythmStrength; // Confidence in rhythm (0-1)
    float onsetDensity;   // Smoothed onsets per second (0-10+)
};
```

| Parameter | Description | Use For |
|-----------|-------------|---------|
| `energy` | Smoothed overall level | Baseline intensity, brightness |
| `pulse` | Transient hits with beat context | Sparks, flashes, bursts |
| `phase` | Position in beat cycle (0=on-beat) | Pulsing, breathing effects |
| `plpPulse` | PLP extracted pattern value (epoch-folded) | Beat-synced pulsing, breathing animations |
| `rhythmStrength` | Periodicity confidence | Music mode vs organic mode |
| `onsetDensity` | Smoothed transients/second (EMA) | Content classification (dance=2-6, ambient=0-1) |

---

## Architecture

```
PDM Microphone
      |
AdaptiveMic (fixed gain + window/range normalization, AGC removed v72)
      |
SharedSpectralAnalysis (FFT-256 -> compressor -> whitening -> mel bands + spectral flux)
      |
      +--- [BPM PATH: spectral flux → tempo estimation]
      |    Spectral Flux (HWR: sum of positive magnitude changes, NN-independent)
      |         |
      |    Contrast sharpening (flux^2.0)
      |         |
      |    +--- OSS Buffer (~5.5s @ ~66 Hz, 360 samples, circular)
      |    |
      |    +--- Autocorrelation (every 150ms)
      |              Peak-finding → period estimate
      |              EMA smoothing -> BPM estimate
      |
      +--- [ONSET PATH: NN → local-maxima peak-picking (b127)]
      |    FrameOnsetNN (Conv1D W16, onset-only, v25 deployed)
      |         Input: sliding window of rawMelBands_ (16 frames x 26 bands)
      |         Output: single onset activation (0-1, kicks/snares)
      |         |
      |    Local-maxima peak-picking (prevSignal > prev AND > next AND > floor)
      |    + Bass-band energy gate + PLP pattern bias
      |         |
      |    +--- Pulse: local-maxima detection with FP suppression (visual sparks)
      |    +--- Energy: hybrid (mic level + bass mel energy + onset peak-hold)
      |
      +--- [PHASE PATH: PLP → direct pattern interpolation (v91+, robust b119+)]
           Epoch-fold ungated spectral flux (ossLinear_) at ACF-detected period
           SuperFlux 3-wide frequency max filter (Bock 2013)
           Robust epoch-fold (b119+): NN-confidence-weighted epochs, per-bin
             reliability (CV-based), Winsorized mean, NN cross-correlation validation
           Direct pattern interpolation at current cycle position → plpPulse
           Phase = patternPosition - accentPhase (data-driven, no oscillator)
           Pattern quality is NN-independent (decoupled v93)
           Cold-start template seeding (8 patterns, cosine similarity > 0.50)
           Pattern slot cache: 4-slot LRU of 16-bin digests (instant section recall)
           Reliability metrics: plpMeanReliability_, plpNNAgreement_ in debug stream
           Silence state reset after 5s (clears all analysis buffers)
                |
AudioControl { energy, pulse, phase, plpPulse, rhythmStrength, onsetDensity }
      |
Generators (HeatFire, Water, PlasmaGlobe)
```

**Key Design Decisions:**

1. **Decoupled BPM and Onset Paths**: BPM estimation uses spectral flux (NN-independent), not NN onset activation. The NN detects acoustic onsets (kicks/snares) but cannot distinguish on-beat from off-beat transients — syncopated kicks, hi-hats, and off-beat snares would corrupt ACF periodicity if used for tempo. Spectral flux is a raw broadband transient signal that preserves periodic structure.

2. **Multi-Source ACF Period Detection (deployed)**: ACF scans beat-level lags (20-80) on 3 mean-subtracted sources (spectral flux, bass energy, NN onset). Parabolic interpolation refines peak position and strength. Bar multipliers (2×/3×/4×) generate candidates from beat peaks. Epoch-fold variance scoring with sqrt(multiplier) penalty selects the period producing the best visual pattern.

3. **Pattern Quality Over BPM Accuracy**: The system is a visualizer, not a BPM detector. Half/double time periods are correct if they capture more rhythmic structure (e.g., a 2-bar kick-snare-kick-snare pattern at half BPM is better than a 1-bar kick pattern at the "true" BPM). Test metrics focus on plpAtTransient, plpAutoCorr, and plpPeakiness — not BPM accuracy.

4. **PLP Pattern Extraction (v91+, robust b119+)**: Epoch-fold ungated spectral flux (ossLinear_) at ACF-detected period. Direct pattern interpolation at current cycle position (cosine OLA removed v91). Phase derived from position offset by accent phase. Pattern quality is NN-independent (decoupled v93). **Robust epoch-fold (b119+):** NN-confidence-weighted epochs (higher-confidence epochs contribute more), per-bin reliability scoring (CV-based, suppresses noisy bins), Winsorized mean (outlier epoch rejection), cross-correlation with NN fold for pattern validation. Reliability metrics plpMeanReliability_ and plpNNAgreement_ exposed in debug stream. gtPatternCorr metric shows extraction accuracy of 0.84-0.97 on test tracks. Cold-start template seeding (8 patterns). Pattern slot cache (4-slot LRU of 16-bin digests) for instant section recall.

5. **NN Onset → Local-Maxima Peak-Picking + ACF Source**: NN activation is peak-picked via local-maxima detection (b127+). Bass-band energy gate and PLP pattern bias suppress false positives from non-percussive spectral changes. PLP beat-grid AND-gate disabled by default since b149 — see §"NN Onset Activation as PLP Source" caveat. NN also serves as one of 3 ACF sources. It does not directly determine period — ACF finds periodic structure across all 3 sources. On-device activations are dynamic (dynRange=0.734) despite being flat offline (dynRange=0.125) — the acoustic chain creates contrast. **Current on-device F1≈0.47 on edm_holdout** (v32 b149, no gate); madmom RNN ceiling on the same corpus is 0.51, so the gap is small. Pre-2026-04-25 docs quoted F1=0.628 — that was on training-contaminated edm/ (18 tracks all in-corpus), not held-out.

---

## Rhythm Tracking Algorithm

### Multi-Source ACF + PLP (v8 - Currently Deployed)

> **NOTE:** The Fourier tempogram (Goertzel DFT, 75ms) was replaced by multi-source ACF (~4ms) in March 2026. ACF scans beat-level lags on 3 sources, bar multipliers generate longer candidates, and epoch-fold variance scoring selects the best visual pattern period. BPM accuracy uses octave-tolerant scoring — half/double time matches are valid.

**Every frame (~62.5 Hz, on new spectral frame):**
1. **NN Inference**: Feed mel bands to Conv1D W16 model → onset activation (primary pulse signal + PLP source)
2. **Pulse Detection**: Local-maxima peak-picking on NN activation (prevSignal > prev AND > next AND > pulseOnsetFloor). Bass-band energy gate (50% threshold increase when bass ratio low). PLP pattern bias (30% threshold increase at off-beat positions, scaled by confidence).
3. **Onset Gate** (PLP path only): Suppress onset below `odfGateThreshold` (prevent noise-driven false patterns)
4. **Spectral Flux**: Half-wave rectified magnitude change from SharedSpectralAnalysis (NN-independent)
5. **Flux Contrast**: Power-law sharpening (`flux^2.0`) to sharpen transient peaks
6. **Buffer OSS**: Store contrast-enhanced spectral flux in ~5.5s circular buffer (360 samples)
7. **Phase Advance**: Free-running sawtooth phase at PLP-selected period

**Every 150ms (acfPeriodMs):**
8. **Linearize OSS**: Copy circular spectral flux buffer to linear array with mean subtraction
9. **Compute ACF**: Autocorrelation at lags corresponding to bpmMin-bpmMax range
10. **Peak Selection**: Best lag -> candidate period
11. **Smooth Update**: EMA filter on BPM (only when change > 5%)

**PLP Update (every 150ms):**
12. **Mean-subtract sources**: Spectral flux, bass energy, NN onset buffers each mean-subtracted
13. **ACF period detection**: Multi-source ACF at beat-level lags (20-80), parabolic interpolation on peaks
14. **Period Selection**: Best ACF peak across 3 sources, bar multipliers (2x/3x/4x) with sqrt penalty
15. **Epoch-fold Pattern**: Fold ungated spectral flux (ossLinear_) at selected period
16. **Robust epoch-fold (b119+)**: NN-confidence-weighted epochs (higher-confidence epochs contribute more), per-bin reliability (CV-based, suppresses noisy bins), Winsorized mean (outlier epoch rejection), cross-correlation with NN fold for pattern validation
17. **Pattern Normalization**: Min-max normalization (signal is mean-subtracted, may have negatives)
18. **Confidence**: ACF peak strength x signal presence (steep mic level gate)
19. **Phase**: Derived from pattern position offset by accent phase (no oscillator)

### Evolution: Why ACF+Comb, and Why PLL Failed

| Version | Approach | Strengths | Weaknesses |
|---------|----------|-----------|------------|
| **v1 (PLL)** | Event-driven phase locking | Low latency | Jitter from unreliable transients |
| **v2 (Single Autocorr)** | Pattern-based single tempo | Robust to noise | Slow adaptation, tempo ambiguity |
| **v3 (Multi-Hypo)** | Multiple concurrent tempos | Handles ambiguity | Competing corrections cause phase jitter |
| **v4 (CBSS)** | Cumulative beat strength | Deterministic phase, no jitter | Complex (~2100 lines, ~56 params). Removed in v75. |
| **v5 (ACF+Comb+PLL)** | ACF tempo + comb validation + PLL phase | Simple (~400 lines, ~10 params), smooth phase | **PLL phase consistency ~0.04 (random)** — onset-gated corrections insufficient |
| **v8 (ACF+PLP, deployed)** | Multi-source ACF + PLP epoch-fold | ACF selects period, epoch-fold extracts pattern, 3 sources | atTransient 0.37-0.48, autoCorr up to +0.93 |

---

## Generator Usage

### Basic Usage

```cpp
void Generator::update(float dt, const AudioControl& audio) {
    if (audio.rhythmStrength > 0.3f) {
        // Beat-synced behavior
        if (audio.pulse > 0.5f) {
            burstSparks();
        }
        float breathe = audio.phaseToPulse();  // PLP pattern value at current phase
        setBrightness(breathe);
    } else {
        // Organic behavior (no rhythm detected)
        updateRandom(dt);
    }
}
```

### Phase Patterns

```cpp
// PLP extracted pattern pulse (actual repeating energy shape)
float breathe = audio.phaseToPulse();  // == audio.plpPulse

// Raw phase (0.0 -> 1.0 over one beat cycle)
float sawPhase = audio.phase;
```

### Hybrid Mode

```cpp
// Blend between organic and beat-synced based on confidence
float organic = randomPulse();
float synced = audio.phaseToPulse();
float blend = audio.rhythmStrength;
float output = organic * (1.0f - blend) + synced * blend;
```

---

## Tuning Parameters

### Tempo Detection (AudioTracker)

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `bpmMin` | `bpmmin` | 15 | 10-120 | Minimum period frequency (15 = full-bar at 60 BPM) |
| `bpmMax` | `bpmmax` | 200 | 120-240 | Maximum detectable BPM |
| `tempoSmoothing` | `temposmooth` | 0.85 | 0.5-0.99 | BPM EMA smoothing (higher = slower) |

### Phase Tracking (PLP — Multi-Source ACF + Robust Epoch-Fold)

PLP uses multi-source ACF at beat-level lags (20-80) across 3 mean-subtracted sources (spectral flux, bass energy, NN onset). ACF peak selects period, epoch-fold extracts pattern. **Robust epoch-fold (b119+):** NN-confidence-weighted epochs, per-bin reliability (CV-based), Winsorized mean (outlier epoch rejection), cross-correlation with NN fold for pattern validation. Reliability metrics: plpMeanReliability_, plpNNAgreement_ exposed in debug stream. Soft blend: PLP pattern and cosine fallback blended continuously by confidence (no hard threshold). Cold-start template seeding (8 patterns, cosine similarity > 0.50) cuts warm-up from ~8 bars to ~2 bars. Beat stability gated learning provides fill/breakdown immunity. Pattern slot cache (v82): 4-slot LRU of 16-bin PLP pattern digests — cached PLP patterns recalled instantly when a previously-heard section returns (cosine similarity > 0.70). `plpActivation` parameter exists in settings but is vestigial (soft blend uses raw confidence). `plpSignalFloor` controls the steep mic-level gate (0→1 transition near noise floor).

### Rhythm Activation

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `activationThreshold` | `activationthreshold` | 0.3 | 0.0-1.0 | Soft gate: quadratic falloff below this threshold (no hard cutoff) |
| `odfGateThreshold` | `odfgate` | 0.25 | 0.0-0.5 | NN output floor gate (suppress noise) |

### Pulse/Energy Modulation — PLP phase-driven

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `pulseBoostOnBeat` | `pulseboost` | 1.3 | 1.0-3.0 | Pulse boost factor near beat |
| `pulseSuppressOffBeat` | `pulsesuppress` | 0.6 | 0.0-1.0 | Pulse suppress factor off-beat |
| `energyBoostOnBeat` | `energyboost` | 0.3 | 0.0-1.0 | Energy boost near predicted beats |

> These parameters use PLP phase for "on-beat" / "off-beat" determination. With PLP multi-source ACF providing meaningful phase output (atTransient 0.37-0.48), beat-proximity modulation is now functional.

### Other

| Parameter | Serial Name | Default | Description |
|-----------|-------------|---------|-------------|
| `acfPeriodMs` | N/A (code constant) | 150 | Autocorrelation update interval (ms) |
| `nnProfile` | `nnprofile` | off | Enable NN inference profiling output |

---

## Onset Detection: FrameOnsetNN (Conv1D W16, Deployed)

### Current: Single Conv1D Onset Model

`FrameOnsetNN` detects acoustic onsets (kicks, snares) from mel spectrograms. With a 144ms receptive field it can only detect local transients — it cannot distinguish on-beat from off-beat onsets. This is why BPM estimation uses spectral flux instead of NN output. The distinction is critical: beats are metrical grid positions (abstract, periodic), while onsets are acoustic transients (concrete, irregular). The NN detects onsets. v25 deployed (b127): KW F1=0.842, on-device F1=0.628. **On-device activations are NOT flat** — offline FP32 on clean audio: mean=0.567, std=0.051, dynRange=0.125 (flat); on-device INT8 on real audio: mean=0.432, std=0.250, dynRange=0.734 (dynamic). The acoustic chain (speaker→room→mic) creates contrast the clean audio lacks. The earlier "flat activation" diagnosis was based on offline analysis and was wrong for on-device. F1 plateau at 0.62 is from model precision (0.50) — model fires on broadband spectral changes (chords, synths, vocals). Mel filterbank corrected to match librosa HTK exactly (26/26 bands were wrong since day one, avg 4.2 INT8 level error). `MEL_DB_RANGE` extracted as constexpr in SharedSpectralAnalysis.h. Mel pipeline verified identical between training and firmware (MAE=0.0017, 0.44 INT8 levels).

The model processes a sliding window of raw mel frames (16 frames x 26 bands = 256ms) every spectral frame. Produces a single output: **onset activation** (used for visual pulse detection and as one of 3 PLP multi-source ACF sources). Uses raw mel bands (pre-compression, pre-whitening), decoupled from firmware signal processing parameters. Always compiled in (TFLite is a required dependency since v68).

**Architecture:** Conv1D onset-only model. 13.4 KB INT8 (per-tensor quantization). Single output channel (onset activation). Arena: 3404 bytes of 32768 allocated.

**Performance:** 6.8ms inference on Cortex-M4F @ 64 MHz (nRF52840), 5.8ms on ESP32-S3.

**Fallback:** If the model fails to load, `mic_.getLevel()` serves as a simple energy-based onset signal.

### Onset Information Gate

The NN onset activation passes through an information gate before use in PLP source input. When NN output is weak (below `odfGateThreshold`), the gate clamps the value to a low floor (0.02) to prevent noise-driven false pattern detection. This improves phase stability during silence and ambient passages. Note: the gate applies to the PLP ACF source path only — pulse detection uses local-maxima peak-picking on NN activation with bass-band energy gate and PLP pattern bias (b127+).

### Spectral Flux (BPM Signal)

BPM estimation uses band-weighted half-wave rectified spectral flux from `SharedSpectralAnalysis::getSpectralFlux()`. Computed from **compressed-but-not-whitened** magnitudes (after soft-knee compressor, before per-bin whitening) to preserve absolute transient contrast. Band weighting emphasizes rhythmically important frequencies:
- **Bass (bins 1-6, 62-375 Hz):** 50% weight — kicks, strongest periodic signal
- **Mid (bins 7-32, 437-2000 Hz):** 20% weight — vocals, pads, less rhythmic
- **High (bins 33-127, 2-8 kHz):** 30% weight — snares, hi-hats, transient markers

Peaks at broadband transients, zero during sustain. NN-independent.

### Pulse Detection

**`control_.pulse` (generators consume this):** Local-maxima peak-picking on NN activation (b127).
- **Local-maxima detection**: `updatePulseDetection()` fires when prevSignal > prev AND prevSignal > next AND prevSignal > pulseOnsetFloor (0.30, sweep-optimized).
- **Bass-band energy gate**: 50% threshold increase when bass ratio is low — suppresses false positives from non-percussive spectral changes (chord changes, synths, vocals).
- **PLP pattern bias**: 30% threshold increase at off-beat positions (scaled by PLP confidence) — leverages rhythmic context to suppress off-beat false positives.
- Decaying envelope (~165ms half-life)
- **On-device activations are NOT flat**: Offline FP32 on clean audio shows flat activations (mean=0.567, std=0.051, dynRange=0.125), but on-device INT8 on real audio is dynamic (mean=0.432, std=0.250, dynRange=0.734). The acoustic chain (speaker→room→mic) creates contrast the clean audio lacks. The earlier "flat activation" diagnosis was based on offline analysis and was wrong for on-device.
- **F1 ceiling on edm_holdout (clean held-out): ~0.47** (v32 b149, 25 tracks × 3 devices, no gate). Madmom RNN — gold-standard algorithmic onset detector — reaches F1=0.509 on the same corpus, so the gap is 4 pp; about 80% of the addressable headroom is closed. The bottleneck appears to be input representation (30 mel × 4 kHz fmax misses hi-hat/snare spectral content) — see ML_IMPROVEMENT_PLAN 2026-04-25 synthesis. v33 (in-flight 2026-04-25) tests 80 mel × 8 kHz fmax against this hypothesis.
- **Pre-2026-04-25 docs quoted F1=0.628 plateau** on `blinky-test-player/music/edm/`. That was a training-contaminated upper bound — all 18 of those tracks are *inside* the v27-hybrid training corpus (14 train, 4 val, 0 held out). The clean held-out number on edm_holdout (25 tracks, formally excluded from training since v30) is the F1≈0.47 above.
- **Detection-algorithm evolution:** b123 first-diff peak-picking over-detected (14.3/s vs 3.5/s GT) → b126 local-maxima → b127 +bass gate +PLP bias → **b142 PLP beat-grid AND-gate (regression: -0.21 F1)** → **b149 gate default flipped 0.4→0.0** restoring recall. The b142 gate is still a runtime-tunable, just disabled by default. See ML_IMPROVEMENT_PLAN "PLP-accuracy diagnosis".

**Discrete onset events (debug/serial, onset density):** Same local-maxima detection.
- **Cooldown**: Tempo-adaptive (40ms at 200 BPM, 150ms at 60 BPM)

### Energy Synthesis

Energy output is a hybrid blend of three sources:
- **Mic level** (30%): Raw microphone amplitude (broadband)
- **Bass mel energy** (30%): Low-frequency mel bands 1-6 (kick drum emphasis)
- **ODF peak-hold** (40%): Peak NN activation with ~100ms exponential decay

Beat-proximity boost is applied when rhythm is active: energy is increased near predicted beats by up to `energyBoostOnBeat` * `rhythmStrength`.

**Previous approaches (REMOVED):** BandFlux Solo detector (removed v67, ~2600 lines), EnsembleDetector/EnsembleFusion/BassSpectralAnalysis. Mel-spectrogram CNN (v4-v9, 79-98ms, too slow). See `IMPROVEMENT_PLAN.md` Closed Investigations.

---

## ESP32-S3 Platform Notes

### JTAG Pin Conflict (Esp32PdmMic)

On XIAO ESP32-S3 Sense, the PDM microphone is wired to GPIO42 (CLK) and GPIO41 (DATA). These are also JTAG strap pins (MTMS and MTDI). When compiled with `USBMode=hwcdc` (required for serial on ESP32 core 3.3.7+), the `USB_SERIAL_JTAG` peripheral can claim these pins via IO_MUX during boot. The I2S driver's `gpio_set_direction()` silently fails to override the JTAG mux, so `i2s_channel_read()` returns zero bytes -- the mic appears dead while `begin()` returns true.

**Fix:** `Esp32PdmMic::begin()` calls `gpio_reset_pin()` on GPIO42 and GPIO41 before I2S initialization. This disconnects the pins from the JTAG peripheral, resets them to GPIO function, and allows the I2S PDM driver to claim them. A verification read (500ms timeout) confirms data is flowing before returning success.

### Software Gain

ESP32-S3 has no hardware PDM gain register. `setGain()` applies a software linear multiplier to PCM samples in `poll()`. The firmware uses a fixed gain of 30 (vs nRF52840's hardware gain of 32).

---

## Resource Usage

| Component | RAM | CPU Time | Notes |
|-----------|-----|----------|-------|
| AdaptiveMic + FFT + spectral flux | ~4 KB + 4 bytes | ~2ms/frame | Fixed gain + window/range normalization. Spectral flux: 1 float (negligible). |
| FrameOnsetNN (Conv1D W16) | 3404 bytes arena + 1.7 KB window buffer | 6.8ms/frame (nRF52840) | 16 frames x 26 bands x 4 bytes = 1,664 bytes. 13.4 KB model in flash. |
| OSS Buffer (360 floats) | 1.4 KB | - | ~5.5 seconds @ ~66 Hz, circular. Fed by spectral flux. |
| ACF computation | ~1.1 KB stack | ~2ms every 150ms | Linearized buffer + correlation |
| PLP (ACF + epoch-fold) | ~2 KB | ~4ms every 150ms | Multi-source ACF at beat-level lags, epoch-fold pattern |
| Pulse + output | negligible | <0.1ms/frame | Simple arithmetic |
| **Total audio budget** | **~13 KB + 32 KB arena** | **~14ms/frame** | Well under 16.7ms frame budget (60 fps) |

**Compared to AudioController (v4, removed):**
- RAM: ~13 KB vs ~20 KB (saved ~7 KB, mainly from smaller NN window buffer)
- Code: ~400 lines vs ~2100 lines
- Parameters: ~10 vs ~56
- Inference: 6.8ms vs 27ms (W16 vs W64 model)

---

## Files

**Core Audio System:**
- `blinky-things/audio/AudioTracker.h` - Main tracker class: ACF + PLP (~10 tunable params)
- `blinky-things/audio/AudioTracker.cpp` - Implementation (autocorrelation, PLP multi-source ACF, pulse detection, output synthesis)
- `blinky-things/audio/AudioControl.h` - Output struct definition (6 fields)
- `blinky-things/audio/SharedSpectralAnalysis.h` - FFT -> compressor -> whitening -> mel bands
- `blinky-things/audio/FrameOnsetNN.h` - TFLite Micro NN onset activation (single Conv1D model)
- `blinky-things/audio/frame_onset_model_data.h` - INT8 TFLite model weights

**Input Processing:**
- `blinky-things/inputs/AdaptiveMic.h` - Microphone processing (fixed hardware gain)
- `blinky-things/inputs/SerialConsole.h` - Command interface
- `blinky-things/inputs/SerialConsole.cpp` - `registerTrackerSettings()`, `handleBeatTrackingCommand()`

**Platform HAL:**
- `blinky-things/hal/hardware/Esp32PdmMic.h` - ESP32-S3 PDM mic with JTAG pin conflict fix
- `blinky-things/hal/hardware/Esp32PdmMic.cpp` - `gpio_reset_pin()` fix, software gain, I2S PDM-RX driver

---

## SerialConsole Commands

**Beat Tracker Inspection:**
```bash
show beat           # Show AudioTracker state (BPM, phase, periodicity, PLP)
audio               # Show overall audio status + BPM
json beat           # JSON output of beat tracker state
json rhythm         # JSON output of rhythm tracking state
show nn             # Show NN model diagnostics (arena, window, inference time)
```

**Debug Control:**
```bash
debug rhythm on     # Enable rhythm debug output
debug rhythm off    # Disable rhythm debug output
debug transient on  # Enable transient detection debug
debug all off       # Disable all debug output
```

---

## Related Documentation

- `docs/AUDIO-TUNING-GUIDE.md` - Parameter tuning instructions
- `blinky-test-player/PARAMETER_TUNING_HISTORY.md` - Calibration test results
- `docs/GENERATOR_EFFECT_ARCHITECTURE.md` - Generator design patterns
- `docs/VISUALIZER_GOALS.md` - Design philosophy (visual quality over metrics)
