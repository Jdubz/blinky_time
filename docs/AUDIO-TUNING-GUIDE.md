# Audio Tuning Guide

**Last Updated:** February 14, 2026
**Architecture Version:** AudioController with CBSS Beat Tracking

This document consolidates all audio testing and tuning information for the Blinky audio-reactive LED system.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Testing Infrastructure](#testing-infrastructure)
3. [All Tunable Parameters](#all-tunable-parameters)
4. [Current Best Settings](#current-best-settings)
5. [Historical Test Results](#historical-test-results)
6. [Comprehensive Test Plan (2-3 hours)](#comprehensive-test-plan)
7. [Troubleshooting](#troubleshooting)

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
   SharedSpectralAnalysis (FFT-256, mel bands, whitening)
        |
   EnsembleDetector (6 detectors, weighted fusion)
   ├── HFC (0.60) ─────── enabled  ← percussive attacks
   ├── Drummer (0.40) ─── enabled  ← amplitude transients
   ├── SpectralFlux ───── disabled (fires on chord changes)
   ├── BassBand ────────── disabled (room noise issues)
   ├── ComplexDomain ───── disabled (adds sparse FPs)
   └── Novelty ─────────── disabled (net negative F1)
        |
   Fusion: agree_1=0.2, cooldown=250ms, minconf=0.55
        |
   AudioController
   ├── OSS Buffer (6 seconds, 360 samples @ 60Hz)
   ├── Autocorrelation (every 500ms) → Best BPM (with tempo prior)
   ├── CBSS Buffer (cumulative beat strength signal)
   ├── Counter-Based Beat Detection (deterministic phase)
   └── Output Synthesis
        |
   AudioControl { energy, pulse, phase, rhythmStrength }
        |
   Fire/Water/Lightning Generators (visual effects)
```

### Key Design Decisions

1. **CBSS beat tracking**: Cumulative Beat Strength Signal combines onset with predicted beat history
2. **Deterministic phase**: Phase derived from counter: `(now - lastBeat) / period` — no drift or jitter
3. **Pattern-based rhythm**: Uses 6-second autocorrelation buffer with tempo prior
4. **Transients → pulse only**: Transient detection affects visual pulse, NOT beat tracking
5. **Counter-based beats**: Expected at `lastBeat + period`, with forced beats during dropouts
6. **Unified 4-parameter output**: Generators receive simple `AudioControl` struct

---

## Testing Infrastructure

### Components

| Component | Location | Purpose |
|-----------|----------|---------|
| **blinky-serial-mcp** | `blinky-serial-mcp/` | MCP server for device communication (20 tools) |
| **blinky-test-player** | `blinky-test-player/` | Audio pattern playback + ground truth generation |
| **param-tuner** | `blinky-test-player/src/param-tuner/` | Binary search + sweep optimization |
| **test-results/** | `test-results/` | Historical test results (JSON) |

### Quick Commands

```bash
# List available test patterns
npx blinky-test-player list

# Run a single pattern test via MCP
run_test --pattern strong-beats --port COM5 --gain 40

# Fast binary search tuning (~30 min)
cd blinky-test-player
npm run tuner -- fast --port COM5 --gain 40

# Full validation suite
npm run tuner -- validate --port COM5 --gain 40
```

### Test Patterns (18 Total)

| Category | Patterns | Purpose |
|----------|----------|---------|
| **Baseline** | strong-beats, medium-beats, soft-beats | Basic transient detection |
| **Rejection** | hat-rejection, pad-rejection, chord-rejection | False positive testing |
| **Tempo** | fast-tempo, tempo-sweep | Speed and tempo detection |
| **Complexity** | bass-line, synth-stabs, lead-melody, full-mix | Real-world complexity |
| **Edge Cases** | sparse, simultaneous | Silence gaps, overlapping hits |

---

## All Tunable Parameters

### Category: `transient` (8 parameters) - Transient Detection

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `hitthresh` | 2.813 | 1.0-10.0 | Hit threshold (multiples of recent average) |
| `attackmult` | 1.1 | 1.0-2.0 | Attack multiplier (sudden rise ratio) |
| `avgtau` | 0.8 | 0.1-5.0 | Recent average tracking time (seconds) |
| `cooldown` | 80 | 20-500 | Cooldown between hits (ms) |
| `adaptthresh` | false | bool | Enable adaptive threshold scaling |
| `adaptminraw` | 0.1 | 0.01-0.5 | Raw level to start scaling |
| `adaptmaxscale` | 0.6 | 0.3-1.0 | Minimum threshold scale factor |
| `adaptblend` | 5.0 | 1.0-15.0 | Adaptive threshold blend time (s) |

### Category: `ensemble` (10+ parameters) - Ensemble Detection

**Detector enable/disable:**
| Command | Default | Description |
|---------|---------|-------------|
| `set detector_enable <type> <0\|1>` | varies | Enable/disable detector (drummer, spectral, hfc, bass, complex, novelty) |
| `set detector_weight <type> <val>` | varies | Set detector weight in fusion |
| `set detector_thresh <type> <val>` | varies | Set detector threshold |

**Fusion parameters:**
| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `ensemble_cooldown` | 250 | 20-500 | Cooldown between detections (ms) |
| `ensemble_minconf` | 0.55 | 0.1-1.0 | Minimum confidence for detection output |
| `agree_<N>` | varies | 0.0-1.5 | Agreement boost for N detectors firing (0-6) |

**Per-detector defaults:**
| Detector | Weight | Threshold | Enabled |
|----------|--------|-----------|---------|
| drummer | 0.40 | 3.5 | yes |
| hfc | 0.60 | 4.0 | yes |
| spectral | 0.20 | 1.4 | no |
| bass | 0.18 | 3.0 | no |
| complex | 0.13 | 2.0 | no |
| novelty | 0.12 | 2.5 | no |

**Note:** The old `detectmode` parameter and mode-based detection (Mode 0-4) were removed in December 2025. All detection now uses the ensemble architecture with individually-enabled detectors.

### Category: `rhythm` (7 parameters) - Beat Tracking

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `musicthresh` | 0.4 | 0.0-1.0 | Rhythm activation threshold (periodicity strength) |
| `phaseadapt` | 0.15 | 0.01-1.0 | Phase adaptation rate |
| `pulseboost` | 1.3 | 1.0-2.0 | Pulse boost on beat |
| `pulsesuppress` | 0.6 | 0.3-1.0 | Pulse suppress off beat |
| `energyboost` | 0.3 | 0.0-1.0 | Energy boost on beat |
| `bpmmin` | 60 | 40-120 | Minimum BPM to detect |
| `bpmmax` | 200 | 80-240 | Maximum BPM to detect |

### Category: `tempoprior` (4 parameters) - Half-Time/Double-Time Disambiguation

**CRITICAL:** Tempo prior MUST be enabled for correct BPM tracking. Without it, autocorrelation prefers half-time peaks and 120 BPM will be detected as ~60 BPM.

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `priorenabled` | **on** | on/off | Enable tempo prior weighting (MUST be on) |
| `priorcenter` | 120 | 60-200 | Center of Gaussian prior (BPM) |
| `priorwidth` | 60 | 10-100 | Width (sigma) of prior - larger = less bias |
| `priorstrength` | 0.5 | 0.0-1.0 | Blend: 0=no prior, 1=full prior weight |

**Tested BPM Accuracy (January 2026):**

| Actual BPM | Prior OFF | Prior ON (120±60) | Notes |
|------------|-----------|-------------------|-------|
| 60 BPM     | 67        | ~80               | Pulled toward center |
| 80 BPM     | -         | ~100              | Pulled toward center |
| **120 BPM**| 68        | **~120**          | ✓ Works well |
| 160 BPM    | 80        | ~85               | Half-time bias |
| 180 BPM    | 104       | ~100              | Half-time bias |

**Optimal Range:** 90-140 BPM works reliably. Outside this range, autocorrelation harmonics dominate.

**Trade-offs:**
- Narrow prior (width=40-50): Strongest bias toward center, may distort extreme tempos
- Wide prior (width=60+): Better for 60 BPM, slightly less precise at 120 BPM
- 160+ BPM: Fundamentally problematic - autocorrelation finds stronger peaks at half-time

**Known Limitation:** Fast tempos (160+ BPM) tend to detect at half-time because autocorrelation naturally produces strong peaks at subharmonics (every other beat). Fixing this would require "double-time promotion" logic.

### Category: `tempo` (2 parameters) - Continuous Tempo Estimation

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `temposmooth` | 0.85 | 0.5-0.99 | Tempo smoothing factor (higher = smoother) |
| `tempochgthresh` | 0.1 | 0.01-0.5 | Min BPM change ratio to trigger update |

### Category: `stability` (1 parameter) - Beat Stability Tracking

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `stabilitywin` | 8 | 4-16 | Number of beats to track for stability |

### Category: `lookahead` (1 parameter) - Beat Prediction

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `lookahead` | 50 | 0-100 | How far ahead to predict beats (ms) |

### Category: `cbss` (4 parameters) - CBSS Beat Tracking

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `cbssalpha` | 0.9 | 0.5-0.99 | CBSS weighting (higher = more predictive) |
| `cbsstight` | 5.0 | 1.0-20.0 | Log-Gaussian tightness (higher = stricter tempo) |
| `beatconfdecay` | 0.98 | 0.9-0.999 | Per-frame confidence decay when no beat |
| `temposnap` | 0.15 | 0.05-0.5 | BPM change ratio to snap vs smooth |

**Serial Commands:**
- `show beat` - View CBSS beat tracker state
- `json beat` - Get beat tracker state as JSON
- `json rhythm` - Get full rhythm tracking state as JSON

**MCP Tool:**
- `get_beat_state` - Retrieves BPM, phase, confidence, periodicity, beatCount, stability

### Category: `agc` (5 parameters) - Hardware Gain Control

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `hwtarget` | 0.35 | 0.05-0.9 | Target raw ADC level |
| `fastagc` | true | bool | Enable fast AGC for low-level sources |
| `fastagcthresh` | 0.15 | 0.05-0.3 | Raw level threshold for fast AGC |
| `fastagcperiod` | 5000 | 2000-15000 | Fast AGC calibration period (ms) |
| `fastagctau` | 5.0 | 1.0-15.0 | Fast AGC tracking time (s) |

### Category: `audio` (2 parameters) - Window/Range Normalization

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `peaktau` | 2.0 | 0.5-10.0 | Peak adaptation speed (s) |
| `releasetau` | 5.0 | 1.0-30.0 | Peak release speed (s) |

### Category: `spectral` (10 parameters) - Spectral Processing (v23+)

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `whitenenabled` | true | bool | Per-bin spectral whitening (adaptive normalization) |
| `whitendecay` | 0.997 | 0.99-1.0 | Per-frame peak decay (~5s memory at 0.997) |
| `whitenfloor` | 0.001 | 0.0001-0.01 | Noise floor for whitening (avoids amplifying silence) |
| `compressorenabled` | true | bool | Soft-knee compressor before whitening |
| `compthreshold` | -30 | -60-0 | Compressor threshold (dB) |
| `compratio` | 3.0 | 1.0-20.0 | Compression ratio (e.g., 3:1) |
| `compknee` | 15.0 | 0.0-30.0 | Soft knee width (dB) |
| `compmakeup` | 6.0 | 0.0-20.0 | Makeup gain (dB) |
| `compattack` | 0.001 | 0.0-0.1 | Attack time constant (seconds) |
| `comprelease` | 2.0 | 0.1-10.0 | Release time constant (seconds) |

**Note on compAttackTau:** At 62.5 fps (16ms frame period), any attack time below ~16ms is effectively instantaneous — the smoothing filter converges in a single frame. The 1ms default means the compressor responds to level increases within one frame. Values above 16ms introduce meaningful smoothing across multiple frames.

### Category: `fire` (13 parameters) - Fire Visual Effect

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `cooling` | 80 | 0-255 | Base cooling rate |
| `sparkchance` | 0.4 | 0.0-1.0 | Probability of sparks |
| `sparkheatmin` | 160 | 0-255 | Minimum spark heat |
| `sparkheatmax` | 255 | 0-255 | Maximum spark heat |
| `audiosparkboost` | 0.3 | 0.0-1.0 | Audio influence on sparks |
| `coolingaudiobias` | 0 | -128-127 | Audio cooling bias |
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

### Category: `fireorganic` (4 parameters) - Non-Rhythmic Fire

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `organicsparkchance` | 0.15 | 0.0-0.5 | Baseline random spark rate |
| `organictransmin` | 0.3 | 0.0-1.0 | Min transient for burst |
| `organicaudiomix` | 0.6 | 0.0-1.0 | Audio influence in organic mode |
| `organicburstsuppress` | true | bool | Suppress after bursts |

**Total: ~60 tunable parameters** (56 original + 4 CBSS)

---

## Current Best Settings

### Ensemble Detector Configuration (as of February 14, 2026)

**Optimized 2-detector ensemble** — HFC + Drummer with tuned fusion parameters.

| Detector | Weight | Threshold | Enabled | Reason |
|----------|--------|-----------|---------|--------|
| **HFC**  | 0.60   | 4.0       | yes     | Primary - best rejection & overall accuracy |
| **Drummer** | 0.40 | 3.5      | yes     | Secondary - complements kicks/bass/fast-tempo |
| SpectralFlux | 0.20 | 1.4    | **no**  | Fires on pad chord changes at all thresholds |
| BassBand | 0.18   | 3.0       | **no**  | Susceptible to room rumble/HVAC |
| ComplexDomain | 0.13 | 2.0    | **no**  | Adds FPs on sparse patterns |
| Novelty  | 0.12   | 2.5       | **no**  | Net negative avg F1, hurts sparse/full-mix |

**Fusion parameters:**
```
agree_1 = 0.2           # Single-detector suppression (calibrated Feb 2026)
ensemble_cooldown = 250  # ms between detections
ensemble_minconf = 0.55  # Minimum confidence for output
```

**Detector algorithm enhancements (Feb 2026):**
- Drummer: minRiseRate=0.02 (rejects gradual swells)
- HFC: sustainRejectFrames=10 (suppresses sustained HF content)
- Disabled detectors skip `detect()` entirely (zero CPU usage)

**Spectral pipeline:**
- SharedSpectralAnalysis runs FFT-256 once per frame
- Soft-knee compressor → per-bin whitening → magnitudes (all detectors see whitened data)
- Whitened mel bands → SpectralFlux, Novelty (change-based metrics)
- Whitening: per-bin running max, decay=0.997, floor=0.001

**Re-enabling disabled detectors:** Per-bin magnitude whitening (v23+) modifies `magnitudes_[]` in-place. Detectors that rely on absolute energy levels (HFC, ComplexDomain) will need threshold retuning if re-enabled, since their thresholds were calibrated against un-whitened magnitudes.

### Comprehensive Solo Detector Testing (16 patterns each, Jan 2026)

| Detector | Avg F1 | Best Pattern | Worst Pattern | Usable? |
|----------|--------|--------------|---------------|---------|
| **HFC** | ~0.82 | pad-rejection (1.00) | bass-line (0.58) | ✅ Yes |
| **Drummer** | ~0.72 | kick-focus (0.88) | snare-focus (0.50) | ✅ Yes |
| ComplexDomain | ~0.60 | fast-tempo (0.80) | lead-melody (0.31) | ⚠️ Limited |
| SpectralFlux | ~0.42 | simultaneous (0.63) | bass-line (0.31) | ❌ No |
| BassBand | ~0.40 | fast-tempo (0.60) | pad-rejection (0.27) | ❌ No |
| Novelty* | ~0.39 | simultaneous (0.56) | pad-rejection (0.29) | ❌ No |

*Novelty replaced MelFlux in Feb 2026 (cosine distance algorithm). Solo performance similar.

### Detailed HFC Performance (Best Detector)

| Pattern | F1 | Precision | Recall | Notes |
|---------|-----|-----------|--------|-------|
| pad-rejection | 1.000 | 1.000 | 1.000 | ✅ Perfect |
| sparse | 1.000 | 1.000 | 1.000 | ✅ Perfect |
| tempo-sweep | 0.970 | 0.941 | 1.000 | ✅ Excellent |
| chord-rejection | 0.968 | 1.000 | 0.938 | ✅ Excellent |
| strong-beats | 0.918 | 0.966 | 0.875 | ✅ Good |
| hat-rejection | 0.903 | 0.933 | 0.875 | ✅ Good |
| full-mix | 0.881 | 0.963 | 0.813 | ✅ Good |
| snare-focus | 0.842 | 0.889 | 0.800 | Good |
| full-kit | 0.822 | 0.811 | 0.833 | Good |
| mixed-dynamics | 0.815 | 0.733 | 0.917 | Moderate |
| kick-focus | 0.783 | 0.783 | 0.783 | Moderate |
| lead-melody | 0.667 | 0.500 | 1.000 | ⚠️ Over-detects |
| simultaneous | 0.667 | 1.000 | 0.500 | ⚠️ Under-detects |
| synth-stabs | 0.609 | 0.636 | 0.583 | ❌ Weak |
| fast-tempo | 0.588 | 1.000 | 0.417 | ❌ Weak (cooldown) |
| bass-line | 0.583 | 0.583 | 0.583 | ❌ Weak |

### Drummer Complements HFC

| Pattern | Drummer F1 | HFC F1 | Winner |
|---------|------------|--------|--------|
| kick-focus | **0.884** | 0.783 | Drummer (+13%) |
| bass-line | **0.750** | 0.583 | Drummer (+29%) |
| fast-tempo | **0.677** | 0.588 | Drummer (+15%) |
| synth-stabs | **0.683** | 0.609 | Drummer (+12%) |

### 2-Detector Ensemble Validation

| Pattern | Ensemble F1 | HFC Solo | Drummer Solo | Notes |
|---------|-------------|----------|--------------|-------|
| strong-beats | **0.938** | 0.918 | 0.800 | ✅ Best of all |
| pad-rejection | 0.941 | 1.000 | 0.727 | Slight loss |
| full-mix | **0.918** | 0.881 | 0.780 | ✅ Best of all |
| tempo-sweep | 0.933 | 0.970 | 0.714 | Good |
| fast-tempo | **0.836** | 0.588 | 0.677 | ✅ +42% vs HFC |
| sparse | 0.889 | 1.000 | 0.778 | Slight loss |
| kick-focus | 0.783 | 0.783 | 0.884 | Equal to HFC |
| simultaneous | 0.694 | 0.667 | 0.667 | ✅ Best of all |
| bass-line | 0.627 | 0.583 | 0.750 | Between both |

### Key Findings

1. **HFC is the best standalone detector** — excellent rejection (pads, chords, sparse)
2. **Drummer complements HFC** for kicks, bass, and fast-tempo content
3. **agree_1=0.2 is the most impactful tuning parameter** — stronger single-detector suppression reduces FPs without hurting 2-detector consensus (Feb 2026)
4. **Additional detectors don't help the 2-detector config** — ComplexDomain, SpectralFlux, and Novelty all tested neutral or negative when added to HFC+Drummer (Feb 2026)
5. **SpectralFlux is fundamentally incompatible with pad rejection** — it fires on chord changes, which IS spectral flux. Whitening amplifies this by normalizing sustained content (Feb 2026)
6. **2-detector ensemble outperforms 3+ detector configs** — adding detectors increases false positive rate through agreement promotion

### Known Limitations

| Pattern | Best F1 | Issue | Potential Fix |
|---------|---------|-------|---------------|
| lead-melody | 0.29 | HFC fires on every melody note (38+ FPs) | Pitch-tracking gate or harmonic analysis |
| bass-line | 0.63 | HFC weak on low frequencies | Drummer helps partially |
| simultaneous | 0.69 | Both detectors merge overlapping hits | Cooldown limits |
| pad-rejection | 0.70-0.80 | Pad transitions still trigger HFC+Drummer | agree_1=0.2 helps; varies with environment |
| chord-rejection | 0.70 | Chord changes produce amplitude spikes | 12 FPs typical |

---

## Historical Test Results

### Test Session: 2025-12-28 (Fast Binary Search)

**Key Findings:**
1. Binary search found 43.3% better F1 than exhaustive sweep
2. Optimal values were near lower bounds (hitthresh=1.688, fluxthresh=1.4)
3. Hybrid mode weights: flux should dominate (0.7) with drummer supplement (0.3)

### Boundaries Extended

Several parameters hit their minimum bounds during tuning, prompting range extensions:

| Parameter | Old Min | New Min | Optimal | Status |
|-----------|---------|---------|---------|--------|
| attackmult | 1.1 | 1.0 | 1.1 | AT boundary |
| hitthresh | 1.5 | 1.0 | 1.688 | Near boundary |
| fluxthresh | 1.0 | 0.5 | 1.4 | Safe margin |

---

## Comprehensive Test Plan

### Overview

**Total Duration:** 2-3 hours
**Goal:** Systematically tune all audio parameters for optimal transient detection and rhythm tracking

### Prerequisites

1. Device connected and functional
2. Serial port identified (e.g., COM5)
3. Audio output to speakers/headphones near microphone
4. Quiet testing environment

### Phase 1: Baseline Measurement (20 min)

**Purpose:** Establish current performance across all patterns

```bash
cd blinky-test-player
npm run tuner -- validate --port COM5 --gain 40
```

**Expected Output:**
- F1 scores for all 18 patterns
- Per-mode performance comparison
- Identifies worst-performing patterns

**Record:** Save results as `baseline-YYYYMMDD.json`

### Phase 2: Transient Detection Optimization (45 min)

#### 2A: Core Threshold Sweep (15 min)

**Parameters:** `hitthresh`, `fluxthresh`, `attackmult`

```bash
npm run tuner -- fast --port COM5 --gain 40 \
  --params hitthresh,fluxthresh,attackmult \
  --patterns strong-beats,bass-line,synth-stabs,pad-rejection
```

**Success Criteria:**
- strong-beats F1 > 0.75
- pad-rejection precision > 0.5

#### 2B: Hybrid Mode Weights (15 min)

**Parameters:** `hyfluxwt`, `hydrumwt`, `hybothboost`

```bash
npm run tuner -- sweep --port COM5 --gain 40 \
  --params hyfluxwt,hydrumwt,hybothboost \
  --modes hybrid \
  --patterns full-mix,mixed-dynamics,hat-rejection
```

**Test Values:**
- hyfluxwt: 0.5, 0.6, 0.7, 0.8
- hydrumwt: 0.2, 0.3, 0.4, 0.5
- hybothboost: 1.0, 1.1, 1.2, 1.3

#### 2C: Cooldown Optimization (15 min)

**Parameter:** `cooldown`

```bash
npm run tuner -- sweep --port COM5 --gain 40 \
  --params cooldown \
  --patterns fast-tempo,simultaneous
```

**Test Values:** 20, 25, 30, 35, 40, 50 ms

**Success Criteria:**
- fast-tempo F1 > 0.6 (from 0.49)

### Phase 3: Rhythm Tracking Optimization (30 min)

#### 3A: Activation Threshold (10 min)

**Parameter:** `musicthresh`

**Test Procedure:**
1. Play steady 120 BPM pattern
2. Enable streaming: `stream debug`
3. Adjust `musicthresh` and observe:
   - Time to rhythm lock (rhythmStrength > 0.5)
   - Phase stability after lock

**Test Values:** 0.2, 0.3, 0.4, 0.5, 0.6

**Success Criteria:**
- Lock time < 4 seconds at 120 BPM
- No false activation on pads

#### 3B: Phase Adaptation (10 min)

**Parameter:** `phaseadapt`

**Test Procedure:**
1. Play 120 BPM, wait for lock
2. Switch to 100 BPM
3. Measure re-lock time

**Test Values:** 0.05, 0.1, 0.15, 0.2, 0.3

**Success Criteria:**
- Re-lock within 3 seconds of tempo change
- No phase hunting/oscillation

#### 3C: BPM Range (10 min)

**Parameters:** `bpmmin`, `bpmmax`

**Test Procedure:**
1. Test tempo detection at 80, 100, 120, 140, 160 BPM
2. Verify BPM estimate within 3% of actual

**Configurations:**
- Narrow range (80-150): Better for typical music
- Wide range (60-200): Better for diverse tempos

### Phase 4: Output Modulation (20 min)

#### 4A: Beat Pulse Enhancement (10 min)

**Parameters:** `pulseboost`, `pulsesuppress`, `energyboost`

**Test Procedure:**
1. Visual inspection of fire effect with beat-synced music
2. Adjust for clear visual distinction between on-beat and off-beat

**Subjective Goals:**
- On-beat sparks visibly brighter
- Off-beat transients subdued but visible
- No visual jarring/flickering

#### 4B: AGC and Dynamic Range (10 min)

**Parameters:** `hwtarget`, `fastagc`, `fastagcthresh`

**Test Procedure:**
1. Test with quiet music (ambient)
2. Test with loud music (EDM)
3. Verify smooth transitions

**Success Criteria:**
- No clipping at loud levels
- Responsive detection at quiet levels

### Phase 5: Full Validation (30 min)

**Purpose:** Verify optimized settings across all patterns

```bash
npm run tuner -- validate --port COM5 --gain 40
```

**Success Criteria:**
- No pattern F1 < 0.5 in best mode
- Average F1 > 0.70
- No regressions from baseline

### Phase 6: Save and Document (15 min)

1. **Save to device:** `save`
2. **Export settings:** `json settings > optimized-settings.json`
3. **Update documentation:** Record optimal values in this guide
4. **Commit to git:** If stable, commit updated defaults

---

## Troubleshooting

### No Audio Detection

1. Check `stream on` shows `alive: 1` (PDM working)
2. Verify hardware gain is reasonable (20-60 range)
3. Check raw level rises with sound
4. Verify detection mode is set correctly

### Too Many False Positives

1. Lower `agree_1` (try 0.15-0.20) to suppress single-detector hits
2. Raise `ensemble_minconf` (try 0.55-0.65)
3. Raise detector thresholds: `set detector_thresh hfc 5.0`
4. Increase `ensemble_cooldown` to reduce rapid triggers (try 250-350ms)
5. Disable problematic detectors: `set detector_enable <type> 0`

### Missing Transients

1. Lower detector thresholds: `set detector_thresh drummer 2.5`
2. Raise `agree_1` (try 0.25-0.30) to let single-detector hits through
3. Lower `ensemble_minconf` (try 0.4-0.5)
4. Ensure `ensemble_cooldown` isn't too long for fast patterns
5. Check AGC is tracking properly

### Rhythm Not Locking

1. Verify steady beat in audio
2. Check `musicthresh` isn't too high
3. Ensure BPM is within `bpmmin`/`bpmmax` range
4. Allow 5+ seconds for autocorrelation to accumulate

### Phase Hunting/Oscillation

1. Decrease `phaseadapt` (try 0.1)
2. Increase `musicthresh` (require stronger periodicity)
3. Check for tempo instability in source audio

### BPM Detected at Half-Time (e.g., 120 BPM → 60 BPM)

1. **FIRST:** Verify `priorenabled` is ON - this is the most common cause
2. Check `priorcenter` is set appropriately (default: 120)
3. Increase `priorwidth` if detecting extreme tempos (try 60-80)
4. For fast tempos (160+ BPM): This is a known limitation - autocorrelation harmonics are stronger at half-time

### BPM Pulled Toward Center (e.g., 60 BPM → 80 BPM)

1. This is expected behavior with tempo prior enabled
2. Increase `priorwidth` to reduce the pulling effect (try 70-80)
3. Decrease `priorstrength` for less prior influence (try 0.3-0.4)
4. Accept that extreme tempos will be biased toward the prior center

---

## Appendix: Removed Parameters

The following parameters were **removed** in AudioController v2/v3 (December 2025):

| Old Parameter | Old Component | Reason |
|---------------|---------------|--------|
| musicbeats | MusicMode PLL | Event-based activation replaced by autocorrelation strength |
| musicmissed | MusicMode PLL | No beat event counting in new architecture |
| phasesnap | MusicMode PLL | PLL replaced by autocorrelation |
| snapconf | MusicMode PLL | No longer needed |
| stablephase | MusicMode PLL | Phase derived from autocorrelation |
| confinc | MusicMode PLL | Confidence replaced by periodicityStrength |
| confdec | MusicMode PLL | Confidence replaced by periodicityStrength |
| misspenalty | MusicMode PLL | No beat event counting |
| pllkp | MusicMode PLL | No PLL in new architecture |
| pllki | MusicMode PLL | No PLL in new architecture |
| combdecay | RhythmAnalyzer (comb filter) | Merged into AudioController autocorrelation |
| combfb | RhythmAnalyzer (comb filter) | Merged into AudioController autocorrelation |
| combconf | RhythmAnalyzer (comb filter) | Merged into AudioController autocorrelation |
| histblend | RhythmAnalyzer (comb filter) | Merged into AudioController autocorrelation |
| rhythmminbpm | RhythmAnalyzer | Replaced by `bpmmin` in AudioController |
| rhythmmaxbpm | RhythmAnalyzer | Replaced by `bpmmax` in AudioController |
| rhythminterval | RhythmAnalyzer | Hardcoded to 500ms (AUTOCORR_PERIOD_MS) |
| beatthresh | RhythmAnalyzer | Replaced by `musicthresh` (activation threshold) |
| minperiodicity | RhythmAnalyzer | Merged into `musicthresh` logic |

**If you see these parameters in old documentation or param-tuner code, ignore them. They have been removed from both firmware and testing tools (January 2026).**
