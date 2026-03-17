# AudioTracker Refactor Plan (Option A — ACF + Comb + PLL)

*Created: March 16, 2026*

## Motivation

The current AudioController is 2162 lines with ~56 tunable parameters, built around CBSS beat tracking with Bayesian tempo fusion. The Conv1D W64 NN model takes 27ms per frame — exceeding the 16.7ms budget for 60fps. The system works but is complex, fragile, and slow.

This refactor replaces AudioController with a dramatically simpler AudioTracker: a W16 onset-only NN (~7ms) feeding into ACF tempo estimation, comb filter validation, and a free-running PLL for smooth phase ramp. ~10 parameters. Total audio budget ~10ms. 60fps achievable.

### Design Philosophy

Focus on the three signals that matter most for visual quality:

1. **Phase ramp** (0→1 sawtooth per beat) — breathing, pulsing, color cycling
2. **Pulse** (onset peaks) — sparks, flashes, bursts on kicks/snares
3. **Energy** (overall level) — brightness, intensity

Downbeat and beatInMeasure are deferred to a future phase. The AudioControl struct keeps all 7 fields, but downbeat=0.0 and beatInMeasure=0. Generators degrade gracefully via organic mode fallback when these signals are absent.

### Architecture Overview

```
PDM Microphone (16 kHz)
    ↓
AdaptiveMic (fixed gain + window/range normalization)
    ↓
SharedSpectralAnalysis (FFT-256 → mel bands)
    ↓
FrameBeatNN (Conv1D W16, ~7ms, single channel)
    → onset activation (0-1, kicks/snares only)
    ↓
ODF Information Gate (suppress weak NN output)
    ↓
    ├── Pulse Detection (floor-tracking baseline)
    ├── OSS Buffer (6s circular, 360 samples @ 62.5 Hz)
    ├── Comb Filter Bank (20 IIR resonators, continuous tempo validation)
    │
    └── ACF (every 150ms)
         + Percival harmonic enhancement
         + Rayleigh prior weighting
         → candidate BPM
              ↓
         Tempo Selection (ACF primary, comb validates)
              ↓
         PLL (free-running sawtooth, onset-corrected)
              ↓
         Phase ramp (0→1, perfectly smooth)
    ↓
AudioControl { energy, pulse, phase, rhythmStrength, onsetDensity, downbeat=0, beatInMeasure=0 }
    ↓
Generators (Fire, Water, Lightning, HeatFire — unchanged)
```

---

## Dependency Graph

```
Phase 1 (Train W16 model) ──┐
                              ├──> Phase 3 (AudioTracker impl) ──> Phase 4 (Integration) ──> Phase 5 (Cleanup)
Phase 2 (Extract CombFilterBank) ┘
```

Phases 1 and 2 are independent and can proceed in parallel.

---

## Phase 1: Train W16 Onset-Only Model

### Goal

Train a Conv1D W16 single-channel model that detects kick/snare onsets. No downbeat output. Same backbone architecture as deployed W64 model but with shorter context window.

### Config: `ml-training/configs/conv1d_w16_onset.yaml`

```yaml
model:
  type: "frame_conv1d"
  channels: [24, 32]          # Same backbone as deployed W64
  kernel_sizes: [5, 5]        # Same kernels — RF = 9 frames = 144ms
  window_frames: 16           # 256ms context (was 64 = 1024ms)
  dropout: 0.1
  downbeat: false             # Single output channel (onset activation only)

export:
  output_header: "../blinky-things/audio/frame_beat_model_data.h"
  c_array_name: "frame_beat_model_data"
  max_model_size_kb: 10
```

### Key Details

- Architecture: Conv1D(26→24, k=5) → Conv1D(24→32, k=5) → Conv1D(32→1, k=1)
- ~5K params, ~5 KB INT8, estimated ~7ms inference on Cortex-M4F
- Receptive field: 9 frames = 144ms (captures full kick drum transient at 10-50ms)
- Training data: consensus_v5 labels (already processed, 141 GB dataset at `data/processed/`)
- `downbeat: false` → train.py automatically uses single-channel output
- FrameBeatNN auto-detects window size from model input shape — zero firmware changes needed

### Training Command

```bash
cd ml-training
tmux new-session -d -s training-w16 \
  "source venv/bin/activate && PYTHONUNBUFFERED=1 python train.py \
   --config configs/conv1d_w16_onset.yaml \
   --output-dir outputs/conv1d_w16_onset_v1 \
   2>&1 | tee outputs/conv1d_w16_onset_v1/training.log"
```

### Export

```bash
python scripts/export_tflite.py \
  --config configs/conv1d_w16_onset.yaml \
  --model outputs/conv1d_w16_onset_v1/best_model.pt
```

### Acceptance Criteria

- Offline beat F1 >= 0.40 (W64 baseline: 0.480 — shorter window will lose some context)
- Model size <= 10 KB INT8
- On-device: `show nn` confirms W16 window, inference < 10ms

### Risk: Low

The training pipeline already handles single-channel Conv1D models (onset_conv1d_v5 was trained previously). FrameBeatNN supports 1-4 output channels and auto-detects window size.

---

## Phase 2: Extract CombFilterBank

### Goal

Move the CombFilterBank class from AudioController.h (inline, lines 63-172) and AudioController.cpp (implementation, lines 2005-2162) into standalone files. Pure refactoring — no logic changes.

### New Files

- `blinky-things/audio/CombFilterBank.h` — class declaration
- `blinky-things/audio/CombFilterBank.cpp` — implementation (init, reset, process, extractPhase)

### Modified Files

- `blinky-things/audio/AudioController.h` — replace inline class with `#include "CombFilterBank.h"`
- `blinky-things/audio/AudioController.cpp` — remove CombFilterBank method implementations

### CombFilterBank API (Existing)

```cpp
class CombFilterBank {
public:
    static constexpr int NUM_FILTERS = 20;
    void init(float minBpm, float maxBpm, float frameRate);
    void reset();
    void process(float onsetStrength);
    void setFeedbackGain(float gain);  // 0.85-0.98
    float getPeakBPM() const;
    float getConfidence() const;
    float getPhase() const;
    float getFilterEnergy(int i) const;
    float getFilterBPM(int i) const;
};
```

### Acceptance Criteria

- `arduino-cli compile` succeeds with zero behavior changes
- Flash one device, audio behavior identical to pre-extraction firmware

### Risk: Very Low

CombFilterBank is self-contained with no dependencies on AudioController private state.

---

## Phase 3: Implement AudioTracker

### Goal

Create a new ~400-500 line AudioTracker class that replaces AudioController's 2162-line CBSS-based system. Same interface, dramatically simpler internals.

### New Files

#### `blinky-things/audio/IAudioSystem.h` — Abstract Interface

```cpp
class IAudioSystem {
public:
    virtual ~IAudioSystem() = default;
    virtual bool begin(uint32_t sampleRate = 16000) = 0;
    virtual void end() = 0;
    virtual const AudioControl& update(float dt) = 0;
    virtual const AudioControl& getControl() const = 0;
    virtual AdaptiveMic& getMicForTuning() = 0;
    virtual const AdaptiveMic& getMic() const = 0;
    virtual SharedSpectralAnalysis& getSpectral() = 0;
    virtual const SharedSpectralAnalysis& getSpectral() const = 0;
    virtual const FrameBeatNN& getFrameBeatNN() const = 0;
    virtual float getCurrentBpm() const = 0;
    virtual int getHwGain() const = 0;
};
```

Both AudioController and AudioTracker implement IAudioSystem. SerialConsole references `IAudioSystem*` — avoids `#ifdef` scatter throughout the codebase.

#### `blinky-things/audio/AudioTracker.h` + `AudioTracker.cpp`

### Core Algorithm

```
update(dt):
    // === Input processing (identical to AudioController) ===
    mic_.update(dt)
    spectral_.addSamples(mic_.getSamplesForExternal())
    spectral_.process()

    // === NN inference → onset strength ===
    odf = frameBeatNN_.infer(spectral_.getRawMelBands())  // W16, single channel

    // === ODF information gate ===
    if (odf < odfGateThreshold) odf = 0.02  // suppress noise-driven false beats

    // === Pulse detection (floor-tracking baseline) ===
    // Reuse existing logic from AudioController lines 183-214
    updatePulseDetection(odf)

    // === Feed DSP components ===
    combFilterBank_.process(odf)       // continuous tempo validation
    addOssSample(odf)                  // 6s circular buffer

    // === Periodic tempo estimation (every 150ms) ===
    if (now - lastAcfMs_ >= acfPeriodMs):
        runAutocorrelation()
        // ACF over linearized OSS buffer
        // + Percival harmonic enhancement (fold 2nd+4th harmonics)
        // + Rayleigh prior weighting (peaked at 140 BPM)
        // → candidate BPM from peak lag

    // === Tempo selection ===
    acfBpm = acfResult
    combBpm = combFilterBank_.getPeakBPM()
    agreement = abs(acfBpm - combBpm) / acfBpm < 0.10
    newBpm = acfBpm  // ACF is primary
    bpm_ = bpm_ * tempoSmoothing + newBpm * (1 - tempoSmoothing)  // EMA
    beatPeriodFrames_ = OSS_FRAME_RATE * 60.0 / bpm_

    // === PLL free-running sawtooth ===
    pllPhase_ += 1.0 / beatPeriodFrames_
    if (pllPhase_ >= 1.0):
        pllPhase_ -= 1.0   // beat wrap
        beatCount_++

    // === PLL onset correction (gated to near-beat region) ===
    if isStrongOnset(odf):
        phaseError = pllPhase_
        if phaseError > 0.5: phaseError -= 1.0  // center around 0: range [-0.5, +0.5]
        if abs(phaseError) < 0.25:  // onset within ±25% of expected beat
            pllPhase_ += pllKp * phaseError                       // proportional
            pllIntegral_ = 0.95 * pllIntegral_ + phaseError      // leaky integrator
            pllPhase_ += pllKi * pllIntegral_                     // integral

    // === Output synthesis ===
    control_.energy = 0.3*micLevel + 0.3*bassMelEnergy + 0.4*odfPeakHold
    control_.pulse = lastPulseStrength_ (with beat-proximity boost/suppress)
    control_.phase = pllPhase_          // perfectly smooth sawtooth
    control_.rhythmStrength = periodicityStrength (from ACF peak + comb agreement)
    control_.onsetDensity = smoothed pulses/second (EMA)
    control_.downbeat = 0.0             // not tracked in this version
    control_.beatInMeasure = 0          // not tracked in this version
```

### PLL Design (The Core Innovation)

The PLL replaces CBSS + predict/countdown + onset snap + beat-boundary deferral with a single free-running oscillator:

1. **Free-running sawtooth**: `phase += 1/T` each frame. Between corrections, the output is a perfectly smooth ramp. No jitter from counter-based phase derivation or onset snap discontinuities.

2. **Onset-gated correction**: Only correct PLL when a strong onset occurs AND phase is near the expected beat boundary (within ±25% of period). This prevents off-beat onsets (hi-hats, syncopation) from pulling the phase. The phase detector only responds to onsets that confirm the predicted beat timing.

3. **PI controller**: Proportional gain (Kp=0.15) corrects immediate phase offset. Integral gain (Ki=0.005) corrects persistent tempo drift. The leaky integrator (decay 0.95) prevents wind-up during silence.

4. **Silence behavior**: PLL free-runs at last known BPM. `rhythmStrength` decays (no new onset evidence). Generators automatically blend toward organic mode. When music resumes, ACF re-estimates tempo, PLL corrects. No explicit silence detection needed.

### Parameters (~10 total, vs current ~56)

| Name | Type | Default | Range | Purpose |
|------|------|---------|-------|---------|
| `bpmMin` | float | 60 | 40-120 | Minimum detectable BPM |
| `bpmMax` | float | 200 | 120-240 | Maximum detectable BPM |
| `rayleighBpm` | float | 140 | 60-180 | Rayleigh prior peak (perceptual tempo preference) |
| `combFeedback` | float | 0.92 | 0.85-0.98 | Comb bank IIR resonance strength |
| `pllKp` | float | 0.15 | 0.0-0.5 | PLL proportional gain (phase correction speed) |
| `pllKi` | float | 0.005 | 0.0-0.05 | PLL integral gain (tempo adaptation speed) |
| `activationThreshold` | float | 0.3 | 0.0-1.0 | Minimum periodicity to activate rhythm mode |
| `odfGateThreshold` | float | 0.25 | 0.0-0.5 | NN output floor gate |
| `acfPeriodMs` | uint16_t | 150 | 50-500 | ACF recomputation interval (ms) |
| `tempoSmoothing` | float | 0.85 | 0.5-0.99 | BPM EMA smoothing factor |

### What Gets Removed (vs AudioController)

| Removed Component | Params | RAM |
|-------------------|:------:|:---:|
| CBSS recursion + buffer + log-Gaussian weights | ~13 | 2.2 KB |
| Bayesian tempo fusion + transition matrix + prior | ~12 | 3.2 KB |
| Beat prediction (predict+countdown) + expectation window | ~3 | 0.7 KB |
| Onset snap + hysteresis | ~3 | 0.1 KB |
| Shadow CBSS octave checker | ~3 | 0.1 KB |
| Adaptive tightness | ~5 | — |
| Beat-boundary tempo deferral | ~1 | — |
| PLL warmup/clamp logic | ~3 | — |
| Downbeat tracking + beatInMeasure | ~3 | — |
| ODF smoothing buffer | — | 44 B |
| Beat stability tracking (IBI buffer) | — | 64 B |
| **Total eliminated** | **~46** | **~6.4 KB** |

### What Gets Reused (from AudioController)

| Component | Source (AudioController.cpp lines) |
|-----------|-----------------------------------|
| Pulse detection: floor-tracking baseline | Lines 183-214 |
| Energy synthesis: mic + bass mel + ODF peak-hold | Lines 1767-1793 |
| Pulse modulation: beat-proximity boost/suppress | Lines 1795-1827 |
| Onset density: pulses/second EMA | Lines 1859-1881 |
| ACF core: linearize + autocorrelate + Percival | Lines 443-534, 767-796 |
| Rayleigh prior: perceptual BPM weighting | Lines 662-679 |

### Resource Budgets

#### Memory

| Component | Current | New | Savings |
|-----------|---------|-----|---------|
| NN arena | 7340 B | ~2000 B | 5340 B |
| NN mel buffer | 6656 B (64×26×4) | 1664 B (16×26×4) | 4992 B |
| CBSS buffer | 1440 B | 0 B | 1440 B |
| Bayesian state | ~3200 B | 0 B | 3200 B |
| Log-Gaussian weights | 720 B | 0 B | 720 B |
| OSS buffer + timestamps | 2880 B | 2880 B | 0 |
| Comb filter bank | 5280 B | 5280 B | 0 |
| PLL state | 0 B | 16 B | -16 B |
| **Total** | **~28 KB** | **~12 KB** | **~16 KB saved** |

#### CPU (per frame at 62.5 Hz)

| Component | Current | New |
|-----------|---------|-----|
| Mic + FFT + mel | 3ms | 3ms |
| NN inference | 27ms (W64) | ~7ms (W16) |
| ACF (amortized) | 0.5ms | 0.5ms |
| CBSS + detection | 1ms | 0ms (removed) |
| Bayesian fusion | 0.3ms | 0ms (removed) |
| PLL + comb bank | 0.01ms | 0.3ms |
| Output synthesis | 0.2ms | 0.2ms |
| **Total audio** | **~32ms** | **~11ms** |
| LED rendering | ~5ms | ~5ms |
| **Total frame** | **~37ms (27 fps)** | **~16ms (60+ fps)** |

### Acceptance Criteria

- Compiles alongside AudioController (both in codebase, only one active)
- `show nn` shows W16 model stats
- `show beat` shows AudioTracker state (BPM, phase, PLL state, beat count)
- Phase ramp is visually smooth (no jitter visible in fire breathing effect)
- Fire generator responds to music (energy/pulse/phase populated)
- Silence → organic mode transition is smooth

### Risk: Medium

New algorithm has different failure modes than CBSS. Mitigated by compile-time switch — old system is one `#define` away. Specific risks:
- PLL may lock to wrong phase if first strong onset is off-beat → mitigated by onset gating
- Without octave checker, may get stuck in half/double time → mitigated by Rayleigh prior + Percival
- No CBSS warmup alpha for fast initial lock → ACF starts after 1s (same as current)

---

## Phase 4: Integration

### Goal

Wire AudioTracker into the firmware with a compile-time switch. Deploy and A/B test.

### Changes

**`blinky-things/blinky-things.ino`**: Add `#define USE_AUDIO_TRACKER` compile switch. Instantiate AudioTracker or AudioController based on switch.

**`blinky-things/inputs/SerialConsole.h/.cpp`**: Use `IAudioSystem*` instead of `AudioController*`. Register ~10 tracker params instead of ~56 when AudioTracker is active. Update `show beat` and `json beat` commands for AudioTracker state.

**`blinky-things/config/ConfigStorage.h/.cpp`**: Add `StoredTrackerParams` struct (~40 bytes). Bump SETTINGS_VERSION to 74. Factory reset loads sane defaults for all 10 params.

**`blinky-things/audio/AudioController.h`**: Implement `IAudioSystem` interface (add `virtual` keyword to existing methods).

### Testing Protocol

1. Flash AudioTracker firmware to one nRF52840 device
2. Flash current AudioController firmware to another
3. Play same music (track manifest EDM tracks), compare:
   - **Phase smoothness**: breathing/pulsing animation quality
   - **BPM accuracy**: correct tempo lock within 5 seconds
   - **Pulse responsiveness**: kicks/snares trigger visual sparks
   - **Energy dynamics**: fire height follows music energy
   - **Silence behavior**: graceful organic fallback, resume on music
4. Serial monitoring: `show beat`, `audio`, `json beat` for debugging
5. Multiple genres: EDM, ambient, complex/syncopated

### Acceptance Criteria

- AudioTracker visual quality is comparable to or better than AudioController for EDM
- Phase ramp is smoother than current (no onset-snap stutter)
- BPM locks within 5 seconds on clear-beat tracks
- Graceful degradation on ambient/complex music (organic mode)
- No regressions when compiled with AudioController (old system identical)

### Risk: Medium

Interface abstraction touches several files. Mitigated by: AudioController behavior preserved identically when USE_AUDIO_TRACKER is not defined.

---

## Phase 5: Cleanup and Documentation

After A/B testing confirms AudioTracker quality:

1. Make AudioTracker the default (remove `#ifdef USE_AUDIO_TRACKER`)
2. Move AudioController.h/.cpp to `archive/` or guard with `#ifdef LEGACY_AUDIO_CONTROLLER`
3. Bump SETTINGS_VERSION to 75
4. Remove ~46 obsolete settings from SettingsRegistry
5. Update documentation:
   - `docs/AUDIO_ARCHITECTURE.md` — describe ACF + Comb + PLL architecture
   - `docs/AUDIO-TUNING-GUIDE.md` — update parameter list (~10 params)
   - `docs/IMPROVEMENT_PLAN.md` — update status
   - `CLAUDE.md` — update system architecture overview

### Risk: Low

Phase 4 already validated the system. Old code recoverable from git.

---

## Files Summary

### New Files

| File | Phase | Purpose |
|------|:-----:|---------|
| `ml-training/configs/conv1d_w16_onset.yaml` | 1 | W16 onset-only training config |
| `blinky-things/audio/CombFilterBank.h` | 2 | Extracted comb filter bank class |
| `blinky-things/audio/CombFilterBank.cpp` | 2 | Comb filter implementation |
| `blinky-things/audio/IAudioSystem.h` | 3 | Abstract audio system interface |
| `blinky-things/audio/AudioTracker.h` | 3 | New simplified tracker class |
| `blinky-things/audio/AudioTracker.cpp` | 3 | AudioTracker implementation |

### Modified Files

| File | Phase | Change |
|------|:-----:|--------|
| `blinky-things/audio/AudioController.h` | 2, 3 | Extract comb bank, implement IAudioSystem |
| `blinky-things/audio/AudioController.cpp` | 2 | Remove comb bank implementations |
| `blinky-things/blinky-things.ino` | 4 | Compile switch for AudioTracker |
| `blinky-things/inputs/SerialConsole.h/.cpp` | 4 | IAudioSystem pointer, tracker settings |
| `blinky-things/config/ConfigStorage.h/.cpp` | 4 | StoredTrackerParams, SETTINGS_VERSION 74 |

### Unchanged Files

| File | Reason |
|------|--------|
| `blinky-things/audio/AudioControl.h` | All 7 fields kept (downbeat=0, beatInMeasure=0) |
| `blinky-things/audio/FrameBeatNN.h` | Already supports W16 + single channel |
| `blinky-things/audio/SharedSpectralAnalysis.h` | Unchanged |
| All generators (Fire, Water, Lightning, HeatFire) | Unchanged — degrade gracefully |
| `ml-training/train.py` | Already handles single-channel Conv1D |
| `ml-training/evaluate.py` | Already skips downbeat for single-channel |
| `ml-training/scripts/export_tflite.py` | Already exports 1-channel Conv1D |

---

## Research References

- [Scheirer 1998](https://pubmed.ncbi.nlm.nih.gov/9440344/) — Comb filter bank tempo estimation (JASA)
- [Percival 2014](https://ieeexplore.ieee.org/document/6809732) — Enhanced ACF with harmonic folding (IEEE/ACM TASLP)
- [Grosche & Müller 2011](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S3_PredominantLocalPulse.html) — PLP and Fourier tempogram
- [Meier, Krause, Müller 2024](https://transactions.ismir.net/articles/10.5334/tismir.189) — Real-time PLP (TISMIR), F1=74.7%
- [Adam Stark 2014](https://github.com/adamstark/BTrack) — BTrack CBSS beat tracking
- [Heydari et al. 2022](https://arxiv.org/abs/2201.12563) — 1D state space with jump-back reward (ICASSP)
- [Beat This! 2024](https://github.com/CPJKU/beat_this) — Peak picking post-processing, no DBN needed
- Full algorithm comparison: `ml-training/docs/beat_tracking_algorithm_comparison.md`
