# Audio Tuning Guide

**Last Updated:** February 27, 2026
**Firmware Version:** SETTINGS_VERSION 25 (BandFlux Solo + Bayesian Tempo Fusion)

This document consolidates all audio testing and tuning information for the Blinky audio-reactive LED system.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Testing Infrastructure](#testing-infrastructure)
3. [All Tunable Parameters](#all-tunable-parameters)
4. [Current Best Settings](#current-best-settings)
5. [Historical Test Results](#historical-test-results)
6. [Comprehensive Test Plan](#comprehensive-test-plan)
7. [Troubleshooting](#troubleshooting)
8. [Appendix: Removed Parameters](#appendix-removed-parameters)

---

## Architecture Overview

### Audio Signal Flow

```
PDM Microphone (16kHz, mono)
        |
   Hardware AGC (0-80 gain, targets hwTarget level)
        |
   AdaptiveMic (Window/Range normalization)
        |
   SharedSpectralAnalysis (FFT-256)
   ├── Soft-knee compressor (Giannoulis 2012)
   └── Per-bin adaptive whitening (Stowell & Plumbley 2007)
        |
   BassSpectralAnalysis (Goertzel-12, 31.25 Hz/bin, optional)
        |
   EnsembleDetector (BandFlux Solo — 1 of 7 detectors enabled)
   ├── BandWeightedFlux (1.0) ── enabled  ← log-compressed band-weighted flux
   ├── Drummer (0.50) ────────── disabled
   ├── SpectralFlux (0.20) ───── disabled
   ├── HFC (0.20) ────────────── disabled
   ├── BassBand (0.45) ───────── disabled
   ├── ComplexDomain (0.50) ──── disabled
   └── Novelty (0.12) ────────── disabled
        |
   Fusion: agree_1=1.0 (solo pass-through), cooldown=250ms, minconf=0.40
        |
   AudioController
   ├── OSS Buffer (6 seconds, 360 samples @ 60Hz)
   ├── Autocorrelation (every 250ms) with inverse-lag normalization
   ├── Bayesian Tempo Fusion (20 bins, 60-180 BPM)
   │   ├── ACF observation (weight 0.3)
   │   ├── Fourier tempogram (weight 2.0, re-enabled v24)
   │   ├── Comb filter bank (weight 0.7, primary)
   │   └── IOI histogram (weight 2.0, re-enabled v24)
   ├── Per-sample ACF harmonic disambiguation (2x + 1.5x checks)
   ├── CBSS beat tracking (adaptive threshold = 1.0 × running mean)
   ├── Counter-based beat detection (deterministic phase)
   └── ODF pre-smoothing (5-point causal moving average)
        |
   AudioControl { energy, pulse, phase, rhythmStrength, onsetDensity }
        |
   Fire/Water/Lightning Generators (visual effects)
```

### Key Design Decisions

1. **BandFlux Solo**: Single detector outperforms multi-detector ensembles (+14% avg Beat F1). Ensemble fusion dilutes BandFlux's cleaner signal.
2. **Spectral conditioning**: Soft-knee compressor normalizes gross signal level; per-bin whitening makes detectors invariant to sustained spectral content. Enables FT/IOI re-activation.
3. **Bayesian tempo fusion**: Unified posterior estimation over 20 tempo bins. Comb filter bank is the primary observation; ACF at low weight (0.3) prevents sub-harmonic lock.
4. **CBSS beat tracking**: Cumulative Beat Strength Signal with adaptive threshold prevents phantom beats during silence/breakdowns.
5. **Deterministic phase**: Phase derived from counter: `(now - lastBeat) / period` — no drift or jitter.
6. **5-parameter output**: Generators receive `AudioControl` struct with energy, pulse, phase, rhythmStrength, onsetDensity.

---

## Testing Infrastructure

### Components

| Component | Location | Purpose |
|-----------|----------|---------|
| **blinky-serial-mcp** | `blinky-serial-mcp/` | MCP server for device communication (20+ tools) |
| **blinky-test-player** | `blinky-test-player/` | Audio pattern playback + ground truth generation |
| **param-tuner** | `blinky-test-player/src/param-tuner/` | Binary search + sweep optimization |
| **run_music_test** | MCP tool | Full-track beat tracking evaluation with ground truth |

### Quick Commands

```bash
# Run a single pattern test via MCP
run_test(pattern: "strong-beats", port: "/dev/ttyACM0")

# Run a full music track with ground truth evaluation
run_music_test(audio_file: "blinky-test-player/music/edm/trance-party.mp3",
               ground_truth: "blinky-test-player/music/edm/trance-party.beats.json",
               port: "/dev/ttyACM0")

# Multi-device parameter sweep
cd blinky-test-player
npm run tuner -- multi-sweep --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM3 \
  --params bayesacf --duration 30

# Monitor audio levels
monitor_audio(port: "/dev/ttyACM0", duration_ms: 3000)

# Monitor transient detections
monitor_transients(port: "/dev/ttyACM0", duration_ms: 5000)
```

### Test Tracks (18 Total)

| Category | Tracks | BPM Range |
|----------|--------|-----------|
| **Trance** | trance-party, trance-infected-vibes, trance-goa-mantra | 128-145 |
| **Techno** | techno-minimal-01, techno-minimal-emotion, techno-deep-ambience, techno-machine-drum, techno-dub-groove | 120-140 |
| **EDM/Trap** | edm-trap-electro, dubstep-edm-halftime | 128-150 |
| **DnB** | dnb-energetic-breakbeat, dnb-liquid-jungle | 170-175 |
| **World/Urban** | afrobeat-feelgood-groove, amapiano-vibez, reggaeton-fuego-lento, garage-uk-2step | 95-130 |
| **Breakbeat** | breakbeat-drive, breakbeat-background | 120-140 |

### Synthetic Test Patterns (14 Total)

| Category | Patterns | Purpose |
|----------|----------|---------|
| **Baseline** | strong-beats, medium-beats, soft-beats | Basic transient detection |
| **Rejection** | hat-rejection, pad-rejection, chord-rejection | False positive testing |
| **Tempo** | fast-tempo, tempo-sweep | Speed and tempo detection |
| **Complexity** | bass-line, synth-stabs, lead-melody, full-mix | Real-world complexity |
| **Edge Cases** | sparse, simultaneous | Silence gaps, overlapping hits |

---

## All Tunable Parameters

### Category: `ensemble` (3 parameters) - Ensemble Detection Gating

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `enscooldown` | 250 | 20-500 ms | Base ensemble cooldown between detections |
| `ensminconf` | 0.40 | 0.0-1.0 | Minimum detector confidence for output |
| `ensminlevel` | 0.0 | 0.0-0.5 | Noise gate audio level |

**Per-detector commands** (via `set`/`show`):
| Command | Description |
|---------|-------------|
| `set detector_enable <type> <0\|1>` | Enable/disable detector (drummer, spectral, hfc, bass, complex, novelty, bandflux) |
| `set detector_weight <type> <val>` | Set detector weight in fusion |
| `set detector_thresh <type> <val>` | Set detector threshold |

**Per-detector defaults (SETTINGS_VERSION 25):**

| Detector | Weight | Threshold | Enabled | Notes |
|----------|--------|-----------|---------|-------|
| **BandWeightedFlux** | 0.50 | 0.5 | **yes** | Log-compressed band-weighted flux, additive threshold |
| Drummer | 0.50 | 4.5 | no | Amplitude transients |
| ComplexDomain | 0.50 | 3.5 | no | Phase onset detection |
| BassBand | 0.45 | 3.0 | no | Too noisy (100+ detections/30s) |
| SpectralFlux | 0.20 | 1.4 | no | Fires on pad chord changes |
| HFC | 0.20 | 4.0 | no | Hi-hat detector, creates busy visuals |
| Novelty | 0.12 | 2.5 | no | Near-zero detections on real music |

### BandFlux Parameters (via `set`/`show` commands, not persisted to flash)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `bandflux_gamma` | 20.0 | 1-100 | Log compression strength: `log(1 + gamma * mag)` |
| `bandflux_bassweight` | 2.0 | 0-5 | Bass band flux weight (promotes kick detection) |
| `bandflux_midweight` | 1.5 | 0-5 | Mid band flux weight |
| `bandflux_highweight` | 0.1 | 0-2 | High band flux weight (low = hi-hat rejection) |
| `bandflux_maxbin` | 64 | 16-128 | Max FFT bin to analyze |
| `bandflux_onsetdelta` | 0.3 | 0-2 | Min flux jump from previous frame (pad/echo rejection) |
| `bandflux_hiresbass` | off | bool | Enable Goertzel-12 hi-res bass analysis |
| `bandflux_diffframes` | 1 | 1-3 | Temporal reference depth (keep at 1) |
| `bandflux_perbandthresh` | off | bool | Per-band independent thresholds (keep off) |
| `bandflux_perbandmult` | 1.5 | 0.5-5 | Per-band threshold multiplier |
| `bandflux_dominance` | 0.0 | 0-1 | Band-dominance gate (disabled, experimental) |
| `bandflux_decayratio` | 0.0 | 0-1 | Post-onset decay gate (disabled, experimental) |
| `bandflux_decayframes` | 3 | 0-6 | Decay confirmation frames |
| `bandflux_crestgate` | 0.0 | 0-20 | Spectral crest factor gate (disabled, experimental) |

### Category: `rhythm` (21 parameters) - Beat Tracking (AudioController)

**Onset Strength Signal:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `ossfluxweight` | 1.0 | 0.0-1.0 | OSS generation: 1.0=spectral flux, 0.0=RMS energy |
| `adaptbandweight` | true | bool | Enable adaptive band weighting |
| `bassbandweight` | 0.5 | 0.0-1.0 | Bass band weight (when adaptive disabled) |
| `midbandweight` | 0.3 | 0.0-1.0 | Mid band weight |
| `highbandweight` | 0.2 | 0.0-1.0 | High band weight |
| `odfsmooth` | 5 | 3-11 (odd) | ODF smoothing window width |
| `odfmeansub` | true | bool | ODF mean subtraction (essential — keep ON) |

**Tempo estimation:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `combbankenabled` | true | bool | Enable comb filter bank for tempo |
| `combbankfeedback` | 0.92 | 0.85-0.98 | Comb bank resonance strength |
| `autocorrperiod` | 250 | 100-1000 ms | Autocorrelation computation interval |
| `bpmmin` | 60 | 40-120 | Minimum BPM to detect |
| `bpmmax` | 200 | 80-240 | Maximum BPM to detect |
| `temposmooth` | 0.85 | 0.5-0.99 | Tempo EMA smoothing factor |
| `ft` | true | bool | Fourier tempogram observation (re-enabled v24) |
| `ioi` | true | bool | IOI histogram observation (re-enabled v24) |

**CBSS beat detection:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `cbssalpha` | 0.9 | 0.5-0.99 | CBSS weighting (higher = more predictive) |
| `cbsstight` | 5.0 | 1.0-20.0 | Log-Gaussian tightness (higher = stricter tempo) |
| `cbssthresh` | 1.0 | 0.0-2.0 | Adaptive threshold: beat fires only if CBSS > factor × mean |
| `beatconfdecay` | 0.98 | 0.9-0.999 | Per-frame confidence decay when no beat |
| `beatoffset` | 5.0 | 0.0-15.0 | Beat prediction advance in frames (ODF+CBSS delay compensation) |
| `phasecorr` | 0.0 | 0.0-1.0 | Phase correction strength (keep at 0 — hurts syncopation) |

**Output modulation:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `musicthresh` | 0.4 | 0.0-1.0 | Rhythm activation threshold |
| `pulseboost` | 1.3 | 1.0-2.0 | Pulse boost on beat |
| `pulsesuppress` | 0.6 | 0.3-1.0 | Pulse suppress off beat |
| `energyboost` | 0.3 | 0.0-1.0 | Energy boost on beat |

### Category: `bayesian` (8 parameters) - Bayesian Tempo Fusion

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `bayeslambda` | 0.15 | 0.01-1.0 | Transition tightness (lower = more rigid tempo) |
| `bayesprior` | 128.0 | 60-200 | Static prior center BPM |
| `bayespriorw` | 0.0 | 0.0-3.0 | Ongoing static prior strength (0 = off, default) |
| `priorwidth` | 50.0 | 10-80 | Prior width (sigma BPM) |
| `bayesacf` | 0.3 | 0.0-5.0 | ACF observation weight (low prevents sub-harmonic lock) |
| `bayesft` | 2.0 | 0.0-5.0 | Fourier tempogram weight (re-enabled v24) |
| `bayescomb` | 0.7 | 0.0-5.0 | Comb filter bank weight (primary observation) |
| `bayesioi` | 2.0 | 0.0-5.0 | IOI histogram weight (re-enabled v24) |

**CRITICAL interaction warning:** Setting `cbssthresh` below 0.8 while `bayesft` or `bayesioi` are above 0.5 causes catastrophic beat tracking failure (F1 drops to 0.049). The FT/IOI sub-harmonic bias floods CBSS with phantom beats that the low threshold can't reject. Safe combinations: `cbssthresh >= 1.0` with any FT/IOI weight, or `bayesft/bayesioi <= 0.5` with any cbssthresh.

### Category: `spectral` (10 parameters) - Spectral Processing (v23+)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `whitenenabled` | true | bool | Per-bin spectral whitening (adaptive normalization) |
| `whitendecay` | 0.997 | 0.9-0.9999 | Per-frame peak decay (~5s memory at 0.997) |
| `whitenfloor` | 0.001 | 0.0001-0.1 | Noise floor for whitening |
| `compenabled` | true | bool | Soft-knee compressor before whitening |
| `compthresh` | -30.0 | -60.0 to 0.0 dB | Compressor threshold |
| `compratio` | 3.0 | 1.0-20.0 | Compression ratio (3:1) |
| `compknee` | 15.0 | 0.0-30.0 dB | Soft knee width |
| `compmakeup` | 6.0 | -10.0 to 30.0 dB | Makeup gain |
| `compattack` | 0.001 | 0.0001-0.1 s | Attack time constant (effectively instantaneous at 62.5 fps) |
| `comprelease` | 2.0 | 0.01-10.0 s | Release time constant |

### Category: `stability` (1 parameter)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `stabilitywin` | 8.0 | 4-16 | Number of beats for stability tracking |

### Category: `lookahead` (1 parameter)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `lookahead` | 50.0 | 0-200 ms | Beat prediction advance for anticipatory effects |

### Category: `tempo` (1 parameter)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `tempochgthresh` | 0.1 | 0.01-0.5 | Min BPM change ratio to trigger tempo update |

### Category: `agc` (5 parameters) - Hardware Gain Control

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `hwtarget` | 0.35 | 0.05-0.9 | Target raw ADC level |
| `fastagc` | true | bool | Enable fast AGC for low-level sources |
| `fastagcthresh` | 0.15 | 0.05-0.3 | Raw level threshold for fast AGC |
| `fastagcperiod` | 5000 | 2000-15000 ms | Fast AGC calibration period |
| `fastagctau` | 5.0 | 1.0-15.0 s | Fast AGC tracking time |

### Category: `audio` (2 parameters) - Window/Range Normalization

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `peaktau` | 2.0 | 0.5-10.0 s | Peak adaptation speed |
| `releasetau` | 5.0 | 1.0-30.0 s | Peak release speed |

### Category: `fire` (13 parameters) - Fire Visual Effect

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `cooling` | 80 | 0-255 | Base cooling rate |
| `sparkchance` | 0.4 | 0.0-1.0 | Probability of sparks |
| `sparkheatmin` | 160 | 0-255 | Minimum spark heat |
| `sparkheatmax` | 255 | 0-255 | Maximum spark heat |
| `audiosparkboost` | 0.3 | 0.0-1.0 | Audio influence on sparks |
| `coolingaudiobias` | 0 | -128 to 127 | Audio cooling bias |
| `bottomrows` | 2 | 1-8 | Spark injection rows |
| `burstsparks` | 5 | 1-20 | Sparks per burst |
| `suppressionms` | 150 | 50-1000 | Burst suppression time |
| `heatdecay` | 0.85 | 0.5-0.99 | Heat decay rate |
| `emberheatmax` | 20 | 0-50 | Max ember heat |
| `spreaddistance` | 8 | 1-24 | Heat spread distance |
| `embernoisespeed` | 0.0008 | 0.0001-0.002 | Ember animation speed |

### Category: `firemusic` (3 parameters) - Beat-Synced Fire

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `musicemberpulse` | 0.4 | 0.0-1.0 | Ember pulse on beat |
| `musicsparkpulse` | 0.3 | 0.0-1.0 | Spark pulse on beat |
| `musiccoolpulse` | 10.0 | 0.0-30.0 | Cooling oscillation amplitude |

### Category: `fireorganic` (1 parameter) - Non-Rhythmic Fire

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `organictransmin` | 0.3 | 0.0-1.0 | Min transient for burst |

**Total: ~75+ tunable parameters** (registered settings + BandFlux runtime params)

---

## Current Best Settings

### BandFlux Solo Configuration (SETTINGS_VERSION 25)

**Single detector outperforms all multi-detector ensembles** — BandFlux Solo achieves avg Beat F1 0.468 vs 0.411 baseline (HFC+Drummer). Multi-detector combos tested worse; ensemble fusion dilutes BandFlux's cleaner signal.

**Detector table:**

| Detector | Weight | Threshold | Enabled | Reason |
|----------|--------|-----------|---------|--------|
| **BandWeightedFlux** | 0.50 | 0.5 | **yes** | Best solo Beat F1, log-compressed band-weighted flux |
| Drummer | 0.50 | 4.5 | no | Multiplicative threshold fails at low signal levels |
| ComplexDomain | 0.50 | 3.5 | no | Adds noise when combined with BandFlux |
| BassBand | 0.45 | 3.0 | no | Too noisy (100+ detections/30s even at thresh 60) |
| SpectralFlux | 0.20 | 1.4 | no | Fires on pad chord changes |
| HFC | 0.20 | 4.0 | no | Hi-hat detector, max F1=0.620, creates busy visuals |
| Novelty | 0.12 | 2.5 | no | Near-zero detections on real music |

**Fusion parameters (BandFlux Solo):**
```
agree_1 = 1.0          # Single-detector pass-through at full strength
enscooldown = 250      # ms between detections (adaptive: tempo-aware)
ensminconf = 0.40      # Minimum confidence for output
ensminlevel = 0.0      # Noise gate (disabled)
```

**BandFlux algorithm:**
1. Log-compress FFT magnitudes: `log(1 + 20 * mag[k])`
2. 3-bin max-filter (SuperFlux vibrato suppression)
3. Band-weighted half-wave rectified flux (bass 2.0×, mid 1.5×, high 0.1×)
4. Additive threshold: `mean + 0.5` (NOT multiplicative)
5. Onset delta filter: reject if `fluxDelta < 0.3` (pad/echo rejection)
6. Hi-hat rejection gate (high-only flux suppression)

### Bayesian Tempo Fusion Defaults (v24)

| Parameter | Command | Default | Role |
|-----------|---------|---------|------|
| Comb weight | `bayescomb` | 0.7 | **Primary** observation — Scheirer-style resonators |
| FT weight | `bayesft` | 2.0 | Re-enabled v24 (spectral processing fixed normalization) |
| IOI weight | `bayesioi` | 2.0 | Re-enabled v24 (spectral whitening stabilized onsets) |
| ACF weight | `bayesacf` | 0.3 | Low weight prevents sub-harmonic lock |
| Lambda | `bayeslambda` | 0.15 | Transition tightness |
| Prior weight | `bayespriorw` | 0.0 | Static prior OFF (hurts off-center tempos) |
| CBSS threshold | `cbssthresh` | 1.0 | Prevents phantom beats during silence |

### Spectral Pipeline Defaults (v23+)

| Component | Parameters | Purpose |
|-----------|-----------|---------|
| **Compressor** | thresh=-30dB, ratio=3:1, knee=15dB, makeup=+6dB | Normalize gross signal level |
| **Whitening** | decay=0.997, floor=0.001 | Per-bin adaptive normalization |

### Known Limitations

| Pattern | Best F1 | Issue | Visual Impact |
|---------|---------|-------|---------------|
| machine-drum | ~0.22 | Sub-harmonic lock (~120 BPM vs 240) | Low — half-time still looks rhythmic |
| trap-electro | ~0.19 | Syncopated kicks challenge causal tracking | Low — energy-reactive mode acceptable |
| deep-ambience | ~0.40 | Soft ambient onsets below detection threshold | None — organic mode is correct response |
| pad-rejection | ~0.42 | Pad transitions create sharp flux (~16 FPs) | Low — FPs are on-beat, not random |
| DnB tracks | varies | Detected at ~117 BPM (half-time of ~170) | Low — half-time looks acceptable |

---

## Historical Test Results

### Pre-Bayesian Baseline (Feb 21, 2026) — BandFlux Solo, beatoffset=5

| Track | Beat F1 | BPM Acc | Transient F1 |
|-------|:-------:|:-------:|:------------:|
| trance-party | 0.775 | 0.993 | 0.774 |
| minimal-01 | 0.695 | 0.959 | 0.544 |
| infected-vibes | 0.691 | 0.973 | 0.721 |
| goa-mantra | 0.605 | 0.993 | 0.294 |
| minimal-emotion | 0.486 | 0.998 | 0.154 |
| deep-ambience | 0.404 | 0.949 | 0.187 |
| machine-drum | 0.224 | 0.825 | 0.312 |
| trap-electro | 0.190 | 0.924 | 0.298 |
| dub-groove | 0.176 | 0.830 | 0.374 |
| **Average** | **0.472** | **0.938** | **0.406** |

### Bayesian v22 Combined Validation (Feb 25, 2026) — 4-device validated

Defaults: bayesacf=0.3, bayescomb=0.7, bayesft=0, bayesioi=0, bayeslambda=0.15, cbssthresh=1.0

**Average Beat F1: 0.519** (+10% vs pre-Bayesian 0.472)

### Bayesian v24 (Feb 26, 2026) — FT/IOI re-enabled after spectral processing

Defaults: bayesacf=0.3, bayescomb=0.7, bayesft=2.0, bayesioi=2.0, bayeslambda=0.15, cbssthresh=1.0

FT and IOI re-enabled at weight 2.0 — spectral compressor + whitening (v23) fixed normalization issues that made them unreliable in v22.

---

## Comprehensive Test Plan

### Overview

**Goal:** Validate current v24 firmware defaults and tune parameters for optimal beat tracking and transient detection across diverse music genres.

### Prerequisites

1. 4 devices connected: `/dev/ttyACM0`, `/dev/ttyACM1`, `/dev/ttyACM2`, `/dev/ttyACM3`
2. USB speakers connected and volume at 100%: `amixer -c 1 set PCM 35`
3. Quiet testing environment
4. All devices at factory defaults: `reset_defaults` on each

### Phase 1: Baseline Validation (all 18 tracks)

**Purpose:** Establish v24 performance across the full track library.

**Method:** Use `run_music_test` MCP tool per track, one at a time (shared acoustic space — all devices hear same audio). Run full tracks (Bayesian fusion needs >30s to warm up).

```
# For each track:
run_music_test(
  audio_file: "blinky-test-player/music/edm/<track>.mp3",
  ground_truth: "blinky-test-player/music/edm/<track>.beats.json",
  port: "/dev/ttyACM0"
)
```

**Record:** Beat F1, BPM accuracy, transient F1 for each track.

### Phase 2: Bayesian Weight Tuning (if needed)

Use multi-device parallel sweep to test parameter values:

```bash
cd blinky-test-player
npm run tuner -- multi-sweep \
  --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM3 \
  --params bayesacf --duration 30
```

**Key parameters to sweep** (in priority order):
1. `cbssthresh` — Most impactful single parameter (0.5, 0.8, 1.0, 1.2)
2. `bayescomb` — Primary observation weight (0.5, 0.7, 1.0)
3. `bayesacf` — Sub-harmonic prevention (0.0, 0.3, 0.5, 1.0)
4. `bayesft` / `bayesioi` — Secondary observations (0, 1.0, 2.0, 3.0)

**CRITICAL:** Always validate combined defaults after independent sweeps — interaction effects are real (bayesacf=0 looked optimal independently but caused half-time lock when combined).

### Phase 3: Transient Detection Tuning (if needed)

**BandFlux parameters to test:**
1. `bandflux_onsetdelta` — Pad rejection vs kick sensitivity (0.2, 0.3, 0.5)
2. `bandflux_bassweight` — Kick detection weight (1.5, 2.0, 3.0)
3. `bandflux_gamma` — Log compression (10, 20, 30)

**Use synthetic patterns** for transient tuning:
```
run_test(pattern: "strong-beats", port: "/dev/ttyACM0")
run_test(pattern: "pad-rejection", port: "/dev/ttyACM0")
```

### Phase 4: Output Modulation (visual tuning)

**Parameters:** `pulseboost`, `pulsesuppress`, `energyboost`

Visual inspection of fire effect with beat-synced music:
- On-beat sparks visibly brighter
- Off-beat transients subdued but visible
- No visual jarring/flickering

### Phase 5: Save and Verify

1. Save settings on each device: `save`
2. Run validation pass on 3-5 tracks to confirm
3. Update documentation with new baseline

---

## Troubleshooting

### No Audio Detection

1. Check `stream on` shows audio data (PDM working)
2. Verify hardware gain is reasonable (20-60 range): `show gain`
3. Check raw level rises with sound
4. Verify BandFlux is enabled: `show detectors`

### Too Many False Positives

1. Raise BandFlux threshold: `set detector_thresh bandflux 0.7`
2. Increase onset delta: `set bandflux_onsetdelta 0.5` (rejects more pads/echoes)
3. Raise `ensminconf` (try 0.50-0.60)
4. Increase `enscooldown` to reduce rapid triggers (try 300-400ms)
5. Check if experimental gates help: `set bandflux_crestgate 5.0`

### Missing Transients

1. Lower BandFlux threshold: `set detector_thresh bandflux 0.3`
2. Decrease onset delta: `set bandflux_onsetdelta 0.1` (lets more through)
3. Lower `ensminconf` (try 0.3)
4. Ensure `enscooldown` isn't too long for fast patterns
5. Check AGC is tracking properly: `show gain`

### Rhythm Not Locking

1. Verify steady beat in audio
2. Check `musicthresh` isn't too high (try 0.3)
3. Ensure BPM is within `bpmmin`/`bpmmax` range
4. Allow 30+ seconds for Bayesian fusion to converge
5. Check tempo: `show beat` or `get_beat_state` MCP tool

### BPM Detected at Half-Time (e.g., 170 BPM → 85 BPM)

1. This is a known limitation — autocorrelation harmonics are stronger at sub-harmonics
2. Per-sample ACF harmonic disambiguation handles most cases (2x and 1.5x checks)
3. Increase `bayesacf` weight slightly (try 0.5) — provides more periodicity signal
4. For DnB (170+ BPM): Half-time detection is expected and visually acceptable
5. **DO NOT** enable ongoing static prior (`bayespriorw`) as a fix — it hurts tracks far from the prior center

### Phantom Beats During Silence/Breakdowns

1. Increase `cbssthresh` (try 1.2-1.5) — higher threshold rejects weak CBSS peaks
2. **NEVER** lower cbssthresh below 0.8 when FT/IOI weights > 0.5 (catastrophic failure)
3. Check that `odfmeansub` is ON (essential for Bayesian fusion)

### FT/IOI Causing Problems

1. If FT/IOI are causing sub-harmonic issues, disable them: `set ft 0` and `set ioi 0`
2. Or reduce their Bayesian weights: `set bayesft 0.5` and `set bayesioi 0.5`
3. Ensure `cbssthresh >= 1.0` before increasing FT/IOI weights
4. FT and IOI depend on spectral processing — if compressor/whitening are disabled, disable FT/IOI too

### Serial Commands Reference

```bash
show beat          # CBSS beat tracker state (BPM, phase, confidence)
show detectors     # All detector statuses, weights, thresholds
show bandflux_*    # BandFlux-specific parameters
json beat          # Beat tracker state as JSON
json rhythm        # Full rhythm tracking state as JSON
json settings      # All settings as JSON
stream on          # Start audio streaming (~20 Hz)
stream fast        # Start fast audio streaming
stream off         # Stop streaming
```

**MCP Tools:**
- `get_beat_state` — BPM, phase, confidence, periodicity, beatCount, stability
- `monitor_audio` — Audio levels, transient count, music mode status
- `monitor_transients` — Raw transient detection stats
- `monitor_music` — BPM tracking accuracy over duration
- `run_test` — Play pattern and record detections
- `run_music_test` — Full track evaluation with ground truth

---

## Appendix: Removed Parameters

### Removed in SETTINGS_VERSION 18-24 (Bayesian Fusion, Feb 2026)

| Old Parameter | Old Component | Reason |
|---------------|---------------|--------|
| `hitthresh` | TransientDetector | Moved to per-detector thresholds |
| `attackmult` | TransientDetector | Moved to per-detector settings |
| `avgtau` | TransientDetector | Moved to per-detector settings |
| `cooldown` | TransientDetector | Replaced by `enscooldown` |
| `adaptthresh` | TransientDetector | Replaced by ensemble architecture |
| `adaptminraw` | TransientDetector | Replaced by ensemble architecture |
| `adaptmaxscale` | TransientDetector | Replaced by ensemble architecture |
| `adaptblend` | TransientDetector | Replaced by ensemble architecture |
| `priorenabled` | AudioController | Replaced by `bayespriorw` (0 = off) |
| `priorcenter` | AudioController | Replaced by `bayesprior` |
| `priorstrength` | AudioController | Replaced by `bayespriorw` |
| `phaseadapt` | AudioController | Phase is now deterministic (counter-based) |
| `temposnap` | AudioController | Bayesian fusion handles tempo transitions |
| `maxbpmchg` | AudioController | Bayesian fusion handles tempo stability |
| `hyfluxwt` | TransientDetector | Hybrid mode replaced by ensemble architecture |
| `hydrumwt` | TransientDetector | Hybrid mode replaced by ensemble architecture |
| `hybothboost` | TransientDetector | Hybrid mode replaced by ensemble architecture |
| `hpsEnabled` | AudioController | HPS tested and rejected (Feb 2022) |
| `pulseTrainEnabled` | AudioController | Pulse train tested and rejected (Feb 2022) |
| `pulseTrainCandidates` | AudioController | Pulse train tested and rejected |
| `combCrossValMinConf` | AudioController | Comb bank now feeds Bayesian fusion directly |
| `combCrossValMinCorr` | AudioController | Comb bank now feeds Bayesian fusion directly |
| `ioiMinPeakRatio` | AudioController | IOI now feeds Bayesian fusion directly |
| `ioiMinAutocorr` | AudioController | IOI now feeds Bayesian fusion directly |
| `ftMinMagnitudeRatio` | AudioController | FT now feeds Bayesian fusion directly |
| `ftMinAutocorr` | AudioController | FT now feeds Bayesian fusion directly |

### Removed in AudioController v2/v3 (December 2025)

| Old Parameter | Old Component | Reason |
|---------------|---------------|--------|
| `musicbeats` | MusicMode PLL | Event-based activation replaced by autocorrelation strength |
| `musicmissed` | MusicMode PLL | No beat event counting in new architecture |
| `phasesnap` | MusicMode PLL | PLL replaced by autocorrelation |
| `snapconf` | MusicMode PLL | No longer needed |
| `stablephase` | MusicMode PLL | Phase derived from autocorrelation |
| `confinc` | MusicMode PLL | Confidence replaced by periodicityStrength |
| `confdec` | MusicMode PLL | Confidence replaced by periodicityStrength |
| `misspenalty` | MusicMode PLL | No beat event counting |
| `pllkp` | MusicMode PLL | No PLL in new architecture |
| `pllki` | MusicMode PLL | No PLL in new architecture |
| `combdecay` | RhythmAnalyzer | Merged into AudioController autocorrelation |
| `combfb` | RhythmAnalyzer | Merged into AudioController autocorrelation |
| `combconf` | RhythmAnalyzer | Merged into AudioController autocorrelation |
| `histblend` | RhythmAnalyzer | Merged into AudioController autocorrelation |
| `rhythmminbpm` | RhythmAnalyzer | Replaced by `bpmmin` |
| `rhythmmaxbpm` | RhythmAnalyzer | Replaced by `bpmmax` |
| `rhythminterval` | RhythmAnalyzer | Replaced by `autocorrperiod` |
| `beatthresh` | RhythmAnalyzer | Replaced by `musicthresh` |
| `minperiodicity` | RhythmAnalyzer | Merged into `musicthresh` logic |

**If you see these parameters in old documentation, ignore them. They have been removed from both firmware and testing tools.**
