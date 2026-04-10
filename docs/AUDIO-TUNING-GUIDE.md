# Audio Tuning Guide

**Last Updated:** April 9, 2026
**Firmware Version:** SETTINGS_VERSION 94 (AudioTracker: ACF + PLP with multi-source ACF + pattern slot cache. Conv1D W16 v16 onset model deployed. NN-modulated pulse via nnConf).

> **NOTE:** Parameter categories marked ~~strikethrough~~ below were removed in v67-v82 (EnsembleDetector, CBSS, Bayesian fusion, AGC). See `docs/IMPROVEMENT_PLAN.md` for removal history. Current tunable parameters are in the AudioTracker section.

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
   AdaptiveMic (fixed gain + window/range normalization, AGC removed v72)
        |
   SharedSpectralAnalysis (FFT-256)
   ├── Soft-knee compressor (Giannoulis 2012)
   ├── Per-bin adaptive whitening (Stowell & Plumbley 2007)
   └── Spectral flux (HWR: sum of positive magnitude changes, NN-independent)
        |
        ├── [BPM PATH]
        │   Spectral flux → contrast sharpening → OSS buffer (~5.5s)
        │   → Autocorrelation (every 150ms) → period estimate
        │
        ├── [ONSET PATH]
        │   FrameOnsetNN (Conv1D W16, ~7ms, single channel)
        │   → onset_activation → NN-modulated spectral flux → pulse envelope
        │   → Energy synthesis (onset peak-hold blend)
        │
        └── [PHASE PATH]
            Multi-source ACF (beat-level lags 20-80, 3 sources)
            Sources: spectral flux, bass energy, NN onset
            → Bar multipliers (2×/3×/4×) + epoch-fold variance scoring
            → Direct pattern interpolation → plpPulse
            → Pattern slot cache (4-slot LRU, instant section recall)
            → Silence state reset after 5s (clears all analysis buffers)
        |
   AudioControl { energy, pulse, phase, plpPulse, rhythmStrength, onsetDensity }
        |
   Fire/Water/PlasmaGlobe Generators (visual effects)
```

### Key Design Decisions

1. **Decoupled BPM and onset paths**: BPM estimation uses spectral flux (NN-independent). The NN detects acoustic onsets (kicks/snares) but cannot distinguish on-beat from off-beat — syncopated transients would corrupt ACF periodicity. Spectral flux preserves periodic structure.
2. **NN-modulated pulse** (b108): `control_.pulse` uses spectral flux weighted by NN activation, self-tuning via `nnConf` (activation variance). Sharp NN → onset-selective pulse; flat NN → falls back to raw spectral flux. Conv1D W16 v16 model. Non-NN fallback: mic level.
3. **Spectral conditioning**: Soft-knee compressor normalizes gross signal level; per-bin whitening for spectral normalization. AGC removed v72 — hardware gain fixed at platform optimal.
4. **PLP phase/pattern extraction** (v93): Multi-source ACF across 3 mean-subtracted sources selects period. Epoch-fold extracts repeating energy pattern at detected period → direct pattern interpolation for visual output.
5. **Pattern slot cache** (v82): 4-slot LRU of 16-bin PLP pattern digests. Every bar, current PLP pattern compared via cosine similarity. Match > 0.70 triggers instant recall from cache. Enables rapid verse/chorus switching.
6. **Onset information gate**: Suppresses low-confidence NN output before PLP source input and pulse detection, preventing noise-driven false patterns during silence/breakdowns.
7. **6-parameter output**: Generators receive `AudioControl` struct with energy, pulse, phase, plpPulse, rhythmStrength, onsetDensity.

---

## Testing Infrastructure

### Components

| Component | Location | Purpose |
|-----------|----------|---------|
| **blinky-server** | `blinky-server/` | Fleet management, test orchestration, scoring (REST API) |
| **blinky-serial-mcp** | `blinky-serial-mcp/` | MCP tools for Claude (thin HTTP client for blinky-server) |
| **test audio** | `blinky-test-player/music/` | Audio files + ground truth labels |

### Quick Commands

```bash
# Run validation suite via server API
curl -X POST http://blinkyhost.local:8420/api/test/validate \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["DEVICE_ID"], "track_dir": "/path/to/music/edm"}'

# Parameter sweep via server API
curl -X POST http://blinkyhost.local:8420/api/test/param-sweep \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["D1","D2","D3"], "param_name": "onsetthresh", "values": [1,2,3,4,5,6,7,8,9], "track_dir": "/path/to/music/edm"}'

# Via MCP tools
run_test(port: "/dev/ttyACM0")
run_validation_suite(ports: ["/dev/ttyACM0", "/dev/ttyACM1"])

# Monitor audio levels
monitor_audio(port: "/dev/ttyACM0", duration_ms: 3000)

# Monitor transient detections
monitor_transients(port: "/dev/ttyACM0", duration_ms: 5000)
```

### Testing Methodology

**Critical: Always seek to middle of track.** EDM tracks typically have 15-30s intros with no beat. The server uses `track_manifest.json` seek offsets to jump to the densest beat region. The first 12 seconds of data are filtered out (ACF convergence settle time).

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
*Detector commands removed in v67. BandFlux/EnsembleDetector fully removed. NN models are the sole ODF source (OnsetNN for pulse, RhythmNN for beat/downbeat). See git log v67 for details.*

### Category: `rhythm` - Beat Tracking (AudioController)

**Onset Strength Signal:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `odfsmooth` | 5 | 3-11 (odd) | ODF smoothing window width |
| `odfmeansub` | false | bool | ODF mean subtraction (OFF since v32 — raw ODF preserves ACF structure, +70% F1) |

**Tempo estimation:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `combbankenabled` | true | bool | Enable comb filter bank for tempo |
| `combbankfeedback` | 0.92 | 0.85-0.98 | Comb bank resonance strength |
| `autocorrperiod` | 250 | 100-1000 ms | Autocorrelation computation interval |
| `bpmmin` | 60 | 40-120 | Minimum BPM to detect |
| `bpmmax` | 200 | 80-240 | Maximum BPM to detect |
| `temposmooth` | 0.85 | 0.5-0.99 | Tempo EMA smoothing factor |
| `ft` | false | bool | Fourier tempogram observation (disabled v28) |
| `ioi` | false | bool | IOI histogram observation (disabled v28) |

**CBSS beat detection:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `cbssalpha` | 0.9 | 0.5-0.99 | CBSS weighting (higher = more predictive) |
| `cbsstight` | 8.0 | 1.0-20.0 | Log-Gaussian tightness (higher = stricter tempo, raised v40) |
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
| `bayeslambda` | 0.07 | 0.01-1.0 | Transition tightness (lower = more rigid tempo, tightened v25) |
| `bayesprior` | 128.0 | 60-200 | Static prior center BPM |
| `bayespriorw` | 0.0 | 0.0-3.0 | Ongoing static prior strength (0 = off, default) |
| `priorwidth` | 50.0 | 10-80 | Prior width (sigma BPM) |
| `bayesacf` | 0.8 | 0.0-5.0 | ACF observation weight (raised in v25 — harmonic comb makes ACF reliable) |
| `bayesft` | 0.0 | 0.0-5.0 | Fourier tempogram weight (disabled v28) |
| `bayescomb` | 0.7 | 0.0-5.0 | Comb filter bank weight (primary observation) |
| `bayesioi` | 0.0 | 0.0-5.0 | IOI histogram weight (disabled v28) |

**CRITICAL interaction warning:** Setting `cbssthresh` below 0.8 while `bayesft` or `bayesioi` are above 0.5 causes catastrophic beat tracking failure (F1 drops to 0.049). The FT/IOI sub-harmonic bias floods CBSS with phantom beats that the low threshold can't reject. Both are disabled by default (v28). If re-enabled: `cbssthresh >= 1.0` with any FT/IOI weight, or `bayesft/bayesioi <= 0.5` with any cbssthresh.

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

### Category: `nn` (1 parameter) - NN Onset Detection

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `nnprofile` | 0 | bool | Enable [NNPROF] per-operator timing output to Serial |

### Category: `v45` — REMOVED (PLL removed v80, replaced by PLP)

PLL parameters (`pll`, `pllkp`, `pllki`, `onsetSnapWindow`) removed. PLP Fourier tempogram handles phase alignment.

### Category: `octave` (5 parameters) - Octave Disambiguation (v32)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `densityoctave` | 1 | bool | Onset-density octave penalty |
| `octavecheck` | 1 | bool | Shadow CBSS octave checker |
| `octavecheckbeats` | 2 | 1-8 | Octave check interval in beats |
| `octavescoreratio` | 1.3 | 1.0-3.0 | Required score improvement for octave switch |
| `odfmeansub` | 0 | bool | ODF mean subtraction (OFF v32: raw ODF +70% F1) |

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

**Total: ~30+ tunable parameters** (v67, after BandFlux removal)

---

## Current Best Settings

### Onset Detection & BPM Configuration (March 2026)

**Architecture:** Decoupled tempo/onset. BPM uses spectral flux (NN-independent). NN onset used for visual pulse + PLP source.
- **BPM signal:** Spectral flux (half-wave rectified magnitude change) → contrast² → ACF → BPM
- **Onset detection:** FrameOnsetNN, Conv1D W16 (256ms), [24,32] channels, 13.4 KB INT8, 6.8ms nRF52840 / 5.8ms ESP32-S3. Single output: onset activation (kicks/snares). v3 deployed: All Onsets F1=0.787 (Kick 0.688, Snare 0.773).
- **Training data:** Consensus v5 labels (7-system), cal63 mel calibration (target_rms_db=-63 dB).

### AudioTracker Tempo Defaults (v74+)

| Parameter | Command | Default | Role |
|-----------|---------|---------|------|
| BPM min | `bpmmin` | 60 | Minimum detectable BPM |
| BPM max | `bpmmax` | 200 | Maximum detectable BPM |
| Tempo smoothing | `temposmooth` | 0.85 | BPM EMA smoothing (higher = slower) |
| Activation threshold | `activationthreshold` | 0.3 | Min periodicity to activate rhythm mode |
| ODF gate | `odfgate` | 0.25 | NN output floor gate (suppress noise) |
| Slot switch threshold | `slotswitchthresh` | 0.70 | Cosine sim to recall cached slot |
| Slot new threshold | `slotnewthresh` | 0.40 | Below this: allocate new slot |
| Slot update rate | `slotupdaterate` | 0.15 | EMA rate for reinforcing active slot |
| Slot save confidence | `slotsaveconf` | 0.50 | Min PLP confidence to save/update slots |
| Slot seed blend | `slotseedblend` | 0.70 | Blend ratio when seeding from cached slot |

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

| Track | On-device F1 | BPM Acc | Transient F1 |
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

Defaults (v22): bayesacf=0.3, bayescomb=0.7, bayesft=0, bayesioi=0, bayeslambda=0.15, cbssthresh=1.0

**Average on-device F1: 0.519** (+10% vs pre-Bayesian 0.472)

### Bayesian v24 (Feb 26, 2026) — FT/IOI re-enabled after spectral processing

Defaults (v24): bayesacf=0.3, bayescomb=0.7, bayesft=2.0, bayesioi=2.0, bayeslambda=0.15, cbssthresh=1.0

FT and IOI re-enabled at weight 2.0 — spectral compressor + whitening (v23) fixed normalization issues that made them unreliable in v22.

### Bayesian v25 (Feb 27, 2026) — BTrack-style harmonic comb ACF + Rayleigh prior

Defaults (v25): bayesacf=0.8, bayescomb=0.7, bayesft=2.0, bayesioi=2.0, bayeslambda=0.07, cbssthresh=1.0

Key changes from v24:
- **Harmonic comb ACF**: 4-harmonic summation with spread windows replaces single-point ACF observation. Fundamental gets 4x advantage over sub-harmonics.
- **Rayleigh tempo prior**: Perceptual weighting peaked at ~120 BPM, applied within ACF observation.
- **ACF weight raised**: 0.3→0.8 — harmonic enhancement makes ACF a reliable signal.
- **Lambda tightened**: 0.15→0.07 — prevents octave jumps accumulating over hundreds of frames.
- **Bidirectional disambiguation**: Added 0.5x downward check (was only 2x/1.5x upward).

---

## Comprehensive Test Plan

### Overview

**Goal:** Validate current v64 firmware defaults and tune parameters for optimal beat tracking and transient detection across diverse music genres.

### Prerequisites

1. 4 devices connected: `/dev/ttyACM0`, `/dev/ttyACM1`, `/dev/ttyACM2`, `/dev/ttyACM3`
2. USB speakers connected and volume at 100%: `amixer -c 1 set PCM 35`
3. Quiet testing environment
4. All devices at factory defaults: `reset_defaults` on each

### Phase 1: Baseline Validation (all 18 tracks)

**Purpose:** Establish v60 performance across the full track library.

**Method:** Use `run_music_test` MCP tool per track (async — poll with `check_test_result`), one at a time (shared acoustic space — all devices hear same audio). Run full tracks (PLP needs >10s to warm up).

```
# For each track:
run_music_test(
  audio_file: "blinky-test-player/music/edm/<track>.mp3",
  ground_truth: "blinky-test-player/music/edm/<track>.beats.json",
  port: "/dev/ttyACM0"
)
```

**Record:** Onset F1, BPM accuracy, transient F1 for each track.

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

### Phase 3: Output Modulation (visual tuning)

**Parameters:** `pulseboost`, `pulsesuppress`, `energyboost`

Visual inspection of fire effect with beat-synced music:
- On-beat sparks visibly brighter
- Off-beat transients subdued but visible
- No visual jarring/flickering

### Phase 4: Save and Verify

1. Save settings on each device: `save`
2. Run validation pass on 3-5 tracks to confirm
3. Update documentation with new baseline

---

## Troubleshooting

### No Audio Detection

1. Check `stream on` shows audio data (PDM working)
2. Verify hardware gain is reasonable (20-60 range): `show gain`
3. Check raw level rises with sound
4. Check NN status: `show nn`

### Too Many False Positives (pulse/spark effects)

1. Pulse detection uses ODF threshold against running mean — check if NN model is producing clean activations
2. Check AGC is tracking properly: `show gain`
3. Retrain model with cleaner labels if persistent

### Missing Transients

1. Check AGC is tracking properly: `show gain`
2. Verify NN model is loaded and producing activations: `show nn`
3. Check mel calibration — training target_rms_db must match firmware AGC levels

### Rhythm Not Locking

1. Verify steady beat in audio
2. Check `musicthresh` isn't too high (try 0.3)
3. Ensure BPM is within `bpmmin`/`bpmmax` range
4. Allow 10+ seconds for PLP to converge
5. Check tempo: `show beat` or `get_beat_state` MCP tool

### BPM Detected at Half-Time (e.g., 170 BPM → 85 BPM)

1. v25 added BTrack-style harmonic comb ACF + Rayleigh prior to prevent this
2. Per-sample ACF harmonic disambiguation handles most cases (2x, 1.5x, and 0.5x checks)
3. Tighter lambda (0.07) makes octave jumps near-impossible once locked
4. For DnB (170+ BPM): Half-time detection is expected and visually acceptable
5. **DO NOT** enable ongoing static prior (`bayespriorw`) as a fix — it hurts tracks far from the prior center

### Phantom Beats During Silence/Breakdowns

1. Increase `cbssthresh` (try 1.2-1.5) — higher threshold rejects weak CBSS peaks
2. **NEVER** lower cbssthresh below 0.8 when FT/IOI weights > 0.5 (catastrophic failure)
3. Check that `odfmeansub` is OFF (v32: raw ODF preserves ACF structure, +70% F1)

### FT/IOI Causing Problems

FT and IOI are **disabled by default** since v28 (`bayesft=0`, `bayesioi=0`). No reference system uses these for real-time beat tracking. If re-enabled for experimentation:
1. Disable them: `set bayesft 0` and `set bayesioi 0`
2. Ensure `cbssthresh >= 1.0` before increasing FT/IOI weights
3. FT and IOI depend on spectral processing — if compressor/whitening are disabled, disable FT/IOI too

### Serial Commands Reference

```bash
show beat          # CBSS beat tracker state (BPM, phase, confidence)
show nn            # NN model status (ready, arena usage, channels)
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

### Removed in SETTINGS_VERSION 64 (v64 dead code removal, March 2026)

| Old Parameter | Old Component | Reason |
|---------------|---------------|--------|
| `fwdfilter` | Forward Filter | A/B tested, severe half-time bias — removed |
| `fwdtranssigma` | Forward Filter | Removed with forward filter |
| `fwdfiltcontrast` | Forward Filter | Removed with forward filter |
| `fwdfiltlambda` | Forward Filter | Removed with forward filter |
| `fwdfiltfloor` | Forward Filter | Removed with forward filter |
| `fwdbayesbias` | Forward Filter | Removed with forward filter |
| `fwdasymmetry` | Forward Filter | Removed with forward filter |
| `fwdphase` | Forward Phase | Removed with forward filter |
| `templatecheck` | Octave Disambiguation | A/B tested, no net benefit — removed |
| `subbeatcheck` | Octave Disambiguation | A/B tested, no net benefit — removed |
| `noiseest` | Noise Estimation | A/B tested, hurts BPM accuracy — default OFF (settings still exposed) |
| `noisesmooth` | Noise Estimation | Default OFF with noise estimation |
| `noiserelease` | Noise Estimation | Default OFF with noise estimation |
| `noiseover` | Noise Estimation | Default OFF with noise estimation |
| `noisefloor` | Noise Estimation | Default OFF with noise estimation |
| `adaptight` | Adaptive Tightness | Removed (dead code) |
| `percival` | Percival Harmonic | Removed (dead code) |
| `bisnap` | Bidirectional Snap | Removed (dead code) |
| `pfNoise` | Particle Filter | Removed (dead code) |
| `pfBeatSigma` | Particle Filter | Removed (dead code) |
| `pfParticles` | Particle Filter | Removed (dead code) |
| `barPointerHmm` | HMM Phase Tracker | Removed (dead code) |
| `hmmContrast` | HMM Phase Tracker | Removed (dead code) |
| `hmmLambda` | HMM Phase Tracker | Removed (dead code) |
| `odfSource` | ODF Source Select | Removed (dead code) |
| `plpphase` | PLP Phase | Removed (dead code) |
| `plpstrength` | PLP Phase | Removed (dead code) |
| `plpminconf` | PLP Phase | Removed (dead code) |
| `fold32` | Misc | Removed (dead code) |
| `sesquicheck` | Misc | Removed (dead code) |
| `harmonicsesqui` | Misc | Removed (dead code) |
| `downwardcorrect` | Misc | Removed (dead code) |

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
