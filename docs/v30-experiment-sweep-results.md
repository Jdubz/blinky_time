# V30 Bayesian Experiment Sweep Results

**Date**: 2026-02-28
**Base**: SETTINGS_VERSION 30 (staging commit 0e5fba7)
**BPM Range**: 60-200 (reverted from 80-160 after first failed sweep)
**Duration**: 30s per track, 18 EDM tracks
**Devices**: 4 simultaneous (shared acoustic space, JBL Pebbles USB speakers at 100%)

## Experiment Configurations

| ID | Branch | Description | ACF | FT | Comb | IOI | FT Enabled | Comb Energy |
|----|--------|-------------|-----|-----|------|-----|------------|-------------|
| EXP1 | exp1-ft-reenable | FT re-enable only | 0.8 | 2.0 | 0.7 | 0.0 | Yes | mean_abs |
| EXP2 | exp2-comb-variance | Comb variance fix | 0.8 | 0.0 | 0.7 | 0.0 | No | sqrt(variance) |
| EXP3 | exp3-ft-plus-comb-fix | FT + comb variance | 0.8 | 2.0 | 0.7 | 0.0 | Yes | sqrt(variance) |
| EXP4 | exp4-tuned-weights | Tuned weights | 0.5 | 1.0 | 0.3 | 0.0 | Yes | sqrt(variance) |

### Device Mapping
- /dev/ttyACM0 → EXP2 (Long Tube)
- /dev/ttyACM1 → EXP3 (Long Tube)
- /dev/ttyACM2 → EXP4 (Tube Light)
- /dev/ttyACM3 → EXP1 (Long Tube)

## Summary

| Experiment | Avg Beat F1 | Avg BPM Accuracy | Music Mode Activated | Failed Tracks |
|-----------|------------|-----------------|---------------------|---------------|
| EXP3 (FT+variance) | **0.109** | 0.651 | 15/18 | 3 |
| EXP1 (FT only) | 0.107 | 0.588 | 15/18 | 3 |
| EXP4 (tuned weights) | 0.084 | **0.783** | 15/18 | 3 |
| EXP2 (variance only) | 0.068 | 0.770 | 12/18 | 6 |

**V27 baseline for comparison**: Avg Beat F1 = 0.519

## Per-Track Results

### Beat F1 Scores

| Track | Expected BPM | EXP2 (ACM0) | EXP3 (ACM1) | EXP4 (ACM2) | EXP1 (ACM3) |
|-------|-------------|-------------|-------------|-------------|-------------|
| afrobeat-feelgood-groove | 117.5 | 0.113 | 0.104 | **0.138** | 0.123 |
| amapiano-vibez | 112.3 | 0.101 | 0.185 | 0.195 | **0.260** |
| breakbeat-background | 86.1 | 0.000 | 0.000 | 0.000 | 0.000 |
| breakbeat-drive | 95.7 | 0.000 | 0.000 | **0.039** | 0.000 |
| dnb-energetic-breakbeat | 117.5 | 0.000 | 0.000 | 0.000 | 0.000 |
| dnb-liquid-jungle | 112.3 | 0.000 | **0.033** | 0.032 | 0.000 |
| dubstep-edm-halftime | 117.5 | 0.000 | **0.174** | 0.000 | 0.101 |
| edm-trap-electro | 112.3 | **0.034** | 0.000 | 0.000 | 0.032 |
| garage-uk-2step | 129.2 | 0.000 | 0.000 | 0.000 | 0.000 |
| reggaeton-fuego-lento | 92.3 | 0.036 | 0.065 | **0.108** | 0.071 |
| techno-deep-ambience | 123.0 | 0.176 | **0.212** | 0.051 | 0.099 |
| techno-dub-groove | 123.0 | 0.000 | 0.079 | 0.076 | **0.081** |
| techno-machine-drum | 143.6 | 0.000 | 0.000 | 0.000 | 0.000 |
| techno-minimal-01 | 129.2 | 0.206 | 0.148 | **0.295** | 0.165 |
| techno-minimal-emotion | 129.2 | 0.239 | **0.342** | 0.187 | 0.296 |
| trance-goa-mantra | 136.0 | 0.151 | 0.220 | 0.209 | **0.265** |
| trance-infected-vibes | 143.6 | 0.124 | **0.276** | 0.141 | 0.226 |
| trance-party | 136.0 | 0.052 | 0.128 | 0.049 | **0.200** |

### BPM Accuracy

| Track | Expected BPM | EXP2 (ACM0) | EXP3 (ACM1) | EXP4 (ACM2) | EXP1 (ACM3) |
|-------|-------------|-------------|-------------|-------------|-------------|
| afrobeat-feelgood-groove | 117.5 | 64.3 (0.547) | 138.3 (0.823) | 152.7 (0.701) | 111.4 (**0.948**) |
| amapiano-vibez | 112.3 | 89.5 (0.797) | 191.0 (0.299) | 124.6 (**0.890**) | 183.7 (0.364) |
| breakbeat-background | 86.1 | — | — | 85.1 (**0.988**) | 114.1 (0.675) |
| breakbeat-drive | 95.7 | 65.4 (0.683) | 156.7 (0.362) | 113.8 (**0.811**) | 187.3 (0.043) |
| dnb-energetic-breakbeat | 117.5 | — | — | 128.0 (**0.910**) | — |
| dnb-liquid-jungle | 112.3 | — | 146.0 (**0.700**) | 152.3 (0.644) | 167.3 (0.511) |
| dubstep-edm-halftime | 117.5 | 69.2 (0.589) | 173.2 (0.526) | 155.6 (**0.676**) | 184.1 (0.433) |
| edm-trap-electro | 112.3 | 87.3 (0.778) | 92.7 (**0.825**) | — | 140.0 (0.753) |
| garage-uk-2step | 129.2 | — | 159.6 (**0.765**) | — | — |
| reggaeton-fuego-lento | 92.3 | 93.4 (**0.988**) | 136.9 (0.517) | 120.1 (0.699) | 185.0 (0.000) |
| techno-deep-ambience | 123.0 | 159.7 (0.702) | 172.3 (0.599) | 173.1 (0.592) | 152.4 (**0.761**) |
| techno-dub-groove | 123.0 | — | 171.4 (0.607) | 137.1 (**0.886**) | 171.3 (0.607) |
| techno-machine-drum | 143.6 | — | — | — | — |
| techno-minimal-01 | 129.2 | 170.6 (0.680) | 156.0 (0.793) | 176.0 (0.638) | 123.5 (**0.956**) |
| techno-minimal-emotion | 129.2 | 135.7 (**0.950**) | 168.5 (0.696) | 154.5 (0.804) | 175.6 (0.640) |
| trance-goa-mantra | 136.0 | 108.5 (0.798) | 155.4 (**0.858**) | 165.2 (0.785) | 179.9 (0.677) |
| trance-infected-vibes | 143.6 | 108.5 (0.756) | 182.5 (0.729) | 104.0 (0.724) | 180.2 (**0.745**) |
| trance-party | 136.0 | 132.0 (0.971) | 180.9 (0.670) | 135.3 (**0.995**) | 175.4 (0.711) |

### Transient F1 Scores

| Track | EXP2 (ACM0) | EXP3 (ACM1) | EXP4 (ACM2) | EXP1 (ACM3) |
|-------|-------------|-------------|-------------|-------------|
| afrobeat-feelgood-groove | 0.545 | 0.444 | 0.370 | 0.532 |
| amapiano-vibez | 0.173 | 0.237 | 0.500 | 0.504 |
| breakbeat-background | 0.198 | 0.063 | 0.283 | 0.113 |
| breakbeat-drive | 0.364 | 0.394 | 0.309 | 0.226 |
| dnb-energetic-breakbeat | 0.200 | 0.286 | 0.316 | 0.240 |
| dnb-liquid-jungle | 0.333 | 0.328 | 0.240 | 0.226 |
| dubstep-edm-halftime | 0.365 | 0.426 | 0.290 | 0.250 |
| edm-trap-electro | 0.235 | 0.325 | 0.294 | 0.405 |
| garage-uk-2step | 0.538 | 0.470 | 0.467 | 0.460 |
| reggaeton-fuego-lento | 0.361 | 0.385 | 0.244 | 0.297 |
| techno-deep-ambience | 0.771 | 0.671 | 0.413 | 0.748 |
| techno-dub-groove | 0.503 | 0.667 | 0.262 | 0.671 |
| techno-machine-drum | 0.253 | 0.273 | 0.229 | 0.174 |
| techno-minimal-01 | 0.356 | 0.371 | 0.341 | 0.387 |
| techno-minimal-emotion | 0.835 | 0.833 | 0.492 | 0.766 |
| trance-goa-mantra | 0.214 | 0.538 | 0.448 | 0.557 |
| trance-infected-vibes | 0.327 | 0.398 | 0.356 | 0.371 |
| trance-party | 0.570 | 0.571 | 0.480 | 0.695 |

## Analysis

### 1. All Experiments Severely Regressed vs V27 Baseline

Best experiment (EXP3) averages Beat F1 = 0.109, an **80% regression** from the v27 baseline of 0.519. This regression is shared across ALL experiments since they all build on the v30 base commit.

### 2. V30 Base Changes Are the Primary Cause

All 4 experiments share the v30 base commit (0e5fba7) which added:
- Posterior floor (`posteriorFloor`)
- Disambiguation nudge (`disambigNudge`)
- Harmonic transition weights (`harmonicTransWeight`)
- 80-160 BPM range (reverted to 60-200 for this sweep, but compile-time lag constants still affect comb filter coverage)

The individual experiment changes (FT re-enable, comb variance, tuned weights) produce only marginal differences (0.068-0.109) compared to the massive shared regression.

### 3. Double-Time Locking Is Pervasive

Most tracks show detected BPM 30-50% above expected:
- 136 BPM tracks → detected at 175-180 BPM
- 117 BPM tracks → detected at 140-190 BPM
- 123 BPM tracks → detected at 150-175 BPM

This suggests the harmonic disambiguation in v30 is pushing toward double-time rather than preventing it.

### 4. FT Helps Music Mode Activation

EXP2 (no FT) only activated music mode on 12/18 tracks vs 15/18 for all FT-enabled experiments. The Fourier tempogram contributes meaningful information for tempo detection.

### 5. Comb Variance Fix Has Marginal Impact

EXP1 (mean_abs comb, FT) vs EXP3 (sqrt-variance comb, FT) differ by only 0.002 in avg F1 (0.107 vs 0.109). The variance fix doesn't meaningfully help or hurt when FT is enabled.

### 6. EXP4 Tuned Weights Have Best BPM Accuracy

Despite lower Beat F1, EXP4 (ACF=0.5, FT=1.0, Comb=0.3) achieves the best average BPM accuracy (0.783). The reduced weights may be preventing some double-time lock but at the cost of beat placement precision.

## Bugs Fixed During This Sweep

1. **BPM range 80-160 too restrictive** — Reverted to 60-200 on all branches. The first sweep (pre-fix) showed avg F1 of 0.06-0.09 with many tracks unable to activate music mode at all.

2. **Comb variance scale mismatch** — Variance values (~0.001-0.01) were being clipped by the 0.01 floor in Bayesian fusion, making comb observation flat. Fixed by returning `sqrtf(variance)` from `getFilterEnergy()`.

## Next Steps

1. **Compare against v29 baseline** — Build v29 firmware (pre-v30 changes) and run the same 18-track sweep to confirm v30 is the regression source.
2. **Revert v30 features individually** — Test disabling posteriorFloor, disambigNudge, and harmonicTransWeight one at a time to identify which causes the double-time locking.
3. **Consider abandoning v30 approach** — If v29 baseline confirms regression, revert to v29 and pursue improvements incrementally.
