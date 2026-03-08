# Audio-Reactive Architecture (AudioController)

## Overview

AudioController provides unified audio analysis and rhythm tracking for LED effects. It combines microphone input processing with pattern-based beat detection to output a simple 4-parameter `AudioControl` struct.

**Current Version:** AudioController with CBSS Beat Tracking + NN Beat ODF + Forward Filter option (March 2026)

**Evolution:**
- **v1 (2024)**: PLL-based phase tracking (unreliable with noisy transients)
- **v2 (December 2024)**: Single-hypothesis autocorrelation (robust but slow to adapt)
- **v3 (January 2026)**: Multi-hypothesis tracking (complex, phase jitter from competing corrections)
- **v4 (February 2026)**: CBSS beat tracking (deterministic phase, counter-based beats)

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
      +--- EnsembleDetector (BandFlux Solo) --> ODF [default]
      |                                    --> Transient Hits (visual only)
      +--- BeatActivationNN (TFLite Micro) --> ODF [nnbeat=1, requires NN=1 build]
      |
OSS Buffer (6s @ 60Hz)
      |
      +--- Autocorrelation (every 500ms) --> Bayesian Tempo Fusion --> Best BPM
      |                                                                    |
      |                                                            beatPeriodSamples_
      |                                                                    |
      +--- [Default] CBSS Buffer → updateCBSS() → detectBeat() → Counter-based beats
      |                                                                    |
      +--- [fwdfilter=1] Forward Filter → updateForwardFilter() → Wrap-based beats
                              |                                            |
                    Joint tempo-phase                               Phase = position/period
                    forward algorithm
                              |
AudioControl { energy, pulse, phase, rhythmStrength }
      |
Generators (Fire, Water, Lightning)
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

**Every 500ms:**
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

## Detection: EnsembleDetector

AudioController delegates transient detection to the EnsembleDetector. Currently **BandFlux Solo** config (1 detector enabled):

| Detector | Weight | Threshold | Enabled | Role |
|----------|--------|-----------|---------|------|
| **BandWeightedFlux** | 1.00 | 0.5 | **Yes** | Log-compressed band-weighted spectral flux |
| Drummer | 0.50 | 4.5 | No | Good kick/snare recall |
| ComplexDomain | 0.50 | 3.5 | No | Good precision |

Design goal: trigger on **kicks and snares** only. Hi-hats and cymbals create overly busy visuals and are filtered out.

### NN Beat Activation (v54+ - Optional, requires NN=1 build)

Causal 1D CNN that replaces BandFlux as ODF source. Consumes raw mel bands from `SharedSpectralAnalysis::getRawMelBands()` (26 bands, 60-8000 Hz). Outputs beat activation (channel 0) and downbeat activation (channel 1). Toggle with `set nnbeat 1`.

**Architecture:** 5-layer dilated causal conv (dilations [1,2,4,8,16], channels=32). 15,330 params, 33.3 KB INT8. Receptive field: 63 frames (~1008ms at 66 Hz).

**Training:** 4-system consensus labels (Beat This! + essentia + librosa + madmom) across 6993 tracks with acoustic environment augmentation (volume, noise, reverb, RIR convolution) and mic profile augmentation. v4 model: Mean Beat F1=0.717, Downbeat F1=0.362.

**Hardware A/B test (March 2026):** NN wins 11/18 tracks vs BandFlux, mean error 14.8 vs 15.6. Best on techno-minimal-01 (0.6 vs 7.5 error). Both dominated by ~135 BPM gravity well in Bayesian tempo estimator.

**Key settings:**

| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `nnBeatEnabled` | false | Use NN ODF instead of BandFlux (requires NN=1 build) | `set nnbeat 1` |

### Forward Filter Beat Tracking (v57 - Optional)

Joint tempo-phase forward filter (Krebs/Böck/Widmer 2015). Toggle with `set fwdfilter 1`. Default OFF — full 6-parameter sweep (v60) optimized to 7/18 octave errors but still worse than CBSS baseline (4/18). Observation model is fundamentally octave-symmetric.

**How it works:** 20 tempo bins × variable phase positions (~700-880 states). Each frame:
1. Shift all phase positions forward by 1
2. Apply observation: beat-zone positions (first `period/λ`) get `λ * ODF`, others get `(1-ODF)/(λ-1)`
3. At position 0 (beat boundary): apply tempo transitions via Gaussian kernel
4. Normalize probabilities
5. Argmax determines current tempo and phase

**Beat detection:** When argmax position wraps from near period-1 to near 0, a beat is fired (with cooldown, silence gate, onset snap, PLL correction).

**Key settings:**

| Parameter | Default | Description | SerialConsole Command |
|-----------|---------|-------------|----------------------|
| `forwardFilterEnabled` | false | Enable forward filter (replaces CBSS+Bayesian for tempo/beats) | `set fwdfilter 1` |
| `fwdTransSigma` | 0.6 | Tempo transition width in lag units (v60 sweep optimal, was 3.0) | `set fwdtranssigma 0.6` |
| `fwdFilterContrast` | 1.0 | ODF power-law contrast (v60 sweep optimal, was 2.0) | `set fwdfiltcontrast 1.0` |
| `fwdFilterLambda` | 10.0 | Beat zone = 1/λ of period (v60 sweep optimal, was 8.0) | `set fwdfiltlambda 10.0` |
| `fwdFilterFloor` | 0.01 | Observation probability floor (prevents zero probabilities) | `set fwdfiltfloor 0.01` |
| `fwdBayesBias` | 0.2 | Bayesian posterior modulation strength (v59, 0=off, 1=full) | `set fwdbayesbias 0.2` |
| `fwdAsymmetry` | 0.8 | Asymmetric non-beat penalty by tempo (v60, 0=off, 0.8=optimal) | `set fwdasymmetry 0.8` |

---

## Resource Usage

| Component | RAM | CPU @ 64 MHz | Notes |
|-----------|-----|-------------|-------|
| AdaptiveMic + FFT | ~4 KB | ~4% | Microphone processing |
| EnsembleDetector | ~0.5 KB | ~1% | BandFlux Solo (1 detector) |
| OSS Buffer (360 floats) | 1.4 KB | - | 6 seconds @ 60 Hz |
| ODF Linear Buffer (360 floats) | 1.4 KB | - | Linearized OSS for ACF (v32) |
| CBSS Buffer (360 floats) | 1.4 KB | - | Cumulative beat strength |
| Autocorrelation buffer | 0.8 KB | - | Correlation storage |
| CombFilterBank (20 filters) | ~5 KB | ~1% | Tempo validation |
| Autocorrelation (500ms) | - | ~3% | Amortized |
| Forward filter (optional) | ~5.1 KB | ~1% | Joint tempo-phase (v57, `fwdfilter=1`) |
| NN beat activation (optional) | ~16 KB | ~2% | TFLite Micro tensor arena (NN=1 build, `nnbeat=1`) |
| **Total** | **~15-20 KB** | **~9-11%** | Base. +16 KB with NN, +5 KB with fwdfilter |

---

## Files

**Core Audio System:**
- `blinky-things/audio/AudioController.h` - Main controller class + CBSS structures
- `blinky-things/audio/AudioController.cpp` - Implementation (autocorrelation, CBSS, beat detection)
- `blinky-things/audio/AudioControl.h` - Output struct definition
- `blinky-things/audio/EnsembleDetector.h` - 2-detector ensemble fusion system
- `blinky-things/audio/BeatActivationNN.h` - TFLite Micro NN beat/downbeat activation (NN=1 build)
- `blinky-things/audio/beat_model_data.h` - INT8 TFLite model weights (v4, 33.3 KB)

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
