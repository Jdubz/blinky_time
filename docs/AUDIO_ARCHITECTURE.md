# Audio-Reactive Architecture (AudioController)

## Overview

AudioController provides unified audio analysis and rhythm tracking for LED effects. It combines microphone input processing with pattern-based beat detection to output a simple 4-parameter `AudioControl` struct.

**Current Version:** AudioController v3 with Multi-Hypothesis Tempo Tracking (January 2026)

**Evolution:**
- **v1 (2024)**: PLL-based phase tracking (unreliable with noisy transients)
- **v2 (December 2024)**: Single-hypothesis autocorrelation (robust but slow to adapt)
- **v3 (January 2026)**: Multi-hypothesis tracking (handles tempo changes and ambiguity)

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
EnsembleDetector (6 detectors + fusion) --> Transient Hits (visual only)
      |
OSS Buffer (6s @ 60Hz)
      |
      +--- Autocorrelation (every 500ms) --> Extract 4 Peaks
      |                                           |
      |                      Multi-Hypothesis Tracker (4 concurrent tempos)
      |                       ├── Primary (120 BPM, conf=0.85)
      |                       ├── Secondary (60 BPM, conf=0.52)  [half-time]
      |                       ├── Tertiary (inactive)
      |                       └── Candidate (inactive)
      |                                           |
      |                                  Promotion Logic (confidence-based)
      |                                           |
      +------------------------------------> Primary Hypothesis
                                                  |
                                         Tempo + Phase + Strength
                                                  |
AudioControl { energy, pulse, phase, rhythmStrength }
      |
Generators (Fire, Water, Lightning)
```

**Key Design Decisions:**

1. **Multi-Hypothesis Tracking**: Maintains 4 concurrent tempo interpretations to handle:
   - Tempo ambiguity (half-time vs full-time: 60 BPM vs 120 BPM)
   - Tempo changes (song transitions, tempo shifts)
   - Polyrhythmic patterns (multiple valid tempos)

2. **Transients → Pulse Only**: Transient detection drives visual pulse output, NOT beat tracking. Beat tracking is derived from buffered pattern analysis.

3. **Confidence-Based Promotion**: Non-primary hypotheses can be promoted to primary if they gain significantly higher confidence (>0.15 advantage) and have enough history (≥8 beats).

4. **Dual Decay Strategy**:
   - **Beat-count decay** during music: 32-beat half-life (phrase-aware)
   - **Time-based decay** during silence: 5-second half-life after 3s grace period

---

## Rhythm Tracking Algorithm

### Multi-Hypothesis Autocorrelation (v3 - Current)

**Every frame (~60 Hz):**
1. **Buffer OSS**: Store spectral flux (onset strength) in 6-second circular buffer
2. **Update All Hypotheses**: Advance phase, update confidence, apply decay for each active hypothesis
3. **Promotion Check**: Promote best non-primary hypothesis if significantly better

**Every 500ms:**
4. **Run Autocorrelation**: Compute correlation across all lags (60-200 BPM range)
5. **Extract Multiple Peaks**: Find up to 4 local maxima above threshold (0.3)
6. **Match or Create Hypotheses**:
   - For each peak: Check if BPM matches existing hypothesis (±5% tolerance)
   - If match: Update hypothesis strength and evidence
   - If no match: Create new hypothesis (evict LRU if slots full)

**Hypothesis Confidence Formula:**
```
confidence = 0.5 × strength           (autocorrelation peak strength)
           + 0.3 × consistency        (phase prediction accuracy)
           + 0.2 × longevity          (beat count / 32, capped at 1.0)
```

**Promotion Conditions:**
- Non-primary hypothesis has confidence > primary + 0.15
- Non-primary has tracked ≥8 beats (avoids spurious switching)
- Swap slots: new primary ← old secondary/tertiary/candidate

### Evolution: Why Multi-Hypothesis?

| Version | Approach | Strengths | Weaknesses |
|---------|----------|-----------|------------|
| **v1 (PLL)** | Event-driven phase locking | Low latency | Jitter from unreliable transients |
| **v2 (Single Autocorr)** | Pattern-based single tempo | Robust to noise | Slow adaptation (6s lag), tempo ambiguity |
| **v3 (Multi-Hypo)** | Multiple concurrent tempos | Handles changes & ambiguity | +1 KB RAM, +2% CPU |

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
| `phaseAdaptRate` | 0.15 | Phase adaptation speed (0-1) | `set phaseAdaptRate 0.15` |
| `bpmMin` | 60 | Minimum detectable BPM | `set bpmMin 60` |
| `bpmMax` | 200 | Maximum detectable BPM | `set bpmMax 200` |

### Multi-Hypothesis Tracker Parameters

Access via: `audioCtrl.getMultiHypothesis().<parameter>`

**Peak Detection:**
| Parameter | Default | Description |
|-----------|---------|-------------|
| `minPeakStrength` | 0.3 | Minimum normalized correlation to create hypothesis |
| `minRelativePeakHeight` | 0.7 | Peak must be >70% of max peak to be considered |

**Hypothesis Matching:**
| Parameter | Default | Description |
|-----------|---------|-------------|
| `bpmMatchTolerance` | 0.05 | ±5% BPM tolerance for matching peaks to existing hypotheses |

**Promotion:**
| Parameter | Default | Description |
|-----------|---------|-------------|
| `promotionThreshold` | 0.15 | Confidence advantage needed to promote (0-1) |
| `minBeatsBeforePromotion` | 8 | Minimum beats before promoting a new hypothesis |

**Decay:**
| Parameter | Default | Description |
|-----------|---------|-------------|
| `phraseHalfLifeBeats` | 32.0 | Half-life in beats during music (8 bars of 4/4) |
| `minStrengthToKeep` | 0.1 | Deactivate hypotheses below this strength |
| `silenceGracePeriodMs` | 3000 | Grace period before silence decay (ms) |
| `silenceDecayHalfLifeSec` | 5.0 | Half-life during silence (seconds) |

**Confidence Weighting:**
| Parameter | Default | Description |
|-----------|---------|-------------|
| `strengthWeight` | 0.5 | Weight of periodicity strength in confidence |
| `consistencyWeight` | 0.3 | Weight of phase consistency in confidence |
| `longevityWeight` | 0.2 | Weight of beat count in confidence |

**Debug:**
| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `debugLevel` | SUMMARY | Hypothesis debug verbosity (OFF/EVENTS/SUMMARY/DETAILED) | `set hypodebug 2` |

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

| Component | RAM | CPU @ 64 MHz | Notes |
|-----------|-----|-------------|-------|
| AdaptiveMic + FFT | ~4 KB | ~4% | Microphone processing |
| EnsembleDetector | ~0.5 KB | ~1% | 6 detectors + fusion |
| OSS Buffer (360 floats) | 1.4 KB | - | 6 seconds @ 60 Hz |
| Autocorrelation buffer (200 floats) | 0.8 KB | - | Correlation storage |
| MultiHypothesisTracker (4 slots) | 0.24 KB | - | Hypothesis storage |
| Autocorrelation (multi-peak) | - | ~3% | Every 500ms |
| Hypothesis updates | - | ~1% | Every frame |
| **Total (v3)** | **~7 KB** | **~9-10%** | +1 KB, +3-4% vs v2 |

**Program Storage:**
- Baseline (no multi-hypo): ~168 KB
- With multi-hypothesis: ~172 KB (+4 KB code)

---

## Files

**Core Audio System:**
- `blinky-things/audio/AudioController.h` - Main controller class + multi-hypothesis structures
- `blinky-things/audio/AudioController.cpp` - Implementation (autocorrelation, hypothesis management)
- `blinky-things/audio/AudioControl.h` - Output struct definition
- `blinky-things/audio/EnsembleDetector.h` - 6-detector fusion system

**Input Processing:**
- `blinky-things/inputs/AdaptiveMic.h` - Microphone processing
- `blinky-things/inputs/SerialConsole.h` - Command interface (hypothesis commands)
- `blinky-things/inputs/SerialConsole.cpp` - handleHypothesisCommand()

---

## SerialConsole Commands

**Hypothesis Inspection:**
```bash
show hypotheses      # Show all 4 hypothesis slots
show hypo           # Alias for above
show primary        # Show primary hypothesis only
audio               # Show overall audio status + BPM
```

**Debug Control:**
```bash
set hypodebug 0     # OFF - no hypothesis debug
set hypodebug 1     # EVENTS - creation/promotion/eviction only
set hypodebug 2     # SUMMARY - primary status every 2s (default)
set hypodebug 3     # DETAILED - all hypotheses every 2s
get hypodebug       # Show current debug level
```

---

## Related Documentation

- `docs/AUDIO-TUNING-GUIDE.md` - Parameter tuning instructions
- `docs/MULTI_HYPOTHESIS_TRACKING_PLAN.md` - Multi-hypothesis design plan
- `docs/MULTI_HYPOTHESIS_OPEN_QUESTIONS_ANSWERED.md` - Design decisions
- `blinky-test-player/PARAMETER_TUNING_HISTORY.md` - Calibration test results
- `docs/GENERATOR_EFFECT_ARCHITECTURE.md` - Generator design patterns
