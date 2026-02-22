# Next Testing Priorities

> **See Also:** [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for comprehensive testing documentation.
> **History:** [PARAMETER_TUNING_HISTORY.md](./PARAMETER_TUNING_HISTORY.md) for all calibration results.

**Last Updated:** February 21, 2026

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

1. **Pad rejection further improvement** — Still 16 FPs at onsetDelta=0.3. Possible band-ratio gate (pads have significant midFlux vs kicks which are bass-only) or sustain envelope check.
2. **Build diverse test music library** — hip hop (syncopated), DnB (broken beats, 170+ BPM), funk (swing), pop (sparse), rock (fills) with ground truth annotations.
3. **deep-ambience regression** — F1 dropped from 0.499 to 0.404 with onset delta filter. Investigate if onsetDelta=0.2 could be auto-selected for ambient content based on signal characteristics.
4. **Environment adaptability** — Auto-switch detector profiles based on signal characteristics (club vs ambient vs festival).

## Known Limitations (Not Fixable by Tuning)

| Issue | Root Cause | Status |
|-------|-----------|--------|
| Machine-drum sub-harmonic lock | Autocorrelation finds ~120 BPM peak, prior reinforces it | Investigated — fundamental limitation |
| Per-track beat offset variation | ODF latency varies with audio content (-58 to +57ms) | beatoffset=5 is best compromise |
| Pad false positives (16 remaining) | Pad chord transitions create sharp spectral changes | onsetDelta=0.3 removes 54%, rest need band-ratio gating |
| deep-ambience sensitive to onset filter | Soft ambient onsets get filtered | onsetDelta=0.2 helps but hurts trance |
| Test run variance | Room acoustics, ambient noise | Accept or use 3-run averaging |
