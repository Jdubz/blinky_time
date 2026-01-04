# Audio-Reactive Architecture (AudioController)

## Overview

AudioController provides unified audio analysis and rhythm tracking for LED effects. It combines microphone input processing with pattern-based beat detection to output a simple 4-parameter `AudioControl` struct.

**Replaced:** The original MusicMode class with PLL-based phase tracking has been replaced by autocorrelation-based rhythm tracking (December 2024).

---

## Output: AudioControl Struct

Generators receive a single struct with 4 parameters:

```cpp
struct AudioControl {
    float energy;         // Audio energy level (0-1)
    float pulse;          // Transient intensity (0-1)
    float phase;          // Beat phase position (0-1)
    float rhythmStrength; // Confidence in rhythm (0-1)
};
```

| Parameter | Description | Use For |
|-----------|-------------|---------|
| `energy` | Smoothed overall level | Baseline intensity, brightness |
| `pulse` | Transient hits with beat context | Sparks, flashes, bursts |
| `phase` | Position in beat cycle (0=on-beat) | Pulsing, breathing effects |
| `rhythmStrength` | Periodicity confidence | Music mode vs organic mode |

---

## Architecture

```
PDM Microphone
      |
AdaptiveMic (level, transient, spectral flux)
      |
OSS Buffer (6s @ 60Hz)
      |
      +--- Autocorrelation (every 500ms) --> Tempo + Phase
      |
      +--- Transient Detection --> Pulse (visual only)
      |
AudioControl { energy, pulse, phase, rhythmStrength }
      |
Generators (Fire, Color, etc.)
```

**Key Design Decision:** Transient detection drives VISUAL PULSE output only. Beat tracking is derived from buffered pattern analysis (autocorrelation), not from individual transient events. This prevents unreliable transients from disrupting beat sync.

---

## Rhythm Tracking Algorithm

### Autocorrelation-Based (Current)

1. **Buffer Onset Strength Signal (OSS)**: Store 6 seconds of spectral flux values (360 samples @ 60 Hz)

2. **Run Autocorrelation**: Every 500ms, compute autocorrelation of OSS buffer to find periodicity

3. **Find Tempo**: Peak lag in autocorrelation → BPM (constrained to 60-200 BPM range)

4. **Derive Phase**: Phase extracted from autocorrelation pattern, not from PLL

5. **Confidence**: Periodicity strength determines rhythmStrength (0-1)

### Why Not PLL?

The PLL-based approach was removed because:
- Required reliable transient events to lock phase
- Unreliable transients caused phase jitter
- Autocorrelation is more robust with noisy real-world audio

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

Exposed via SerialConsole:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `activationThreshold` | 0.4 | Periodicity strength to activate rhythm mode |
| `pulseBoostOnBeat` | 1.3 | Boost factor for on-beat transients |
| `pulseSuppressOffBeat` | 0.6 | Suppress factor for off-beat transients |
| `energyBoostOnBeat` | 0.3 | Energy boost near predicted beats |
| `phaseAdaptRate` | 0.15 | Phase adaptation speed (0-1) |
| `bpmMin` | 60 | Minimum detectable BPM |
| `bpmMax` | 200 | Maximum detectable BPM |

---

## Detection Modes (AdaptiveMic)

AudioController delegates transient detection to AdaptiveMic. Available modes:

| Mode | Name | Description |
|------|------|-------------|
| 0 | Drummer | Amplitude-based spike detection |
| 1 | Bass | Low-frequency band filter |
| 2 | HFC | High-frequency content emphasis |
| 3 | Spectral | FFT-based spectral flux |
| 4 | Hybrid | Combined drummer + spectral (best F1) |

Set via: `set detmode 4` (hybrid recommended)

---

## Resource Usage

| Component | RAM | CPU @ 60fps |
|-----------|-----|-------------|
| AdaptiveMic + FFT | ~4 KB | ~4% |
| OSS Buffer (360 floats) | 1.4 KB | - |
| Autocorrelation | - | ~2% (every 500ms) |
| **Total** | **~6 KB** | **~5-6%** |

---

## Files

- `blinky-things/audio/AudioController.h` - Main controller class
- `blinky-things/audio/AudioController.cpp` - Implementation
- `blinky-things/audio/AudioControl.h` - Output struct definition
- `blinky-things/inputs/AdaptiveMic.h` - Microphone processing
- `blinky-things/inputs/SpectralFlux.h` - FFT-based onset detection

---

## Related Documentation

- `docs/AUDIO-TUNING-GUIDE.md` - Parameter tuning instructions
- `blinky-test-player/PARAMETER_TUNING_HISTORY.md` - Calibration test results
- `docs/GENERATOR_EFFECT_ARCHITECTURE.md` - Generator design patterns
