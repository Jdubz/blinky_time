# Parameter Tuning Test Plan

> **SUPERSEDED:** This document is partially out of date. For the current comprehensive guide, see:
> **[docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md)**

---

## Architecture Change Notice (December 2025)

The audio analysis architecture was refactored from PLL-based to autocorrelation-based rhythm tracking:

### Removed Components
- **MusicMode**: PLL-based beat tracking (removed)
- **RhythmAnalyzer**: Comb filter tempo detection (merged)

### New Architecture
- **AudioController**: Unified audio analysis with 6-second autocorrelation buffer
- **Pattern-based**: Rhythm derived from onset pattern analysis, not transient events
- **Simplified output**: 4-parameter `AudioControl` struct

### Removed Parameters

The following parameters from this document **no longer exist**:

| Parameter | Status |
|-----------|--------|
| phasesnap, snapconf, stablephase | REMOVED (PLL gone) |
| confinc, confdec, misspenalty | REMOVED (PLL gone) |
| combdecay, combfb, combconf, histblend | REMOVED (merged into autocorrelation) |
| pllkp, pllki | REMOVED (no PLL) |

### New Parameters

| Parameter | Range | Description |
|-----------|-------|-------------|
| `musicthresh` | 0.0-1.0 | Rhythm activation threshold |
| `phaseadapt` | 0.01-1.0 | Phase adaptation rate |
| `bpmmin` | 40-120 | Minimum BPM to detect |
| `bpmmax` | 80-240 | Maximum BPM to detect |
| `pulseboost` | 1.0-2.0 | On-beat pulse enhancement |
| `pulsesuppress` | 0.3-1.0 | Off-beat pulse suppression |
| `energyboost` | 0.0-1.0 | On-beat energy enhancement |

---

## Current Best Results (Updated December 2025)

### Transient Detection: Hybrid Mode F1 = 0.705

```
detectmode: 4
hitthresh: 2.813
attackmult: 1.1
cooldown: 40
fluxthresh: 1.4
hyfluxwt: 0.7
hydrumwt: 0.3
hybothboost: 1.2
```

### Performance by Pattern

| Pattern | F1 | Precision | Recall | Notes |
|---------|-----|-----------|--------|-------|
| strong-beats | 0.70 | 0.58 | 0.88 | Baseline OK |
| sparse | 0.78 | 0.70 | 0.88 | Good |
| bass-line | 0.73 | 0.79 | 0.68 | Acceptable |
| synth-stabs | 0.74 | 0.76 | 0.73 | Acceptable |
| pad-rejection | 0.44 | 0.29 | 1.00 | HIGH FALSE POSITIVES |
| fast-tempo | 0.80 | 1.00 | 0.67 | Good precision |
| simultaneous | 0.79 | 0.89 | 0.72 | Good |
| full-mix | 0.65 | 0.52 | 0.88 | Needs improvement |

### Key Issues to Address
1. **pad-rejection**: Sustained tones triggering false positives
2. **full-mix**: Low precision in complex audio
3. **simultaneous**: Overlapping sounds (algorithmic limitation)

---

## Quick Start Testing

See [docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md) for the comprehensive 2-3 hour test plan.

### Fast Binary Search (~30 min)

```bash
cd blinky-test-player
npm run tuner -- fast --port COM5 --gain 40
```

### Full Validation

```bash
npm run tuner -- validate --port COM5 --gain 40
```

---

## For Complete Documentation

See **[docs/AUDIO-TUNING-GUIDE.md](../docs/AUDIO-TUNING-GUIDE.md)** which includes:

- All 56 tunable parameters with descriptions
- Complete 2-3 hour test plan
- Historical test results
- Troubleshooting guide
- Architecture overview
