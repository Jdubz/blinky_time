# Audio-Reactive Architecture (AudioTracker)

> **Last checked against the code: 2026-10-21 (SETTINGS_VERSION 98, after PR #268).** The task is **onset detection** (fire on every percussive hit), not beat detection. The only validation corpus is `blinky-test-player/music/edm/`, scored against `.onsets_consensus.json`. Older F1 numbers on the deleted `edm_holdout` corpus are gone from this doc on purpose. When in doubt, `audio/AudioTracker.{h,cpp}` and `audio/SharedSpectralAnalysis.h` win.

## Overview

AudioTracker turns the microphone into one `AudioControl` struct that the generators read. It combines:
- mic input with window/range normalization,
- onset detection (spectral flux on devices; the onset NN is compiled out),
- ACF period detection on three rhythm sources,
- PLP (Predominant Local Pulse) pattern extraction by epoch folding.

**Period detection:** ACF on three mean-subtracted sources (bass flux, bass mel energy, mic level), beat lags 20-80 frames in steps of 2, every `acfPeriodMs` (100 ms). Peaks are refined by parabolic interpolation. Each beat peak spawns bar candidates at 2×/3×/4×, plus syncopation-derived candidates (#166). Candidates are scored by ACF strength × epoch-fold contrast, divided by sqrt(multiplier), times a log-Gaussian tempo prior around 120 BPM (`tempoPriorWidth` 0.8 octave). **Pattern quality is the objective: a half- or double-time period is fine if it gives a better visual pattern.**

**Phase/pattern:** The bass-flux buffer is epoch-folded at the chosen period (recency-weighted, Winsorized, per-bin reliability). The pattern is stored accent-aligned (bin 0 = accent), and `plpPulse` is the pattern read at the current phase. The phase keeps absolute time on the mic's sample clock (see Frame Clock).

**Onset detection:** the task is drum hits (kicks and snares). On devices: SuperFlux (Böck & Widmer, DAFx-13) through the online peak picker of Böck, Krebs & Schedl (ISMIR 2012) (`BLINKY_ONSET_NN_ENABLED=0`). The NN path (FrameOnsetNN through the same picker) is kept for the host NN harness and a future cheaper model (#174). See "Onset Detection" below.

**AGC:** Removed (v72). Hardware gain is fixed at the platform value (`Platform::Microphone::DEFAULT_GAIN`: nRF52840 = 32, ESP32-S3 = 30). Window/range normalization is the only dynamic-range system. There is no noise gate (removed again 2026-10-20).

**How it's measured:** `tests/tracker/` runs the real tracker code on edm/ audio at simulated loop rates. Metrics: `lock` (phase consistency at strong onsets), `worst20s`, `recall` (±50 ms), `bpm`. CI gates on them. See `tests/tracker/README.md`.

**Evolution:**
- **v1 (2024)**: PLL-based phase tracking (unreliable with noisy transients)
- **v2 (December 2024)**: Single-hypothesis autocorrelation (robust but slow to adapt)
- **v3 (January 2026)**: Multi-hypothesis tracking (complex, phase jitter from competing corrections)
- **v4 (February 2026)**: CBSS beat tracking (deterministic phase, counter-based beats)
- **v5 (March 2026)**: AudioTracker — ACF+Comb+PLL, replaces AudioController CBSS. ~400 lines, ~10 params (vs ~2100 lines, ~56 params). No CBSS, no Bayesian fusion.
- **v6 (March 2026)**: PLP (Predominant Local Pulse) — replaced PLL with Fourier tempogram. Soft blend, cold-start template seeding, beat stability gated learning. PLL proven ineffective (phase consistency ~0.04).
- **v7 (March 2026)**: Pattern slot cache — 4-slot LRU of 16-bin PLP pattern digests for instant section recall. v77 pattern memory replaced. Fisher's g removed.
- **v8 (March 2026)**: Multi-source ACF replaces Fourier tempogram (75ms → ~4ms). Parabolic interpolation on ACF peaks. Bar multipliers (2×/3×/4×) with sqrt penalty. Pattern-quality-driven period selection — octave matches are valid.
- **b208 (2026-10)**: Frame clock — every rhythm step runs once per spectral frame; fractional period; accent-aligned pattern; `onset` and `beatCount` outputs.
- **2026-10**: Mic level replaces the NN as rhythm source 2 (#155); syncopation-derived candidates (#166); NN off on devices, tuned flux onsets; noise gate and silence detection removed ("rhythm needs hits").
- **PR #268 (2026-10-21)**: phase advances on the absolute sample clock and is extrapolated to drawing time (#260, #261).

---

## Output: AudioControl Struct

Generators receive a single struct (`audio/AudioControl.h`):

```cpp
struct AudioControl {
    float energy;         // Audio energy level (0-1), never below a 0.12-0.20 "smolder" floor
    float level;          // Mic loudness, auto-ranged 0-1, no gate, no extra smoothing
    float pulse;          // Hit envelope (0-1): onset strength at each onset, 100 ms decay
    float onset;          // Onset EVENT: strength this update, 0 if none
    float phase;          // Rhythm-cycle phase (0-1, 0 = accent)
    uint16_t beatCount;   // +1 per rhythm cycle, never decrements
    float plpPulse;       // PLP pattern value at current phase (0-1)
    float rhythmStrength; // Confidence in rhythm (0-1)
    float onsetDensity;   // Smoothed onsets per second (0-10+)
};
```

| Parameter | Description | Use For |
|-----------|-------------|---------|
| `energy` | Blend of mic level, bass mel energy and flux peak-hold | Baseline intensity, brightness |
| `level` | Normalized mic loudness, swings the full range | Continuous "breathing with the music" (height, brightness) |
| `pulse` | Transient envelope (level, decays) | Continuous brightness/size modulation |
| `onset` | One non-zero value per detected drum hit: its strength, 0-1, on the same scale whichever detector ran | Edge-triggered bursts/flashes (`onset > 0`); strength-gated bursts |
| `phase` | Position in rhythm cycle (0=accent); can be corrected slightly backward | Continuous phase-based modulation |
| `beatCount` | Monotonic cycle counter | "A beat happened": `beatCount != last` (never threshold `phase`) |
| `plpPulse` | PLP extracted pattern value (epoch-folded) | Beat-synced pulsing, breathing animations |
| `rhythmStrength` | Periodicity confidence | Music mode vs organic mode |
| `onsetDensity` | Smoothed transients/second (EMA) | Content classification (dance=2-6, ambient=0-1) |

`rhythmStrength` and `plpPulse` fade to 0 when no onset has fired for 2-4 s (`RHYTHM_HOLD_S` + `RHYTHM_FADE_S`); the next onset restores them at once.

---

## Architecture

```
PDM Microphone
      |
AdaptiveMic (fixed gain + window/range normalization; no AGC, no noise gate)
      |
SharedSpectralAnalysis (FFT-256, 62.5 Hz frames -> soft-knee compressor -> 50 mel bands + spectral flux)
      |
      +--- [ONSET PATH]
      |    Devices: SuperFlux (pre-compressor, 78-band log filterbank, log10(32768 S + 1),
      |         +-1 band max filter, unweighted half-wave sum over bands <= 5 kHz)
      |    NN builds only: FrameOnsetNN activation
      |         -> shared online picker (max of 30 ms, mean of 100 ms + delta, 30 ms gap)
      |            + PLP pattern bias
      |         |
      |    +--- onset (event), pulse (envelope), onsetDensity
      |    +--- Energy: mic level + bass mel energy + flux peak-hold
      |
      +--- [PERIOD PATH: every acfPeriodMs = 100 ms, after 100 frames of warm-up]
      |    3 sources, 792 frames each (~12.7 s at 62.5 Hz):
      |      1. bass flux ^ odfcontrast (the "OSS" buffer)
      |      2. bass mel energy (mel bands 0-5)
      |      3. mic level (#155: never the NN)
      |    ACF lags 20-80 (step 2) -> top 5 peaks -> x1/x2/x3/x4 + syncopation-derived candidates
      |    Score = ACF x sqrt(fold variance) / sqrt(multiplier) x tempo prior
      |    Fractional period: ACF refined at the K-th repetition (K <= 12)
      |
      +--- [PHASE PATH: PLP]
           Accent measured on the bass-flux fold (whichever source won)
           Robust epoch fold of bass flux: recency weights, Winsorized mean, per-bin reliability
           Pattern rotated to accent-aligned storage (bin 0 = accent)
           Phase advanced on the sample clock, pulled toward the measured accent
           plpPulse = pattern at phase (extrapolated to drawing time)
           Pattern slot cache: 4-slot LRU of 16-bin digests; cold-start seeding from 8 templates
                |
AudioControl { energy, level, pulse, onset, phase, beatCount, plpPulse, rhythmStrength, onsetDensity }
      |
Generators (Fire, Water, PlasmaGlobe ["lightning"], Audio)
```

**Key Design Decisions:**

1. **Onsets and rhythm use different signals.** Onset detection fires on every percussive hit, on-beat or not. The rhythm sources are tuned for periodicity. When the NN drove an ACF source it locked straight 4/4 to 2/3 tempo and broke DnB (#155: corpus tempo 0.726 → 0.621), so source 2 is the mic level in every build.

2. **Multi-source ACF period detection.** ACF scans beat lags 20-80 on three mean-subtracted sources. Bar candidates come from multiples of the beat peaks. Breakbeat/DnB patterns put the strongest peaks at 1.5 and 2.5 beats, so each beat peak L also yields beat hypotheses 2L/3 and 2L/5 and bar candidates at 2× and 4× of those (#166, `syncopatedCandidateScale`, default 1.0).

3. **Pattern quality over BPM accuracy.** The system is a visualizer, not a BPM meter. Half/double time is fine if it captures more rhythmic structure.

4. **PLP pattern extraction.** The bass-flux buffer is folded at the period. Each epoch is weighted by recency (`plpDecayRate`). With 4+ epochs, the highest and lowest value per bin are dropped (Winsorized mean). Bins whose epochs disagree (high coefficient of variation) are blended toward the fold mean. The result is min-max normalized. Confidence = ACF strength × signal presence (~1 s smoothed mic level vs `plpSignalFloor`) × agreement with the mic-level fold.

5. **Rhythm needs hits.** There is no silence detector. Room noise fires no flux onsets, so `rhythmStrength` and `plpPulse` fade out 2-4 s after the last onset. Tempo and phase state are kept, so a breakdown can't wipe them (#172).

---

## Frame Clock (b208)

**Every rhythm computation runs exactly once per spectral frame** (16 kHz /
256 samples = 62.5 Hz, `AudioTracker::OSS_FRAME_RATE`). `update()` drains
the samples that were waiting in the mic ring when it was called and calls
`processFrame()` for each completed window: pulse detection, OSS/bass/level
buffers, ACF cadence (counted in frames) and one phase step. In NN builds the
NN runs at most once per `update()`, on the last frame (every frame is still
pushed into its input window). The main loop's rate only decides how many
frames are handled per call.

**The phase keeps absolute time on the sample clock** (`audio/PhaseClock.h`).
- **Overruns (#261):** each phase step covers the samples that actually elapsed, measured by the mic's absolute read index, which counts dropped samples too. Normally that's one frame. When an overrun dropped samples inside a window, the step covers them, so lost audio doesn't put the beat behind real time. Every accent crossed is counted.
- **At output (#260):** `control_.phase` (and `plpPulse`) are extrapolated from the last processed frame to the moment of drawing: `AdaptiveMic::samplesHeardSince()`, which counts samples in the ring plus the PDM block being captured. It's capped at 4 frames, so a stalled audio path waits instead of spinning. It's held just below 1.0, so it never crosses an accent the tracker hasn't counted yet.

Before b208, the phase advanced once per *loop iteration*, only the last
frame of a catch-up burst reached the NN/OSS/pulse detector, the frame rate
constant was 66 instead of 62.5, and the period was an integer. Phase ran at
`loopFps / 62.5` of the tempo, about a third of frames were invisible at
~43 fps, and the NN's input window held unevenly spaced frames.

**Phase model.** The epoch fold is end-aligned: bin `P-1` holds the newest
frame. So at each analysis, `(P-1 - accentBin) / P_frac` is the phase of
"now", which `correctPlpPhase()` pulls toward with gain `plpPhaseGain` (0.3,
not serial-tunable). A period jump of more than 10% snaps instead. Between
analyses, `advancePlpPhase()` adds `elapsedFrames / P_frac` per frame.

**Accent source.** The accent is always measured on the OSS buffer (half-wave rectified bass flux), folded at the chosen period, whichever source won the period vote. Smooth sources (bass energy, mic level) put the fold maximum at the newest frames whenever loudness trends, so the accent tracked "now" and phase stalled near 1.0.

**Fractional period.** The period is refined by unbiased ACF at the K-th
repetition of the beat lag (K up to 12), then divided by K. This removes the
frame-quantization bias a single-period parabola has (~0.15 frame short).

**Pattern storage.** `plpPattern_` is rotated to accent-aligned storage
(bin 0 = accent), so phase indexes it directly. Cached slots and the seed
templates also share a stable origin.

Verify with `tests/tracker/` (host harness, real `AudioTracker` code on
corpus audio at simulated loop rates).

## Rhythm Tracking Algorithm

### Multi-Source ACF + PLP (current)

**Every spectral frame (62.5 Hz), in `processFrame()`:**
1. **Onset detection**: the detector's ODF, then `pickOnset()` (SuperFlux on devices; the NN in NN builds). See "Pulse Detection".
2. **OSS sample**: bass flux raised to `odfContrast` (default 1.25), clamped to 0-1, into the 792-frame OSS buffer.
3. **Bass energy sample**: mean of mel bands 0-5, into the bass buffer.
4. **Level sample**: `mic_.getLevel()`, into source buffer 2.
5. **Phase advance**: `elapsedFrames / P_frac` (see Frame Clock).

**Every `acfPeriodMs` (100 ms), once the OSS buffer holds 100 frames:**
6. **Linearize + mean-subtract** all three sources.
7. **ACF**: lags 20-80 in steps of 2 on each source; local maxima above 0.05, parabolic refinement; top 5 peaks kept (10% minimum separation).
8. **Candidates**: each peak × 1/2/3/4, plus syncopation-derived bar candidates; periods up to 264 frames with 2+ epochs in the buffer.
9. **Scoring**: ACF strength × sqrt(epoch-fold variance) / sqrt(multiplier) × tempo prior.
10. **Fractional period** and **accent position** (on the bass-flux fold), then phase correction.
11. **BPM**: `OSS_FRAMES_PER_MIN / P_frac`, clamped to `bpmMin`..`bpmMax`, EMA with `tempoSmoothing`. A BPM change above 15% clears the pattern slots.
12. **PLP analysis** (`updatePlpAnalysis()`): robust epoch fold, normalization, agreement with the level fold, rotation to accent-aligned storage, confidence update.

**Every 4 beats:** pattern slot check (recall, reinforce or allocate; template seeding for new slots) when PLP confidence > 0.1.

### Evolution: Why ACF+PLP, and Why PLL Failed

| Version | Approach | Strengths | Weaknesses |
|---------|----------|-----------|------------|
| **v1 (PLL)** | Event-driven phase locking | Low latency | Jitter from unreliable transients |
| **v2 (Single Autocorr)** | Pattern-based single tempo | Robust to noise | Slow adaptation, tempo ambiguity |
| **v3 (Multi-Hypo)** | Multiple concurrent tempos | Handles ambiguity | Competing corrections cause phase jitter |
| **v4 (CBSS)** | Cumulative beat strength | Deterministic phase, no jitter | Complex (~2100 lines, ~56 params). Removed in v75. |
| **v5 (ACF+Comb+PLL)** | ACF tempo + comb validation + PLL phase | Simple (~400 lines, ~10 params), smooth phase | **PLL phase consistency ~0.04 (random)** — onset-gated corrections insufficient |
| **v8+ (ACF+PLP, current)** | Multi-source ACF + PLP epoch-fold | ACF selects period, epoch-fold extracts pattern, 3 sources | Bar-length periods phase-lock worse than beat-length ones |

---

## Generator Usage

Generators implement `generate(PixelMatrix& matrix, const AudioControl& audio)` (`generators/Generator.h`). Typical patterns:

### Basic Usage

```cpp
void MyGenerator::generate(PixelMatrix& matrix, const AudioControl& audio) {
    const float dt = frameClock_.tick(millis());   // real seconds, never clamped
    if (audio.onset > 0.0f) {
        burst(audio.onset);                        // one burst per drum hit
    }
    if (audio.rhythmStrength > 0.3f) {
        setBrightness(audio.plpPulse);             // beat-synced breathing
    } else {
        updateOrganic(dt);                         // no rhythm detected
    }
}
```

### Phase Patterns

```cpp
// PLP extracted pattern pulse (actual repeating energy shape)
float breathe = audio.phaseToPulse();  // == audio.plpPulse

// Raw phase (0.0 -> 1.0 over one rhythm cycle)
float sawPhase = audio.phase;

// Beat-synced event: use the monotonic counter, not phase thresholds
if (audio.beatCount != lastBeatCount_) { lastBeatCount_ = audio.beatCount; /* beat */ }

// One event per drum hit
if (audio.onset > 0.0f) { /* burst with strength audio.onset */ }
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

All of these are `set <name> <value>` serial settings (category `tracker`, or `slots` for the slot cache) and are saved by `save`. Defaults are the factory values in `ConfigStorage.cpp`. For the full list, see `docs/AUDIO-TUNING-GUIDE.md`.

### Tempo Detection

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `bpmMin` | `bpmmin` | 15 | 10-120 | Lowest reported BPM (15 lets a full bar at 60 BPM count as one period) |
| `bpmMax` | `bpmmax` | 200 | 120-240 | Highest reported BPM |
| `tempoSmoothing` | `temposmooth` | 0.85 | 0.5-0.99 | BPM EMA smoothing (higher = slower) |

`acfPeriodMs` (100 ms) is persisted but has no serial command. `tempoPriorWidth` (0.8), `syncopatedCandidateScale` (1.0) and `plpPhaseGain` (0.3) are code defaults only.

### PLP Pattern

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `plpConfAlpha` | `plpconfalpha` | 0.25 | 0.01-0.5 | Confidence EMA rate |
| `plpNovGain` | `plpnovgain` | 1.0 | 0.1-5.0 | Power-law exponent on the normalized pattern (>1 sharpens peaks) |
| `plpSignalFloor` | `plpsigfloor` | 0.10 | 0.01-0.5 | Smoothed mic level that gives full confidence |
| `plpVarianceSens` | `plpvarsens` | 0.0 | 0-50 | Persisted and tunable, but not read by the current tracker code |
| `plpDecayRate` | `plpdecay` | 0.2 | 0.05-1.0 | Epoch recency decay (`exp(-rate × epochs)`) |

The pattern slot cache has its own settings (`slotswitchthresh`, `slotnewthresh`, `slotupdaterate`, `slotsaveconf`, `slotseedblend`).

### Rhythm Activation

| Parameter | Serial Name | Default | Range | Description |
|-----------|-------------|---------|-------|-------------|
| `activationThreshold` | `activationthreshold` | 0.3 | 0.0-1.0 | Persisted and tunable; not read by the current tracker code |

`plpActivation` is persisted (0.3) but has no serial command and isn't read.

### Other

| Parameter | Serial Name | Default | Description |
|-----------|-------------|---------|-------------|
| `odfContrast` | `odfcontrast` | 1.25 | Exponent on bass flux before it enters the OSS buffer |
| `audioAmp` | `audio_amp` | 1.0 | Diagnostic: scales every AudioControl signal (100 = saturate, 0 = mute). Not saved. |
| `nnProfile` | `nnprofile` | off | NN inference profiling output (NN builds only) |

---

## Onset Detection: spectral flux on devices; FrameOnsetNN off

**Status (2026-10-19): the NN is off on devices** (`BLINKY_ONSET_NN_ENABLED=0`, `AudioTracker.h`). Onset pulses come from spectral flux, which wins on both accuracy and frame rate. Measured on edm/ (onsets_consensus, ±50 ms) with the host harness CPU cost model calibrated to hat_v1 (`tests/tracker/README.md`):

| Detector (onsets_consensus, ±50 ms, cost model) | F1 | Precision | Recall | LED fps |
|---|---|---|---|---|
| Spectral flux (pre-#280 picker) | 0.546 | 0.457 | 0.679 | 178 |
| v33 NN (50 mel, 19.6 KB, **12 ms/inference** on device) | 0.480 | 0.394 | 0.613 | 48 |

Those are all-onset numbers; detectors are now scored on drums (kick + snare labels aligned per track, ±50 ms, host harness at 60 fps, no cost model; #280). **The corpus plays at the device's level:** hat_v1's mic peaks at about 0.003 FS with music, about −50 dB re the mastered files, re-quantized to int16. Drum F at −60 / −50 / −40 dB; the other columns at −50 dB:

| Detector | Picker | drum F (−60 / −50 / −40 dB) | on-drum | kick R | snare R* | det/s | noise det/s |
|---|---|---|---|---|---|---|---|
| Spectral flux (weighted, adaptive baseline) | 96 ms window + cooldown + gates | 0.243 / 0.543 / 0.603 | 57% | 63% | 20% | 3.1 | 3–6 |
| SuperFlux, all 78 bands | online (Böck 2012), δ 10, λ 32768 | 0.647 / 0.651 / 0.648 | 55% | 79% | 81% | 4.8 | 0 |
| **SuperFlux ≤ 5 kHz (devices)** | **online (Böck 2012), δ 7, λ 32768** | **0.648 / 0.657 / 0.657** | **56%** | **79%** | **76%** | **4.7** | **0** |
| v33 NN, floor 0.20 | 96 ms window + cooldown + gates | 0.594 / 0.595 / 0.588 | 50% | 75% | 72% | 5.0 | ~5 |
| v33 NN, floor 0.55 | online (Böck 2012) | 0.677 / 0.672 / 0.672 | 52% | 95% | 95% | 6.1 | 1–2 |

\*Snare labels are provisional (#284). Noise: steady white or pink noise from −70 to −30 dBFS. Kick timing moves from +4 ms to −4 ms, because the online picker reports the peak frame itself. The harness SuperFlux matches the Python replica of the audit to within 1.3e-4.

Re-enable a model only when it beats flux under this same measurement (a cheaper export is #174). The host NN harness (`BLINKY_HOST_NN`) keeps the NN path tested in CI. The section below describes the NN for that case.

**Drain-loop livelock (b208-b210, fixed):** `update()` used to keep processing samples that arrived *during* a ~12 ms inference. Every frame looked like the newest, so the NN ran on every frame and starved the renderer: hat_v1 b210 ran at 12 fps with 62.4 inferences/s. `update()` now drains only what was waiting at entry and runs at most one inference. CI gates on simulated fps with and without the NN.

**No noise gate, no level-based silence detection (2026-10-20).** This MEMS mic is so weak that music sits at the level a fixed gate calls silence: the b200 gate (0.002) equalled the measured average music level of a mic with enclosure holes, and on hat_v1 it silenced music entirely. It was removed once before (2025-12-26) for the same reason. With the hardware gain fixed at its SNR optimum, the normalizer adapts to whatever is there.

Silence detection went with it. At device-like levels on edm/, an adaptive noise floor falsely reset tempo 487–2159 times in music. Never resetting gave 0 false resets and re-locked on a new song as fast or faster. Instead, **rhythm needs hits**: `rhythmStrength` and `plpPulse` fade out 2–4 s after the last onset, and the next onset restores them instantly. Room noise fires no flux onsets, so the rhythm outputs drop to 0 within 5 s after music stops (they read 0.45–0.77 without this). In music the fade engages 2.1% of the time, mostly in percussion-free breakdowns.

## FrameOnsetNN — off on devices, see above

`FrameOnsetNN` (`audio/FrameOnsetNN.h`) runs a TFLite Micro INT8 model on a sliding window of mel frames. The embedded model (`audio/frame_onset_model_data.h`) is the v33 export: 50 mel bands (30-8000 Hz, `SpectralConstants::NUM_MEL_BANDS`), 20,056 bytes, about 12 ms per inference on hat_v1. Its single output is an onset activation. It detects acoustic onsets (kicks, snares, other hits), not metrical beats.

In NN builds it only drives onset detection. It never feeds the rhythm sources (#155). If the model fails to initialize, `BLINKY_ASSERT` reports it (no silent fallback).

### Spectral Flux

Two flux signals, for different jobs:

- **`getSuperFlux()`: onset detection (#280).** SuperFlux (Böck & Widmer, "Maximum filter vibrato suppression for onset detection", DAFx-13):
  1. Pre-compressor magnitudes go through a triangular log-frequency filterbank: 24 bands per octave from 62.5 Hz to 8 kHz, not area-normalised. Edges that round to the same FFT bin are merged, so there are 78 bands, and bins 2–41 are one band each (`SUPERFLUX_EDGES`, generated, with a `static_assert` on its size).
  2. Each band is log-compressed: `log10(λ·S + 1)`, with λ = `superFluxLambda` = 32768 (the paper's log(1 + X) on int16-scale magnitudes).
  3. Each band is differenced against the previous frame, max-filtered over ±1 band, half-wave rectified, and summed with no weights over the bands centred ≤ 5 kHz (62 of 78). The bands above mostly add hats: at device level, lone hats triggered 75% with all bands and 60% with ≤ 5 kHz at the same δ, with kick and snare recall unchanged. At δ 7 it's 45%.
  - **Why unweighted:** in a full mix a kick rises only about 2–3 dB below 190 Hz but 6–7 dB from 190 Hz to over 4 kHz. A drum hit rises across many bands, a bass note in a few. The old band weights gave each bass bin about 26× a high bin's weight, which amplified bass lines and buried the broadband attack.
  - **Why log:** there is no AGC. Log flux measures relative change, so one picker δ works at any input level. On edm/, drum F was 0.650 / 0.650 / 0.648 / 0.646 at −20 / −10 / 0 / +10 dB, against linear flux's 0.158 / 0.554 / 0.675 / 0.632.
  - **Deviations from the paper:** our hop is 16 ms with no overlap, so the max filter compares against the previous frame (μ = 1). λ is on our [−1, 1] sample scale, chosen by that level test (1000 scored the same).
- **`getSpectralFlux()`: energy synthesis only.** Band-weighted half-wave flux of the compressed magnitudes, with the previous frame max-filtered over 3 bins. Each band is normalised by its bin count, then weighted by `bassflux` 0.5 (bins 1–6), `midflux` 0.2 (bins 7–32) and `highflux` 0.3 (bins 33–127). It feeds the flux peak-hold in `energy`, and the `bassRatio` feature in the onset snapshot.

`getBassFlux()` (bass band only) feeds the OSS rhythm buffer.

### Pulse Detection

`AudioTracker::pickOnset()` runs once per spectral frame, for whichever detector is in use (`audio/OnsetDetector.h`):

```
SharedSpectralAnalysis ─► OnsetDetector ─► OnsetEvidence {fresh, odf}
                            FluxOnsetDetector (SuperFlux) | NNOnsetDetector (activation)
                       ─► AudioTracker::pickOnset (shared)
                            online peak picker (Böck, Krebs & Schedl, ISMIR 2012)
                            × rhythm-pattern bias on delta
                            activation (0-1) + feature snapshot for every onset
                       ─► onset strength, pulse envelope, onset density, rhythm timing
```

- **Picker:** frame t is an onset if:
  - its ODF is the maximum of the last W1 frames and itself;
  - it reaches mean(ODF over the last W3 frames and itself) + δ;
  - more than W5 frames have passed since the last onset.

  The paper's online values are 30 / 100 / 30 ms, which at 62.5 fps is W1 = 2 (`onsetmaxframes`), W3 = 6 (`fluxavgframes`, flux only) and W5 = 2 (`onsetmingap`). These are runtime-tunable and not saved. There's no look-ahead: the onset is reported at the frame that peaks, a frame earlier than the old local-max picker.
- **Per-detector δ and average:**
  - Flux: `fluxdelta`, default 11, saved. The edm/ sweep is flat from 8 to 14, and every leave-one-track-out fold chose 11 or 12. Flux uses the moving average.
  - NN: `pulseonsetfloor`, default 0.55, saved. Its activation is already a probability, so the NN uses a fixed threshold with no moving average. The old 0.20 fired 10/s without the window and cooldown.
- **What it replaced, and why:**
  - **Deleted** in the same change: the 96 ms local-max window, the tempo-adaptive 40–150 ms cooldown, the adaptive flux baseline (`pulsethreshmult`, `fluxonsetfloor`, `blfastdrop`, `blslowrise`), and the crest, beat-grid, re-arm, bass-ratio and presence gates.
  - **Why:** the cooldown blocked 61–71% of the kicks the old picker missed. The adaptive baseline settled at the noise level, so the old flux fired 3–6/s on steady noise at −60 to −30 dBFS; SuperFlux fires 0. The bass gate cost recall for no drum F. Presence never blocked an onset.
- **Rhythm-pattern bias** (`onsetpatternbias` 0.3, runtime, 0 = off): while PLP confidence is above 0.3, δ is raised by up to 30% where the pattern predicts no hit. It is beat-informed and on by default. It should become a strength multiplier rather than a gate (separate work). It is neutral on edm/ drum F.
- **Activation** (onset strength and pulse-envelope input, 0–1):
  - NN: the activation itself.
  - Flux: `1 − 0.7·r^−0.9` with `r = (odf − mean) / δ`, which is 0.30 at the threshold. Below the threshold it is linear to 0.
- **Expensive detectors:** the NN runs at most once per `update()`. Frames it skips return `fresh = false`, and the picker doesn't step on them.
- **Selection:** the NN when built in (`BLINKY_ONSET_NN_ENABLED`) and ready, otherwise flux. Devices use flux. `show nn` names the detector in use.
- `onset` = strongest pulse fired in this `update()`, else 0.
- `pulse` envelope: each picked onset raises it to that onset's strength, then it decays with a 150 ms time constant (`pulsedecay`, runtime-tunable; 100 ms read as a glitch on the hat).
  - It used to max-hold the per-frame activation above 0.4, mapped `(a − 0.4) / 0.6`, with a 240 ms decay. That was a second, threshold-only path beside the picker, and its decay equalled the median gap between drum hits (kick+snare gaps are p25 157 ms, p50 240 ms).
  - Host, device level: only 53% of drum hits made a visible flash, although the picker caught 79%. Driving the pulse from onsets with a 100 ms decay raises that to 77%.
  - Onset strength barely separates drum from non-drum picks (AUC 0.60). So the pulse can't make drums brighter than other onsets; that needs a drum cue in the detector.
- `onsetDensity`: picked onsets per second, EMA 0.7/0.3.
- Gate mask (TRANSIENT stream `gateMask`): bit 1 = pattern bias raised δ. Bits 0, 2 and 3 belonged to the deleted gates and are never set.

### Energy Synthesis

Energy is a blend of three sources, with a 0.12-0.20 deterministic "smolder" floor:
- **Mic level** (`emicweight` 0.30): normalized mic amplitude
- **Bass mel energy** (`emelweight` 0.30): mean of mel bands 0-5
- **Flux peak-hold** (`eodfweight` 0.40): peak spectral flux, released by `odfpkdecay` (0.85) per frame

**Previous approaches (REMOVED):** BandFlux Solo detector (removed v67, ~2600 lines), EnsembleDetector/EnsembleFusion/BassSpectralAnalysis. Mel-spectrogram CNN (v4-v9, 79-98ms, too slow). Beat-proximity pulse/energy boosts (`pulseboost`, `pulsesuppress`, `energyboost`) and the `odfgate` information gate no longer exist.

---

## ESP32-S3 Platform Notes

ESP32-S3 support was cut in March 2026; the code below is kept for the next-generation fixtures (`docs/NEXTGEN_ESP32S3_PLAN.md`).

### JTAG Pin Conflict (Esp32PdmMic)

On XIAO ESP32-S3 Sense, the PDM microphone is wired to GPIO42 (CLK) and GPIO41 (DATA). These are also JTAG strap pins (MTMS and MTDI). When compiled with `USBMode=hwcdc` (required for serial on ESP32 core 3.3.7+), the `USB_SERIAL_JTAG` peripheral can claim these pins via IO_MUX during boot. The I2S driver's `gpio_set_direction()` silently fails to override the JTAG mux, so `i2s_channel_read()` returns zero bytes -- the mic appears dead while `begin()` returns true.

**Fix:** `Esp32PdmMic::begin()` calls `gpio_reset_pin()` on GPIO42 and GPIO41 before I2S initialization. This disconnects the pins from the JTAG peripheral, resets them to GPIO function, and allows the I2S PDM driver to claim them. A verification read (500ms timeout) confirms data is flowing before returning success.

### Software Gain

ESP32-S3 has no hardware PDM gain register. `setGain()` applies a software linear multiplier to PCM samples in `poll()`. The firmware uses a fixed gain of 30 (vs nRF52840's hardware gain of 32).

---

## Resource Usage

| Component | RAM | CPU Time | Notes |
|-----------|-----|----------|-------|
| Spectral frame (FFT, compressor, mel, flux) | a few KB | ~3.2 ms/frame on hat_v1 | `--frame-cost-us=3200` in the harness cost model |
| Rhythm buffers | ~22 KB | - | 7 float arrays of 792 (OSS, bass, level, their linear copies, epoch-fold copy) |
| Scratch buffers | ~4 KB | - | 4 × 264 floats, class members so they stay off the 4 KB loop stack |
| ACF + PLP analysis | - | ~4 ms every 100 ms (code estimate) | Timed in `lastAcfDurationMs_` / `lastPlpMs_` |
| FrameOnsetNN | 8 KB arena + input window | ~12 ms/inference (NN builds only) | The class and its arena are still in device builds; the model never runs there |

---

## Files

**Core Audio System:**
- `blinky-things/audio/AudioTracker.h` - Main tracker class: ACF + PLP, tunable params and defaults
- `blinky-things/audio/AudioTracker.cpp` - Implementation (frame clock, ACF, PLP, pulse detection, output synthesis)
- `blinky-things/audio/PhaseClock.h` - Sample-clock phase advance and render-time extrapolation
- `blinky-things/audio/AudioControl.h` - Output struct definition (9 fields)
- `blinky-things/audio/SharedSpectralAnalysis.h` - FFT -> compressor -> mel bands, spectral flux
- `blinky-things/audio/FrameOnsetNN.h` - TFLite Micro NN onset activation (NN builds only)
- `blinky-things/audio/frame_onset_model_data.h` - INT8 TFLite model weights

**Input Processing:**
- `blinky-things/inputs/AdaptiveMic.h` - Microphone processing (fixed hardware gain, window/range normalization)
- `blinky-things/inputs/SerialConsole.h` - Command interface
- `blinky-things/inputs/SerialConsole.cpp` - `registerTrackerSettings()`, `handleBeatTrackingCommand()`

**Platform HAL:**
- `blinky-things/hal/hardware/Esp32PdmMic.h` - ESP32-S3 PDM mic with JTAG pin conflict fix
- `blinky-things/hal/hardware/Esp32PdmMic.cpp` - `gpio_reset_pin()` fix, software gain, I2S PDM-RX driver

**Tests:**
- `tests/tracker/` - host harness running the real tracker on edm/ audio (CI gate)

---

## SerialConsole Commands

**Beat Tracker Inspection:**
```bash
show beat           # AudioTracker state (BPM, phase, periodicity, PLP, pulse, onset density)
audio               # Overall audio status + BPM (aliases: music, rhythm)
json beat           # JSON tracker state (alias: json rhythm)
show pattern        # Pattern slot cache (alias: show slots; JSON: json pattern / json slots)
reset pattern       # Clear the pattern slots (alias: reset slots)
show spectral       # Spectral analysis state (JSON: json spectral)
show nn             # NN diagnostics (says the NN is disabled on device builds)
show visual         # Render frame metrics (brightness, contrast, activity, centroid) as one JSON line
```

**Debug Control:**
```bash
debug               # Show enabled channels
debug rhythm on     # Enable rhythm debug output
debug rhythm off    # Disable rhythm debug output
debug transient on  # Enable transient detection debug
debug all off       # Disable all debug output
```

Channels: `transient`, `rhythm`, `audio`, `generator`, `ensemble`, `all`.

---

## Related Documentation

- `docs/AUDIO-TUNING-GUIDE.md` - Parameter tuning instructions
- `blinky-test-player/PARAMETER_TUNING_HISTORY.md` - Calibration test results
- `docs/GENERATOR_EFFECT_ARCHITECTURE.md` - Generator design patterns
- `docs/VISUALIZER_GOALS.md` - Design philosophy (visual quality over metrics)
- `tests/tracker/README.md` - Host tracker harness and CPU cost model
