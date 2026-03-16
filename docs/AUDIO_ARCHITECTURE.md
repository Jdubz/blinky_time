# Audio-Reactive Architecture (AudioController)

## Overview

AudioController provides unified audio analysis and rhythm tracking for LED effects. It combines microphone input processing with pattern-based beat detection to output an `AudioControl` struct with 6 parameters.

**Current Version:** AudioController with CBSS Beat Tracking + Frame-Level NN ODF (March 2026)
**ODF Source:** FrameBeatNN (frame-level FC, 56.8 KB INT8). Non-NN fallback: mic level.
**Planned:** Dual-model architecture — separate OnsetNN (short window, every frame) + RhythmNN (full-bar window with temporal pooling, every 4th frame). See `IMPROVEMENT_PLAN.md` for details.

**Evolution:**
- **v1 (2024)**: PLL-based phase tracking (unreliable with noisy transients)
- **v2 (December 2024)**: Single-hypothesis autocorrelation (robust but slow to adapt)
- **v3 (January 2026)**: Multi-hypothesis tracking (complex, phase jitter from competing corrections)
- **v4 (February 2026)**: CBSS beat tracking (deterministic phase, counter-based beats)

---

## Output: AudioControl Struct

Generators receive a single struct with 7 parameters:

```cpp
struct AudioControl {
    float energy;         // Audio energy level (0-1)
    float pulse;          // Transient intensity (0-1)
    float phase;          // Beat phase position (0-1)
    float rhythmStrength; // Confidence in rhythm (0-1)
    float onsetDensity;   // Smoothed onsets per second (0-10+)
    float downbeat;       // Beat-synchronized downbeat activation (0-1)
    uint8_t beatInMeasure; // Position in measure (1-4, 0=unknown)
};
```

| Parameter | Description | Use For |
|-----------|-------------|---------|
| `energy` | Smoothed overall level | Baseline intensity, brightness |
| `pulse` | Transient hits with beat context | Sparks, flashes, bursts |
| `phase` | Position in beat cycle (0=on-beat) | Pulsing, breathing effects |
| `rhythmStrength` | Periodicity confidence | Music mode vs organic mode |
| `onsetDensity` | Smoothed transients/second (EMA) | Content classification (dance=2-6, ambient=0-1) |
| `downbeat` | Beat-synchronized downbeat (v65, smoothed NN output) | Extra-dramatic effects on bar 1 |
| `beatInMeasure` | Position in measure (1-4, 0=unknown, v65) | Syncopation patterns, accent beats |

---

## Architecture

**Current (single model):**
```
PDM Microphone
      |
AdaptiveMic (level, transient, spectral flux)
      |
SharedSpectralAnalysis (FFT-256 → compressor → whitening → mel bands)
      |
      +--- FrameBeatNN (frame-level FC, ~60-200µs)
      |         Input: sliding window of rawMelBands_ (32 frames × 26 bands)
      |         Output: beat_activation (ODF for CBSS) + downbeat_activation
      |
OSS Buffer (6s @ 60Hz)
      |
      +--- Autocorrelation (every 250ms) --> Bayesian Tempo Fusion --> Best BPM
      |                                                                    |
      |                                                            beatPeriodSamples_
      |                                                                    |
      +--- CBSS Buffer → updateCBSS() → detectBeat() → Counter-based beats
      |                                        |
AudioControl { energy, pulse, phase, rhythmStrength, onsetDensity, downbeat, beatInMeasure }
      |
Generators (Fire, Water, Lightning)
```

**Planned (dual model):**
```
SharedSpectralAnalysis (FFT-256 → compressor → whitening → mel bands)
      |
      rawMelBands_ (26 bands, 62.5 Hz)
      |
      +--- OnsetNN (every frame, <1ms)
      |         Input: 8-16 frames × 26 mels (128-256ms)
      |         Output: onset_activation → OSS buffer (primary ODF) + AudioControl.pulse
      |
      +--- RhythmNN (every 4th frame, <8ms)
      |         Input: 192 frames × 26 mels (3.07s, 1.5+ bars)
      |         Output: beat_activation + downbeat_activation
      |
OSS Buffer (6s @ 60Hz)  ← fed by OnsetNN
      |
      +--- Autocorrelation (every 250ms) --> Bayesian Tempo Fusion --> Best BPM
      |                                                                    |
      +--- CBSS Buffer → updateCBSS() → detectBeat() → Counter-based beats
      |                                        |
AudioControl { energy, pulse, phase, rhythmStrength, onsetDensity, downbeat, beatInMeasure }
```

**Key Design Decisions:**

1. **CBSS Beat Tracking**: Cumulative Beat Strength Signal combines current onset strength with predicted beat history. Phase is derived deterministically from a counter — no drift, no jitter.

2. **Counter-Based Beat Detection**: Beats are expected at `lastBeat + period`. A search window around the expected time finds local maxima in the CBSS. Forced beats maintain phase during dropouts.

3. **Transients → Pulse Only**: Transient detection drives visual pulse output, NOT beat tracking. Beat tracking is derived from buffered pattern analysis.

4. **Tempo Prior**: Gaussian prior centered on 120 BPM helps disambiguate half-time/double-time autocorrelation harmonics.

---

## Rhythm Tracking Algorithm

### CBSS Beat Tracking (v4 - Current)

**Every frame (~60 Hz):**
1. **Buffer OSS**: Store onset strength in 6-second circular buffer
2. **Update CBSS**: `CBSS[n] = (1-alpha)*OSS[n] + alpha*max(CBSS[n-2T : n-T/2])`
3. **Detect Beat**: Search for local maximum in CBSS within window around expected beat time
4. **Derive Phase**: `phase = (sampleCounter - lastBeatSample) / beatPeriodSamples`

**Every 250ms:**
5. **Run Autocorrelation**: Compute correlation across all lags (60-200 BPM range), with inverse-lag normalization (`acf[i] /= lag`) to penalize sub-harmonics
6. **Apply Tempo Prior**: Weight autocorrelation by Gaussian prior to disambiguate harmonics
7. **Update Beat Period**: Convert best BPM to `beatPeriodSamples`

**Beat Detection Logic:**
- **Early/late window**: `[T*(1-windowScale), T*(1+windowScale)]` around expected beat
- **Detection**: CBSS local maximum above adaptive threshold within the window
- **Forced beat**: If past the late bound with no detection, force a beat (maintains phase during dropouts, reduces confidence)
- **Confidence**: Increases on real beats (+0.15), decreases on forced beats (*0.9), decays per-frame when no beat (*beatConfidenceDecay)

### Evolution: Why CBSS?

| Version | Approach | Strengths | Weaknesses |
|---------|----------|-----------|------------|
| **v1 (PLL)** | Event-driven phase locking | Low latency | Jitter from unreliable transients |
| **v2 (Single Autocorr)** | Pattern-based single tempo | Robust to noise | Slow adaptation, tempo ambiguity |
| **v3 (Multi-Hypo)** | Multiple concurrent tempos | Handles ambiguity | Competing corrections cause phase jitter |
| **v4 (CBSS)** | Cumulative beat strength | Deterministic phase, no jitter | Requires good onset detection |

---

## Generator Usage

### Basic Usage

```cpp
void Generator::update(float dt, const AudioControl& audio) {
    if (audio.hasRhythm()) {
        // Beat-synced behavior
        if (audio.pulse > 0.5f) {
            burstSparks();
        }
        float breathe = audio.phaseToPulse();  // 1.0 on-beat, 0.0 off-beat
        setBrightness(breathe);
    } else {
        // Organic behavior (no rhythm detected)
        updateRandom(dt);
    }
}
```

### Phase Patterns

```cpp
// Smooth breathing (1.0 on-beat, 0.0 off-beat)
float breathe = audio.phaseToPulse();

// Distance from beat (0.0 on-beat, 0.5 off-beat)
float offBeat = audio.distanceFromBeat();

// Raw phase (0.0 → 1.0 over one beat cycle)
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

### Core Rhythm Parameters (AudioController)

| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `activationThreshold` | 0.4 | Periodicity strength to activate rhythm mode | `set activationThreshold 0.4` |
| `pulseBoostOnBeat` | 1.3 | Boost factor for on-beat transients | `set pulseBoostOnBeat 1.3` |
| `pulseSuppressOffBeat` | 0.6 | Suppress factor for off-beat transients | `set pulseSuppressOffBeat 0.6` |
| `energyBoostOnBeat` | 0.3 | Energy boost near predicted beats | `set energyBoostOnBeat 0.3` |
| `bpmMin` | 60 | Minimum detectable BPM | `set bpmMin 60` |
| `bpmMax` | 200 | Maximum detectable BPM | `set bpmMax 200` |

### CBSS Beat Tracker Parameters

| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `cbssAlpha` | 0.9 | CBSS weighting (0.8-0.95, higher = more predictive) | `set cbssalpha 0.9` |
| `cbssTightness` | 8.0 | Log-Gaussian tightness (higher = stricter tempo) | `set cbsstight 8.0` |
| `beatConfidenceDecay` | 0.98 | Per-frame confidence decay when no beat | `set beatconfdecay 0.98` |
| `tempoSnapThreshold` | 0.15 | BPM change ratio to snap vs smooth | `set temposnap 0.15` |

### Tempo Prior Parameters

| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `tempoPriorCenter` | 120 | Center of Gaussian prior (BPM) | `set tempoprior_center 120` |
| `tempoPriorWidth` | 40 | Width (sigma) of prior | `set tempoprior_width 40` |
| `tempoPriorStrength` | 0.3 | Blend: 0=no prior, 1=full prior | `set tempoprior_strength 0.3` |
| `tempoPriorEnabled` | true | Enable tempo prior weighting | `set tempoprior_enabled 1` |

### Octave Disambiguation Parameters (v32)

| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `odfMeanSubEnabled` | false | ODF mean subtraction before ACF (disabled: raw ODF +70% F1) | `set odfmeansub 0` |
| `adaptiveOdfThresh` | false | Local-mean ODF threshold (BTrack-style, marginal benefit) | `set adaptodf 0` |
| `densityOctaveEnabled` | true | Onset-density octave penalty in Bayesian posterior | `set densityoctave 1` |
| `densityMinPerBeat` | 0.5 | Min plausible transients per beat | `set densityminpb 0.5` |
| `densityMaxPerBeat` | 5.0 | Max plausible transients per beat | `set densitymaxpb 5.0` |
| `octaveCheckEnabled` | true | Shadow CBSS octave checker (T vs T/2 comparison) | `set octavecheck 1` |
| `octaveCheckBeats` | 2 | Check octave every N beats | `set octavecheckbeats 2` |
| `octaveScoreRatio` | 1.3 | T/2 must score this much better to switch | `set octavescoreratio 1.3` |

---

## Detection: FrameBeatNN (Current) / Dual-Model NN (Planned)

### Current: Single FrameBeatNN

`FrameBeatNN` is the primary ODF source. It processes a sliding window of raw mel frames (32 frames × 26 bands) every spectral frame. Produces two outputs: **beat activation** (ODF for CBSS) and **downbeat activation** (drives `AudioControl.downbeat`). Follows the same paradigm as all leading beat trackers (BeatNet, Beat This!, madmom) — frame-level NN activation → post-processing — but uses FC layers instead of convolutions for Cortex-M4F feasibility (~3ms vs 79-98ms for CNNs). Uses raw mel bands (pre-compression, pre-whitening), decoupled from firmware signal processing parameters. Always compiled in (TFLite is a required dependency since v68).

**Architecture:** FC(832→64→32→2), 55K params, 56.8 KB INT8 (per-tensor quantization). Tensor arena ~2 KB, window buffer 3.2 KB.

**TFLite export note:** Must use per-tensor weight quantization (`_experimental_disable_per_channel=True`). CMSIS-NN FullyConnected kernel does not support per-channel quantization — causes constant zero output.

### Planned: Dual-Model NN

Two specialized models replace the single FrameBeatNN:

**OnsetNN** — Kick/snare detection for visual triggers
- Conv1D(26→24,k=3) → Conv1D(24→24,k=3) → Conv1D(24→1,k=1,sigmoid)
- Input: 8-16 mel frames (128-256ms), output: onset_activation (0-1)
- ~4 KB INT8, <1ms inference, runs every frame (62.5 Hz)
- Feeds OSS buffer as primary ODF and directly drives AudioControl.pulse

**RhythmNN** — Bar structure and downbeat detection
- Conv1D(26→32,k=5) → AvgPool(4) → Conv1D(32→48,k=5) → AvgPool(4) → Conv1D(48→32,k=3) → Conv1D(32→2,k=1)
- Input: 192 mel frames (3.07s = 1.5+ bars at all EDM tempos), output: beat + downbeat activation
- ~16 KB INT8, <8ms inference, runs every 2nd frame (31.25 Hz)
- Drives CBSS beat/downbeat tracking, AudioControl.downbeat, beatInMeasure

**Why two models:** Onset detection needs short windows with high temporal precision. Downbeat detection needs full-bar context. A single FC model with W192 (3.07s) regressed severely (F1=0.370 vs W32's 0.491) because FC flattening destroys temporal locality. Conv1D with temporal pooling (AvgPool1D) preserves local patterns while progressively compressing the time axis for bar-level classification.

### Pulse Detection (v67)

Pulse detection (for visual spark effects) is derived from the ODF signal directly in AudioController. Simple threshold against running mean with tempo-adaptive cooldown (min 40ms, max 150ms). Replaces the EnsembleFusion cooldown + noise gate removed in v67. With the dual-model architecture, OnsetNN output will replace this threshold-based approach.

**Previous approaches (REMOVED):** BandFlux Solo detector (removed v67, ~2600 lines), EnsembleDetector/EnsembleFusion/BassSpectralAnalysis. Mel-spectrogram CNN (v4-v9, 79-98ms, too slow), beat-synchronous hybrid (circular dependency, no discriminative signal). See `IMPROVEMENT_PLAN.md` Closed Investigations.

---

## Resource Usage

| Component | RAM | CPU @ 64 MHz | Notes |
|-----------|-----|-------------|-------|
| AdaptiveMic + FFT | ~4 KB | ~4% | Microphone processing |
| OSS Buffer (360 floats) | 1.4 KB | - | 6 seconds @ 60 Hz |
| ODF Linear Buffer (360 floats) | 1.4 KB | - | Linearized OSS for ACF (v32) |
| CBSS Buffer (360 floats) | 1.4 KB | - | Cumulative beat strength |
| Autocorrelation buffer | 0.8 KB | - | Correlation storage |
| CombFilterBank (20 filters) | ~5.3 KB | ~1% | Tempo validation (20 bins, 60-198 BPM) |
| Autocorrelation (250ms) | - | ~3% | Amortized |
| FrameBeatNN (current) | ~8-16 KB arena + 3.3 KB mel buffer | ~0.1-0.3% | FC model at ~15.6 Hz, ~60-200µs/inference |
| **Total (current)** | **~20 KB base** | **~15-20%** | +16 KB arena (FrameBeatNN) |

**Projected dual-model resource usage:**

| Component | RAM | CPU @ 64 MHz | Notes |
|-----------|-----|-------------|-------|
| OnsetNN (planned) | ~2 KB arena + 1.6 KB mel buffer | ~6% | Conv1D at 62.5 Hz, <1ms/inference |
| RhythmNN (planned) | ~8 KB arena + 19.5 KB mel buffer | ~12% amortized | Conv1D+Pool at 15.6 Hz, <8ms/inference |
| **Total (planned)** | **~31 KB** | **~25%** | Replaces FrameBeatNN row above |

---

## Files

**Core Audio System:**
- `blinky-things/audio/AudioController.h` - Main controller class + CBSS structures
- `blinky-things/audio/AudioController.cpp` - Implementation (autocorrelation, CBSS, beat detection)
- `blinky-things/audio/AudioControl.h` - Output struct definition
- `blinky-things/audio/SharedSpectralAnalysis.h` - FFT → compressor → whitening → mel bands (owned by AudioController since v67)
- `blinky-things/audio/FrameBeatNN.h` - TFLite Micro NN beat/downbeat activation
- `blinky-things/audio/frame_beat_model_data.h` - INT8 TFLite model weights

**Input Processing:**
- `blinky-things/inputs/AdaptiveMic.h` - Microphone processing
- `blinky-things/inputs/SerialConsole.h` - Command interface
- `blinky-things/inputs/SerialConsole.cpp` - handleBeatTrackingCommand()

---

## SerialConsole Commands

**Beat Tracker Inspection:**
```bash
show beat           # Show CBSS beat tracker state
audio               # Show overall audio status + BPM
json beat           # JSON output of beat tracker state
json rhythm         # JSON output of rhythm tracking state
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
