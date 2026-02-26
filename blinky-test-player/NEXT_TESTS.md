# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** February 26, 2026

## Current Config

**Detector:** BandWeightedFlux Solo (all others disabled)
- gamma=20, bassWeight=2.0, midWeight=1.5, highWeight=0.1, threshold=0.5
- minOnsetDelta=0.3 (onset sharpness gate — rejects slow-rising pads/echoes)
- All defaults confirmed near-optimal via sweep testing (Feb 21, 2026)

**Beat tracking:** CBSS, beatoffset=5 (changed from 9, +109% avg beat F1)

## Synthetic Pattern Performance (BandFlux Solo + onsetDelta=0.3)

| Pattern | F1 | Precision | Recall | FPs | Notes |
|---------|:---:|:---------:|:------:|:---:|-------|
| strong-beats | 1.000 | 1.00 | 1.00 | 0 | Perfect |
| hat-rejection | 1.000 | 1.00 | 1.00 | 0 | Perfect |
| synth-stabs | 1.000 | 1.00 | 1.00 | 0 | **Fixed** (was 0.600, 16 FPs) |
| medium-beats | 0.985 | 0.97 | 1.00 | 1 | Near-perfect |
| tempo-sweep | 0.914 | 0.84 | 1.00 | 3 | +0.05 vs no filter |
| bass-line | 0.869 | 1.00 | 0.77 | 0 | |
| full-mix | 0.843 | 1.00 | 0.73 | 0 | |
| sparse | 0.800 | 0.67 | 1.00 | 1 | |
| lead-melody | 0.720 | 1.00 | 0.56 | 0 | |
| chord-rejection | 0.688 | 0.69 | 0.69 | 1 | |
| pad-rejection | 0.421 | 0.27 | 1.00 | 16 | **+0.11** (was 0.314, 25 FPs) |

## Real Music Beat Tracking (9 tracks, beatoffset=5, onsetDelta=0.3)

| Track | Beat F1 | BPM Acc | Beat Offset | Transient F1 |
|-------|:-------:|:-------:|:-----------:|:------------:|
| trance-party | 0.775 | 0.993 | -54ms | 0.774 |
| minimal-01 | 0.695 | 0.959 | -47ms | 0.544 |
| infected-vibes | 0.691 | 0.973 | -58ms | 0.721 |
| goa-mantra | 0.605 | 0.993 | +45ms | 0.294 |
| minimal-emotion | 0.486 | 0.998 | +57ms | 0.154 |
| deep-ambience | 0.404 | 0.949 | -18ms | 0.187 |
| machine-drum | 0.224 | 0.825 | -42ms | 0.312 |
| trap-electro | 0.190 | 0.924 | -23ms | 0.298 |
| dub-groove | 0.176 | 0.830 | +56ms | 0.374 |
| **Average** | **0.472** | **0.938** | | **0.406** |

## Completed Work (Feb 21, 2026)

- **Onset delta filter** (minOnsetDelta=0.3): Rejects slow-rising signals (pads, echoes)
  - Fixed synth-stabs regression: 0.600 → 1.000
  - Improved pad-rejection: 0.314 → 0.421 (35 FPs → 16 FPs)
  - Improved real music avg Beat F1: 0.452 → 0.472
  - Best track improvements: trance-party +0.098, minimal-emotion +0.104, goa-mantra +0.055
- BandWeightedFluxDetector: log-compressed FFT, band-weighted half-wave flux, additive threshold
- beatoffset recalibration: 9→5 (doubled avg beat F1 from 0.216 to 0.452)
- BandFlux parameter sweep: gamma (10-30), bassWeight (1.5-3.0), threshold (0.3-0.7) all at defaults
- Sub-harmonic investigation: machine-drum locks at ~120 BPM, fundamental autocorrelation limitation
- Runtime params persisted: tempoSmoothFactor, odfSmoothWidth, harmup2x, harmup32, peakMinCorrelation → flash (SETTINGS_VERSION 12)
- Confidence gating analysis: existing activationThreshold is sufficient
- Per-track offset investigation: varies -58ms to +57ms, adaptive offset risks oscillation, accepted as limitation

## Onset Delta Sweep Results (Feb 21)

| onsetDelta | pad-rejection FPs | medium-beats recall | trance-party Beat F1 | deep-ambience Beat F1 |
|:----------:|:-----------------:|:-------------------:|:--------------------:|:---------------------:|
| 0.0 (off) | 35 | 1.00 | 0.677 | 0.499 |
| 0.2 | ~22 | 1.00 | 0.637 | 0.618 |
| **0.3** | **16** | **1.00** | **0.775** | **0.404** |
| 0.5 | 16 | 1.00 | — | — |
| 0.7 | 15 | 0.97 | — | — |
| 1.0 | 5 | 0.88 | — | — |

0.3 chosen as default: best overall average, preserves all medium-strength kicks, fixes synth-stabs.

## Next Priorities

> **Design philosophy:** See [VISUALIZER_GOALS.md](../docs/VISUALIZER_GOALS.md) — visual quality over metric perfection. Low Beat F1 on ambient/trap tracks is acceptable (organic mode fallback is correct).

1. **FFT-512 bass-focused analysis** — Kick drum energy (40-80Hz) occupies 1-2 FFT bins at current FFT-256. FFT-512 doubles bass resolution for better kick/bass discrimination. ~200 lines, ~5KB RAM.
2. **Particle filter beat tracking** — Fundamentally different approach that handles multi-modal tempo distributions (e.g., half-time ambiguity). ~100-150 lines, ~2KB RAM.

### Completed (Feb 2026)
- ~~**Verify startup latency improvement**~~ — ✅ Progressive startup implemented (autocorrelation at 1s). Validated through multi-device testing.
- ~~**Validate onset density values**~~ — ✅ Implemented as `AudioControl::onsetDensity` and `"od"` in streaming JSON. Modulates rhythmStrength.
- ~~**Build diverse test music library**~~ — ✅ 18 tracks (9 original + 9 syncopated) with ground truth annotations.
- ~~**Microphone sensitivity**~~ — ✅ Addressed by spectral compressor (6dB makeup gain) + per-bin adaptive whitening (v23+). Enabled FT/IOI re-activation in v24.

## Known Limitations

| Issue | Root Cause | Visual Impact |
|-------|-----------|---------------|
| Machine-drum sub-harmonic lock | Autocorrelation finds ~120 BPM peak | **Low** — half-time still looks rhythmic |
| Per-track beat offset variation | ODF latency varies (-58 to +57ms) | **None** — invisible at LED update rates |
| Pad false positives (~16-22) | Pad chord transitions create sharp flux | **Low** — timing analysis shows all FPs are on-beat (clustered around musical events), not random. Visually appears as "extra busy but rhythmic." Worst case is isolated pads only (synthetic pattern); real music masks these. |
| deep-ambience low Beat F1 | Soft ambient onsets below detection threshold | **None** — organic mode is correct response |
| trap-electro low Beat F1 | Syncopated kicks challenge causal tracking | **Low** — energy-reactive mode is acceptable |
| Test run variance | Room acoustics, ambient noise | Accept or use 3-run averaging |
