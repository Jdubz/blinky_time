# Audio Tuning Guide

**Last Updated:** January 2026
**Architecture Version:** AudioController v3 (Multi-hypothesis tempo tracking)

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
   AdaptiveMic
   ├── Window/Range normalization (peak/valley tracking)
   ├── Transient Detection (5 algorithms)
   │   ├── Mode 0: DRUMMER (amplitude spikes)
   │   ├── Mode 1: BASS_BAND (low-pass filtered)
   │   ├── Mode 2: HFC (high frequency content)
   │   ├── Mode 3: SPECTRAL_FLUX (FFT-based)
   │   └── Mode 4: HYBRID (drummer + flux, RECOMMENDED)
   └── Spectral Flux output → AudioController
        |
   EnsembleDetector (6 detectors + fusion) → Transient Hits
        |
   AudioController
   ├── OSS Buffer (6 seconds, 360 samples @ 60Hz)
   ├── Autocorrelation (every 500ms) → Extract 4 Peaks
   ├── Multi-Hypothesis Tracker (4 concurrent tempos)
   │   ├── Primary (e.g., 120 BPM, conf=0.85)
   │   ├── Secondary (e.g., 60 BPM, conf=0.52) [half-time]
   │   ├── Tertiary (inactive)
   │   └── Candidate (inactive)
   ├── Promotion Logic (confidence-based, ≥8 beats)
   ├── Phase Tracking (from primary hypothesis)
   └── Output Synthesis
        |
   AudioControl { energy, pulse, phase, rhythmStrength }
        |
   Fire/Water/Lightning Generators (visual effects)
```

### Key Design Decisions

1. **Multi-hypothesis tempo tracking**: Maintains 4 concurrent tempo interpretations to handle tempo changes and ambiguity
2. **Pattern-based rhythm**: Uses 6-second autocorrelation buffer instead of event-based PLL
3. **Transients → pulse only**: Transient detection affects visual pulse, NOT beat tracking
4. **Phase from primary hypothesis**: Phase is derived from the highest-confidence tempo interpretation
5. **Confidence-based promotion**: Better hypotheses can replace the primary after accumulating evidence (≥8 beats)
6. **Dual decay strategy**: Beat-count decay during music (phrase-aware), time-based decay during silence
7. **Unified 4-parameter output**: Generators receive simple `AudioControl` struct

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

### Category: `detection` (11 parameters) - Algorithm Selection

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `detectmode` | 4 | 0-4 | Algorithm (0=drummer, 1=bass, 2=hfc, 3=flux, 4=hybrid) |
| `bassfreq` | 120 | 40-200 | Bass filter cutoff (Hz) [Mode 1] |
| `bassq` | 1.0 | 0.5-3.0 | Bass filter Q [Mode 1] |
| `bassthresh` | 3.0 | 1.5-10.0 | Bass detection threshold [Mode 1] |
| `hfcweight` | 1.0 | 0.5-5.0 | HFC weighting [Mode 2] |
| `hfcthresh` | 3.0 | 1.5-10.0 | HFC threshold [Mode 2] |
| `fluxthresh` | 1.4 | 0.5-10.0 | Spectral flux threshold [Mode 3,4] |
| `fluxbins` | 64 | 4-128 | FFT bins to analyze [Mode 3,4] |
| `hyfluxwt` | 0.5 | 0.1-1.0 | Hybrid: flux-only weight [Mode 4] |
| `hydrumwt` | 0.5 | 0.1-1.0 | Hybrid: drummer-only weight [Mode 4] |
| `hybothboost` | 1.2 | 1.0-2.0 | Hybrid: both-agree boost [Mode 4] |

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

### Category: `hypothesis` (14 parameters) - Multi-Hypothesis Tempo Tracking

**Peak Detection:**
| Parameter | Default | Range | Description | Access |
|-----------|---------|-------|-------------|--------|
| `minPeakStrength` | 0.3 | 0.1-0.8 | Min normalized correlation to create hypothesis | `audioCtrl.getMultiHypothesis().minPeakStrength` |
| `minRelativePeakHeight` | 0.7 | 0.5-1.0 | Peak must be >N% of max peak | `audioCtrl.getMultiHypothesis().minRelativePeakHeight` |

**Hypothesis Matching:**
| Parameter | Default | Range | Description | Access |
|-----------|---------|-------|-------------|--------|
| `bpmMatchTolerance` | 0.05 | 0.01-0.2 | ±N% BPM tolerance for matching | `audioCtrl.getMultiHypothesis().bpmMatchTolerance` |

**Promotion:**
| Parameter | Default | Range | Description | Access |
|-----------|---------|-------|-------------|--------|
| `promotionThreshold` | 0.15 | 0.05-0.5 | Confidence advantage to promote | `audioCtrl.getMultiHypothesis().promotionThreshold` |
| `minBeatsBeforePromotion` | 8 | 4-32 | Min beats before promoting | `audioCtrl.getMultiHypothesis().minBeatsBeforePromotion` |

**Decay:**
| Parameter | Default | Range | Description | Access |
|-----------|---------|-------|-------------|--------|
| `phraseHalfLifeBeats` | 32.0 | 8-64 | Half-life in beats (music) | `audioCtrl.getMultiHypothesis().phraseHalfLifeBeats` |
| `minStrengthToKeep` | 0.1 | 0.05-0.3 | Deactivate threshold | `audioCtrl.getMultiHypothesis().minStrengthToKeep` |
| `silenceGracePeriodMs` | 3000 | 1000-10000 | Grace before silence decay (ms) | `audioCtrl.getMultiHypothesis().silenceGracePeriodMs` |
| `silenceDecayHalfLifeSec` | 5.0 | 2.0-15.0 | Half-life during silence (s) | `audioCtrl.getMultiHypothesis().silenceDecayHalfLifeSec` |

**Confidence Weighting:**
| Parameter | Default | Range | Description | Access |
|-----------|---------|-------|-------------|--------|
| `strengthWeight` | 0.5 | 0.0-1.0 | Weight of autocorr strength | `audioCtrl.getMultiHypothesis().strengthWeight` |
| `consistencyWeight` | 0.3 | 0.0-1.0 | Weight of phase consistency | `audioCtrl.getMultiHypothesis().consistencyWeight` |
| `longevityWeight` | 0.2 | 0.0-1.0 | Weight of beat count | `audioCtrl.getMultiHypothesis().longevityWeight` |

**Debug:**
| Command | Default | Values | Description |
|---------|---------|--------|-------------|
| `hypodebug` | 2 (SUMMARY) | 0-3 | Debug level: OFF/EVENTS/SUMMARY/DETAILED | `set hypodebug 2` |

**Serial Commands:**
- `show hypotheses` - View all 4 hypothesis slots
- `show primary` - View primary hypothesis only
- `json hypotheses` - Get hypothesis state as JSON (for automated testing)
- `set hypodebug <0-3>` - Set debug output level
- `get hypodebug` - Get current debug level
- `set minpeakstr <0.1-0.8>` - Set minimum peak strength (and all other params above)

**Parameter Tuning:**
All multi-hypothesis parameters are now accessible via:
1. **SerialConsole**: Use `set <param> <value>` commands (e.g., `set minpeakstr 0.4`)
2. **param-tuner**: Automated binary search optimization (updated January 2026)
3. **blinky-serial-mcp**: `set_setting` and `get_hypotheses` tools for AI integration

**Testing Support:**
- MCP `get_hypotheses` tool retrieves all 4 hypothesis slots with BPM, confidence, phase, strength
- JSON output enables automated validation of tempo tracking and promotion logic

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

**Total: 70 tunable parameters** (56 original + 14 multi-hypothesis)

---

## Current Best Settings

### Transient Detection (as of 2025-12-30)

Best overall mode: **Hybrid (mode 4)** with F1 = 0.598

```
detectmode: 4
hitthresh: 2.813
attackmult: 1.1
cooldown: 80
fluxthresh: 1.4
hyfluxwt: 0.5
hydrumwt: 0.5
hybothboost: 1.2
```

**Note:** Equal hybrid weights (0.5/0.5) significantly outperformed the original 0.7/0.3 split.

### Mode Performance Comparison

| Mode | F1 Score | Precision | Recall | Best For |
|------|----------|-----------|--------|----------|
| Hybrid (4) | 0.705 | 67.3% | 72.6% | General use (RECOMMENDED) |
| Spectral (3) | 0.670 | 72.3% | 69.8% | Clean audio, fewer false positives |
| Drummer (0) | 0.664 | 90.5% | 57.0% | Precision-critical, OK missing some hits |

### Known Problem Patterns

| Pattern | Best F1 | Issue | Potential Fix |
|---------|---------|-------|---------------|
| pad-rejection | 0.44 | 20 false positives | Increase fluxthresh for sustained tones |
| full-mix | 0.65 | Low precision in complex audio | Fine-tune hybrid weights |
| simultaneous | 0.64 | Overlapping sounds = single event | Algorithmic (not parameter-tunable) |
| fast-tempo | 0.49 (drummer) | 67% missed at high speed | Reduce cooldown below 40ms |

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

1. Increase `hitthresh` (try 3.0-4.0)
2. Increase `fluxthresh` (try 2.0-3.0)
3. For pads: Switch to drummer mode (lower false positive rate)
4. Increase `cooldown` to reduce rapid triggers

### Missing Transients

1. Decrease `hitthresh` (try 1.5-2.0)
2. Decrease `attackmult` (try 1.05-1.1)
3. Ensure `cooldown` isn't too long for fast patterns
4. Check AGC is tracking properly

### Rhythm Not Locking

1. Verify steady beat in audio
2. Check `musicthresh` isn't too high
3. Ensure BPM is within `bpmmin`/`bpmmax` range
4. Allow 5+ seconds for autocorrelation to accumulate

### Phase Hunting/Oscillation

1. Decrease `phaseadapt` (try 0.1)
2. Increase `musicthresh` (require stronger periodicity)
3. Check for tempo instability in source audio

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
