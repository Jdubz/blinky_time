# Audio-Reactive Architecture (AudioTracker)

## Overview

AudioTracker provides unified audio analysis and rhythm tracking for LED effects. It combines microphone input processing with ACF+Comb+PLL beat/tempo/phase tracking to output an `AudioControl` struct with 7 parameters.

**Current Version:** AudioTracker with ACF+Comb+PLL + Conv1D W16 NN ODF (March 2026)
**ODF Source:** FrameBeatNN (Conv1D W16, 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3). Single output: onset activation. Non-NN fallback: mic level.
**AGC:** Removed (v72). Hardware gain fixed at platform optimal (nRF52840: 32, ESP32-S3: 30). Window/range normalization is sole dynamic range system.

**Evolution:**
- **v1 (2024)**: PLL-based phase tracking (unreliable with noisy transients)
- **v2 (December 2024)**: Single-hypothesis autocorrelation (robust but slow to adapt)
- **v3 (January 2026)**: Multi-hypothesis tracking (complex, phase jitter from competing corrections)
- **v4 (February 2026)**: CBSS beat tracking (deterministic phase, counter-based beats)
- **v5 (March 2026)**: AudioTracker — ACF+Comb+PLL, replaces AudioController CBSS. ~400 lines, ~10 params (vs ~2100 lines, ~56 params). No CBSS, no Bayesian fusion.

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
    float downbeat;       // Reserved, always 0.0 (not wired)
    uint8_t beatInMeasure; // Reserved, always 0 (not wired)
};
```

| Parameter | Description | Use For |
|-----------|-------------|---------|
| `energy` | Smoothed overall level | Baseline intensity, brightness |
| `pulse` | Transient hits with beat context | Sparks, flashes, bursts |
| `phase` | Position in beat cycle (0=on-beat) | Pulsing, breathing effects |
| `rhythmStrength` | Periodicity confidence | Music mode vs organic mode |
| `onsetDensity` | Smoothed transients/second (EMA) | Content classification (dance=2-6, ambient=0-1) |
| `downbeat` | Reserved (always 0.0) | Not currently active |
| `beatInMeasure` | Reserved (always 0) | Not currently active |

**Note:** The `downbeat` and `beatInMeasure` fields exist in the struct for forward compatibility but are always zero. The system focuses exclusively on onset detection, BPM identification, and pulse/phase alignment.

---

## Architecture

```
PDM Microphone
      |
AdaptiveMic (fixed gain + window/range normalization, AGC removed v72)
      |
SharedSpectralAnalysis (FFT-256 -> compressor -> whitening -> mel bands)
      |
      +--- FrameBeatNN (Conv1D W16, onset-only)
      |         Input: sliding window of rawMelBands_ (16 frames x 26 bands)
      |         Output: single onset activation (0-1)
      |
      +--- ODF Information Gate (suppresses weak NN output)
      |
      +--- ODF Contrast (power-law sharpening, exponent 2.0)
      |
      +--- CombFilterBank (20 parallel IIR filters, Scheirer 1998)
      |         Independent tempo validation (60-198 BPM)
      |
      +--- OSS Buffer (6s @ ~66 Hz, circular)
      |
      +--- Autocorrelation (every 150ms)
      |         Percival harmonic enhancement (2nd+4th harmonics)
      |         Rayleigh prior weighting (peak at rayleighBpm)
      |         ACF primary tempo, comb bank validates (average when within 10%)
      |         EMA smoothing -> BPM estimate
      |
      +--- PLL (free-running sawtooth at estimated BPM)
      |         Onset-gated proportional + integral phase correction
      |
      +--- Pulse: floor-tracking baseline detection
      |
      +--- Energy: hybrid (mic level + bass mel energy + ODF peak-hold)
      |
AudioControl { energy, pulse, phase, rhythmStrength, onsetDensity, downbeat=0, beatInMeasure=0 }
      |
Generators (HeatFire, Water, Lightning)
```

**Key Design Decisions:**

1. **PLL Phase Tracking**: A free-running sawtooth ramp at the estimated BPM produces perfectly smooth phase output. Only corrected when strong onsets align near expected beats (within 25% of period). No CBSS, no Bayesian fusion, no predict/countdown.

2. **Dual Tempo Estimation**: ACF (autocorrelation) is the primary tempo estimator with Percival harmonic enhancement and Rayleigh prior weighting. CombFilterBank provides independent validation. When both agree within 10%, their average is used.

3. **Transients -> Pulse Only**: Transient detection drives visual pulse output, NOT beat tracking. Beat tracking is derived from buffered pattern analysis (ACF + comb bank).

4. **Tempo Prior**: Rayleigh distribution centered on `rayleighBpm` (default 140 BPM) weights ACF peaks to disambiguate half-time/double-time harmonics.

---

## Rhythm Tracking Algorithm

### ACF + Comb + PLL (v5 - Current)

**Every frame (~62.5 Hz, on new spectral frame):**
1. **NN Inference**: Feed mel bands to Conv1D W16 model, get onset activation (ODF)
2. **Pulse Detection**: Floor-tracking baseline, fire pulse when ODF exceeds baseline * 2.0
3. **ODF Gate**: Suppress ODF below `odfGateThreshold` (prevent noise-driven false beats)
4. **ODF Contrast**: Power-law sharpening (`odf^2.0`) to sharpen peaks relative to baseline
5. **Feed Comb Bank**: 20 parallel IIR comb filters (Scheirer 1998: `y[n] = (1-a)*x[n] + a*y[n-L]`)
6. **Buffer OSS**: Store contrast-enhanced ODF in 6-second circular buffer
7. **PLL Free-Run**: Advance sawtooth phase by `1/beatPeriodFrames` per frame

**Every 150ms (acfPeriodMs):**
8. **Linearize OSS**: Copy circular buffer to linear array with mean subtraction
9. **Compute ACF**: Autocorrelation at lags corresponding to bpmMin-bpmMax range
10. **Percival Enhancement**: Fold 2nd harmonic (ACF[2L], weight 0.5) and 4th harmonic (ACF[4L], weight 0.25) into fundamental lag L
11. **Rayleigh Weighting**: Weight each lag by `(L/sigma^2) * exp(-L^2/(2*sigma^2))` where sigma = lag at rayleighBpm
12. **Peak Selection**: Best weighted lag -> candidate BPM
13. **Comb Validation**: Compare ACF BPM with comb bank peak BPM. Average when within 10% agreement.
14. **Smooth Update**: EMA filter on BPM (only when change > 5%)

**PLL Correction (every frame, onset-gated):**
15. **Detect Strong Onset**: ODF > baseline * 2.0 and ODF > 0.1
16. **Phase Error**: Distance from nearest beat boundary, centered [-0.5, +0.5]
17. **Gate**: Only correct if |phase error| < 0.25 (onset near expected beat)
18. **Proportional**: Pull phase toward beat boundary by `pllKp * phaseError`
19. **Integral**: Leaky integrator (`0.95 * integral + phaseError`), correct by `pllKi * integral`

### Evolution: Why ACF+Comb+PLL?

| Version | Approach | Strengths | Weaknesses |
|---------|----------|-----------|------------|
| **v1 (PLL)** | Event-driven phase locking | Low latency | Jitter from unreliable transients |
| **v2 (Single Autocorr)** | Pattern-based single tempo | Robust to noise | Slow adaptation, tempo ambiguity |
| **v3 (Multi-Hypo)** | Multiple concurrent tempos | Handles ambiguity | Competing corrections cause phase jitter |
| **v4 (CBSS)** | Cumulative beat strength | Deterministic phase, no jitter | Complex (~2100 lines, ~56 params) |
| **v5 (ACF+Comb+PLL)** | ACF tempo + comb validation + PLL phase | Simple (~400 lines, ~10 params), smooth phase | Requires good onset detection |

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
| `bpmMin` | `bpmmin` | 60 | 40-120 | Minimum detectable BPM |
| `bpmMax` | `bpmmax` | 200 | 120-240 | Maximum detectable BPM |
| `rayleighBpm` | `rayleighbpm` | 140 | 60-180 | Rayleigh prior peak BPM (perceptual bias) |
| `combFeedback` | `combfeedback` | 0.92 | 0.85-0.98 | Comb bank resonance strength |
| `tempoSmoothing` | `temposmooth` | 0.85 | 0.5-0.99 | BPM EMA smoothing (higher = slower) |

### Phase Tracking (PLL)

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `pllKp` | `pllkp` | 0.15 | 0.0-0.5 | Proportional gain (phase correction speed) |
| `pllKi` | `pllki` | 0.005 | 0.0-0.05 | Integral gain (tempo adaptation speed) |

### Rhythm Activation

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `activationThreshold` | `activationthreshold` | 0.3 | 0.0-1.0 | Min periodicity to activate rhythm mode |
| `odfGateThreshold` | `odfgate` | 0.25 | 0.0-0.5 | NN output floor gate (suppress noise) |

### Pulse/Energy Modulation

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `pulseBoostOnBeat` | `pulseboost` | 1.3 | 1.0-3.0 | Pulse boost factor near beat |
| `pulseSuppressOffBeat` | `pulsesuppress` | 0.6 | 0.0-1.0 | Pulse suppress factor off-beat |
| `energyBoostOnBeat` | `energyboost` | 0.3 | 0.0-1.0 | Energy boost near predicted beats |

### Other

| Parameter | Serial Name | Default | Description |
|-----------|-------------|---------|-------------|
| `acfPeriodMs` | N/A (code constant) | 150 | Autocorrelation update interval (ms) |
| `nnProfile` | `nnprofile` | off | Enable NN inference profiling output |

---

## Detection: FrameBeatNN (Conv1D W16, Deployed)

### Current: Single Conv1D Onset Model

`FrameBeatNN` is the primary ODF source. It processes a sliding window of raw mel frames (16 frames x 26 bands = 256ms) every spectral frame. Produces a single output: **onset activation** (ODF for ACF/Comb/PLL tracking). Follows the same paradigm as leading beat trackers (BeatNet, Beat This!, madmom) -- frame-level NN activation followed by post-processing. Uses raw mel bands (pre-compression, pre-whitening), decoupled from firmware signal processing parameters. Always compiled in (TFLite is a required dependency since v68).

**Architecture:** Conv1D onset-only model. 13.4 KB INT8 (per-tensor quantization). Single output channel (onset activation). Arena: 3404 bytes of 32768 allocated.

**Performance:** 6.8ms inference on Cortex-M4F @ 64 MHz (nRF52840), 5.8ms on ESP32-S3.

**Fallback:** If the model fails to load, `mic_.getLevel()` serves as a simple energy-based ODF.

### ODF Information Gate

The NN onset activation passes through an information gate before entering the ACF/Comb pipeline. When NN output is weak (below `odfGateThreshold`), the gate clamps the ODF to a low floor value (0.02) to prevent noise-driven false tempo detection. This improves tracking during silence and ambient passages.

### Pulse Detection

Pulse detection (for visual spark effects) is derived from the raw ODF signal (before the information gate, so transient sensitivity is unaffected). Uses floor-tracking baseline detection:
- **Baseline tracking**: Slow rise (alpha=0.005), fast drop (alpha=0.05) -- peaks don't inflate baseline, but floor drops are caught quickly.
- **Threshold**: ODF must exceed baseline * 2.0 and mic level must exceed 0.03.
- **Cooldown**: Tempo-adaptive, shorter at faster tempos (40ms at 200 BPM, 150ms at 60 BPM).
- **Beat modulation**: When rhythm is active, on-beat pulses are boosted and off-beat pulses are suppressed.

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
| AdaptiveMic + FFT | ~4 KB | ~2ms/frame | Fixed gain + window/range normalization |
| FrameBeatNN (Conv1D W16) | 3404 bytes arena + 1.7 KB window buffer | 6.8ms/frame (nRF52840) | 16 frames x 26 bands x 4 bytes = 1,664 bytes. 13.4 KB model in flash. |
| OSS Buffer (360 floats) | 1.4 KB | - | 6 seconds @ ~66 Hz, circular |
| ACF computation | ~1.1 KB stack | ~2ms every 150ms | Linearized buffer + correlation |
| CombFilterBank (20 filters) | ~5.3 KB | ~1ms/frame | 20 x 66 delay line = 5,280 bytes + state |
| PLL + pulse + output | negligible | <0.1ms/frame | Simple arithmetic |
| **Total audio budget** | **~13 KB + 32 KB arena** | **~14ms/frame** | Well under 16.7ms frame budget (60 fps) |

**Compared to AudioController (v4, removed):**
- RAM: ~13 KB vs ~20 KB (saved ~7 KB, mainly from smaller NN window buffer)
- Code: ~400 lines vs ~2100 lines
- Parameters: ~10 vs ~56
- Inference: 6.8ms vs 27ms (W16 vs W64 model)

---

## Files

**Core Audio System:**
- `blinky-things/audio/AudioTracker.h` - Main tracker class: ACF + Comb + PLL (~10 tunable params)
- `blinky-things/audio/AudioTracker.cpp` - Implementation (autocorrelation, PLL, pulse detection, output synthesis)
- `blinky-things/audio/AudioControl.h` - Output struct definition (7 fields)
- `blinky-things/audio/CombFilterBank.h` - Comb filter bank header (20 parallel IIR resonators)
- `blinky-things/audio/CombFilterBank.cpp` - Comb filter bank implementation (Scheirer 1998)
- `blinky-things/audio/SharedSpectralAnalysis.h` - FFT -> compressor -> whitening -> mel bands
- `blinky-things/audio/FrameBeatNN.h` - TFLite Micro NN onset activation (single Conv1D model)
- `blinky-things/audio/frame_beat_model_data.h` - INT8 TFLite model weights

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
show beat           # Show AudioTracker state (BPM, phase, periodicity, comb bank)
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
