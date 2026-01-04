# Music Mode Testing Plan
## Comprehensive Testing for Transient Detection, Beat Prediction, and Music Mode

**Last Updated**: 2025-12-28
**Status**: Gap Analysis Complete - Implementation In Progress

---

## Testing Infrastructure Gap Analysis

**Reviewed**: 2025-12-28

### Current State Summary

**✅ Excellent Coverage:**
- **param-tuner**: Automated sweep for 9 transient detection parameters
- **blinky-serial-mcp**: Robust test execution with F1/precision/recall metrics
- **Test patterns**: 20 patterns with deterministic samples and ground truth
- **Fast mode**: Binary search completes in ~30 minutes

**❌ Critical Gaps:**
1. **MusicMode parameters** (10 params): Not in automated sweeps
2. **RhythmAnalyzer parameters** (5 params): Not in automated sweeps
3. **Bass/HFC mode parameters** (5 params): Not in automated sweeps
4. **RhythmAnalyzer telemetry**: Not implemented (no dedicated stream)
5. **Integration metrics**: Activation rate, lock duration, virtual beat % not auto-calculated
6. **New features untested**: BPM feedback, context-aware filtering, beat likelihood

### Parameters Requiring Automation

#### MusicMode Parameters (10 total)
```cpp
activationThreshold      // 0.0-1.0, default 0.6
minBeatsToActivate       // 2-16, default 4
maxMissedBeats          // 4-16, default 8
pllKp                   // 0.01-0.5, default 0.1
pllKi                   // 0.001-0.1, default 0.01
confidenceIncrement      // 0.05-0.2, default 0.1
confidenceDecrement      // 0.05-0.2, default 0.1
phaseErrorTolerance     // 0.1-0.5, default 0.2
missedBeatTolerance     // 1.0-3.0, default 1.5
bpmMin/bpmMax           // 40-300, defaults 60/200
```

#### RhythmAnalyzer Parameters (5 total)
```cpp
minBPM                  // 40-120, default 60
maxBPM                  // 120-300, default 200
autocorrUpdateIntervalMs // 500-2000, default 1000
beatLikelihoodThreshold  // 0.5-0.9, default 0.7
minPeriodicityStrength   // 0.3-0.8, default 0.5
```

#### Bass/HFC Mode Parameters (5 total)
```cpp
bassfreq, bassq, bassthresh      // Bass Band mode
hfcweight, hfcthresh             // HFC mode
```

### Missing Telemetry Implementation

**RhythmAnalyzer Telemetry** (proposed but not implemented):
```json
{
  "type": "RHYTHM",
  "bpm": 125.3,
  "strength": 0.82,
  "periodMs": 480.2,
  "likelihood": 0.73,
  "phase": 0.45,
  "bufferFill": 256
}
```

**Current State**: RhythmAnalyzer data embedded in music state, no dedicated stream.

### Missing Integration Metrics

Tests for PR #26 features not covered:
1. **BPM Feedback Loop**: `applyExternalBPMGuidance()` effectiveness
2. **Context-Aware Filtering**: Rhythm-based transient modulation measurement
3. **Virtual Beat Synthesis**: Real vs synthesized beat percentage
4. **Activation Reliability**: Auto-calculated success rate
5. **Lock Duration**: Auto-calculated sustained lock time

### Implementation Priorities

**Priority 1: SerialConsole Extensions** (enables all testing)
- Add RhythmAnalyzer telemetry stream
- Register all MusicMode parameters (10 params)
- Register all RhythmAnalyzer parameters (5 params)
- Register Bass/HFC mode parameters (5 params)

**Priority 2: param-tuner Extensions**
- Add 20 new parameters to sweep automation
- Implement integration metric calculation
- Add multi-component interaction tests

**Priority 3: Integration Test Patterns**
- Breakdown recovery pattern (steady → silence → steady)
- Tempo ramp pattern (100→140 BPM gradual)
- Quiet section pattern (soft transients, virtual beats needed)
- Polyrhythm pattern (multiple periodicities)

### Time Estimates

**Current Capability:**
- Transient detection only: 30 min (fast) to 6 hrs (full)

**With Full Implementation:**
- Complete musical analysis: 2-3 hrs (fast) to 12-16 hrs (full)

---

## Goal

Validate and optimize the complete music mode system including:
- **5 Transient Detection Modes** (drummer, bass, HFC, spectral flux, hybrid)
- **RhythmAnalyzer Predictive Analysis** (OSS buffering, autocorrelation, beat synthesis)
- **MusicMode Integration** (discrete + continuous hybrid tracking)
- **Real-world Music Performance** (electronic, rock, acoustic, dynamic)

---

## Table of Contents

1. [Testing Architecture](#testing-architecture)
2. [Phase 1: Telemetry & Instrumentation](#phase-1-telemetry--instrumentation)
3. [Phase 2: Transient Detection Testing](#phase-2-transient-detection-testing)
4. [Phase 3: RhythmAnalyzer Testing](#phase-3-rhythmanalyzer-testing)
5. [Phase 4: MusicMode Integration Testing](#phase-4-musicmode-integration-testing)
6. [Phase 5: Real-World Music Testing](#phase-5-real-world-music-testing)
7. [Test Automation](#test-automation)
8. [Key Metrics](#key-metrics)
9. [Implementation Order](#implementation-order)

---

## Testing Architecture

### System Under Test

```
┌─────────────────────────────────────────────────────────────┐
│                    AUDIO INPUT (Test Patterns)              │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ↓
         ┌───────────────────────────────┐
         │      AdaptiveMic              │
         │  ┌─────────────────────────┐  │
         │  │ Mode 0: DRUMMER         │  │ ← Test each mode
         │  │ Mode 1: BASS_BAND       │  │   independently
         │  │ Mode 2: HFC             │  │
         │  │ Mode 3: SPECTRAL_FLUX   │  │
         │  │ Mode 4: HYBRID          │  │
         │  └─────────────────────────┘  │
         │                               │
         │  Output: transient (0.0-1.0)  │
         └──────────┬────────────┬───────┘
                    │            │
         Discrete ──┘            └── Continuous (flux value)
         (transient)
                    │            │
      ┌─────────────┘            └──────────────┐
      ↓                                         ↓
┌──────────────────┐              ┌──────────────────────┐
│   MusicMode      │              │  RhythmAnalyzer      │
│                  │              │                      │
│ • IOI Histogram  │◄─────────────┤  • OSS Buffering     │
│ • PLL Tracking   │              │  • Autocorrelation   │
│ • Confidence     │              │  • Periodicity       │
│ • Activation     │──────────────►│  • Beat Likelihood   │
│                  │              │  • Beat Synthesis    │
└──────────────────┘              └──────────────────────┘
         │                                    │
         └────────────────┬───────────────────┘
                          ↓
                 ┌─────────────────┐
                 │  TEST METRICS   │
                 │                 │
                 │ • Transient F1  │
                 │ • BPM accuracy  │
                 │ • Lock time     │
                 │ • Beat F1       │
                 │ • Phase error   │
                 └─────────────────┘
```

### Test Data Flow

1. **Audio Pattern Generation** → Test patterns played via MCP server
2. **Device Monitoring** → Serial telemetry captured in real-time
3. **Ground Truth Comparison** → Expected vs actual detections
4. **Metrics Calculation** → Precision, recall, F1, timing accuracy
5. **Results Storage** → Compact summary + detailed logs

---

## Phase 1: Telemetry & Instrumentation

### 1.1 Arduino Telemetry Extensions

**Add to `blinky-things.ino` serial stream:**

#### Transient Detection Telemetry
```json
{
  "type": "TRANSIENT",
  "ts": 12345,
  "strength": 0.85,
  "mode": 3,
  "level": 0.42,
  "energy": 0.67
}
```

**Fields:**
- `ts`: Timestamp (millis)
- `strength`: Transient strength (0.0-1.0)
- `mode`: Detection mode (0-4)
- `level`: Current audio level
- `energy`: Raw energy (mode-dependent: bass/HFC/flux)

#### RhythmAnalyzer Telemetry
```json
{
  "type": "RHYTHM",
  "bpm": 125.3,
  "strength": 0.82,
  "periodMs": 480.2,
  "likelihood": 0.73,
  "phase": 0.45,
  "bufferFill": 256
}
```

**Fields:**
- `bpm`: Detected BPM from autocorrelation
- `strength`: Periodicity strength (0.0-1.0)
- `periodMs`: Detected period in milliseconds
- `likelihood`: Current beat likelihood (0.0-1.0)
- `phase`: Current phase within beat cycle (0.0-1.0)
- `bufferFill`: OSS buffer fill level (0-256)

#### MusicMode Telemetry
```json
{
  "type": "MUSIC",
  "active": 1,
  "bpm": 124.8,
  "phase": 0.45,
  "confidence": 0.89,
  "beatType": "quarter",
  "virtual": 0
}
```

**Fields:**
- `active`: Music mode state (0/1)
- `bpm`: Current BPM estimate
- `phase`: Beat phase (0.0-1.0)
- `confidence`: Confidence level (0.0-1.0)
- `beatType`: Beat type on detection (quarter/half/whole)
- `virtual`: Virtual beat flag (0=real transient, 1=synthesized)

#### Periodic Status Update (Every 1 second)
```json
{
  "type": "STATUS",
  "ts": 12345,
  "mode": 3,
  "hwGain": 40,
  "level": 0.35,
  "avgLevel": 0.28,
  "peakLevel": 0.72
}
```

### 1.2 Implementation Files

**Files to modify:**
- `blinky-things/blinky-things.ino` - Add telemetry output in main loop
- `blinky-things/music/MusicMode.cpp` - Emit beat events
- `blinky-things/music/RhythmAnalyzer.cpp` - Optional debug output

**Integration points:**
```cpp
// In main loop after mic->update():
if (mic->getTransient() > 0.0f) {
    Serial.print(F("{\"type\":\"TRANSIENT\",\"ts\":"));
    Serial.print(millis());
    Serial.print(F(",\"strength\":"));
    Serial.print(mic->getTransient(), 2);
    Serial.print(F(",\"mode\":"));
    Serial.print(mic->getDetectionMode());
    Serial.println(F("}"));
}

// After rhythm->update():
if (rhythm && millis() - lastRhythmTelemetry > 1000) {
    lastRhythmTelemetry = millis();
    if (rhythm->detectedPeriodMs > 0.0f) {
        Serial.print(F("{\"type\":\"RHYTHM\",\"bpm\":"));
        Serial.print(rhythm->getDetectedBPM(), 1);
        // ... rest of fields
        Serial.println(F("}"));
    }
}
```

---

## Phase 2: Transient Detection Testing

### 2.1 Test Each Detection Mode Independently

**Goal**: Characterize performance of each transient detection algorithm.

#### Mode 0: DRUMMER (Amplitude-Based)
**Strengths**: Simple, low CPU, works on any audio
**Weaknesses**: Sensitive to noise, misses quiet transients

**Test Patterns:**
- `simple-beat`: Clean kick drum on quarter notes (120 BPM)
- `varying-velocity`: Kicks with velocity 0.3-1.0
- `noise-test`: Kicks + white noise background
- `decay-test`: Long decay samples (bass hits)

**Expected Performance:**
- F1 > 0.85 on clean beats
- Degradation with noise (target: F1 > 0.70 @ SNR 10dB)
- Miss rate increases with quiet transients

#### Mode 1: BASS_BAND (Filtered Bass Energy)
**Strengths**: Immune to hi-hat false positives
**Weaknesses**: Misses mid/high transients (snares, claps)

**Test Patterns:**
- `bass-only`: Kick drums (60-100 Hz)
- `snare-only`: Snare hits (200-400 Hz) - should NOT detect
- `kick-plus-hats`: Kick + constant hi-hats - should detect kicks only
- `bass-drop`: EDM-style bass drops

**Expected Performance:**
- F1 > 0.90 on bass transients
- False positive rate < 0.05 on hi-hats
- Precision > 0.95 (few false positives)

#### Mode 2: HFC (High Frequency Content)
**Strengths**: Detects sharp attacks, percussive hits
**Weaknesses**: May miss low-frequency kicks

**Test Patterns:**
- `snare-beats`: Snare on 2&4 (classic rock pattern)
- `hats-only`: Hi-hat 16th notes - should detect or reject based on threshold
- `claps`: Hand claps, finger snaps
- `mixed-percussion`: Kicks + snares + hats

**Expected Performance:**
- F1 > 0.85 on sharp attacks
- May have lower recall on kicks (acceptable trade-off)

#### Mode 3: SPECTRAL_FLUX (FFT-Based)
**Strengths**: Industry standard, balanced detection
**Weaknesses**: Higher CPU cost, FFT latency

**Test Patterns:**
- `full-kit`: Kick + snare + hats (house beat)
- `complex-timbre`: Synthesized sounds, bass wobbles
- `tempo-changes`: Gradual tempo ramp (100-140 BPM)
- `simultaneous`: Multiple instruments hitting together

**Expected Performance:**
- F1 > 0.88 across diverse patterns
- Best all-around performance
- Consistent across timbres

#### Mode 4: HYBRID (Drummer + Spectral Flux)
**Strengths**: Best of both worlds, tuned weights
**Weaknesses**: Complex, harder to debug

**Test Patterns:**
- All patterns from modes 0 and 3
- Focus on edge cases where one method fails but other succeeds

**Expected Performance:**
- F1 > 0.90 (target: best overall)
- Should outperform individual modes on challenging patterns

### 2.2 Transient Detection Test Metrics

**Per-mode metrics:**
```json
{
  "mode": 3,
  "pattern": "house-4x4",
  "metrics": {
    "precision": 0.91,
    "recall": 0.89,
    "f1": 0.90,
    "falsePositiveRate": 0.09,
    "avgTimingError": 8.3,
    "maxTimingError": 23.1
  },
  "parameters": {
    "fluxThresh": 2.8,
    "fluxBins": 64
  }
}
```

**Timing accuracy:**
- `avgTimingError`: Mean absolute error in milliseconds
- `maxTimingError`: Worst-case timing error
- Target: < 10ms average, < 30ms max

### 2.3 Mode Comparison Matrix

**Run same pattern across all modes, compare:**

| Pattern | Mode 0 F1 | Mode 1 F1 | Mode 2 F1 | Mode 3 F1 | Mode 4 F1 | Winner |
|---------|-----------|-----------|-----------|-----------|-----------|--------|
| house-4x4 | 0.85 | 0.91 | 0.83 | 0.92 | **0.94** | Hybrid |
| dnb-break | 0.78 | 0.82 | 0.88 | 0.89 | **0.91** | Hybrid |
| quiet-kicks | 0.72 | **0.85** | 0.68 | 0.80 | 0.83 | Bass |
| snare-only | 0.81 | 0.42 | **0.90** | 0.87 | 0.89 | HFC |

**Goal**: Identify best mode for each musical context, validate hybrid superiority.

---

## Phase 3: RhythmAnalyzer Testing

### 3.1 Autocorrelation Accuracy

**Test Goal**: Verify RhythmAnalyzer correctly detects tempo via OSS autocorrelation.

**Test Procedure:**
1. Play steady beat pattern at known BPM (60, 80, 100, 120, 140, 174, 200)
2. Wait for buffer to fill (256 frames @ 60 Hz = ~4.3 seconds)
3. Capture detected BPM and periodicity strength
4. Compare to ground truth

**Test Patterns:**
- `metronome-60`: Perfect metronome at 60 BPM
- `metronome-120`: 120 BPM (most common)
- `metronome-174`: 174 BPM (drum & bass)
- `metronome-200`: 200 BPM (speed metal)

**Expected Results:**
```json
{
  "pattern": "metronome-120",
  "groundTruthBPM": 120.0,
  "detectedBPM": 119.8,
  "error": 0.2,
  "errorPercent": 0.17,
  "periodicityStrength": 0.94,
  "convergenceTime": 4.1
}
```

**Success Criteria:**
- BPM error < 2% (±2.4 BPM @ 120 BPM)
- Periodicity strength > 0.8 for steady beats
- Convergence time < 5 seconds

### 3.2 Beat Likelihood Prediction

**Test Goal**: Verify `getBeatLikelihood()` accurately predicts beat occurrence.

**Test Procedure:**
1. Play pattern with known beat positions
2. Sample beat likelihood at 60 Hz
3. Check if likelihood peaks align with actual beats
4. Measure phase accuracy

**Metrics:**
- **Peak alignment**: Do likelihood peaks occur within ±50ms of beats?
- **False peaks**: How many non-beat peaks exceed threshold?
- **Phase error**: RMS error between predicted and actual beat phase

**Test Pattern**: `house-4x4` with clear kick on every quarter note

**Expected Results:**
```json
{
  "pattern": "house-4x4",
  "beatsInPattern": 16,
  "peaksDetected": 16,
  "peaksAligned": 15,
  "alignmentRate": 0.94,
  "avgPhaseError": 0.08,
  "rmsPhaseError": 0.12
}
```

**Success Criteria:**
- Alignment rate > 0.90
- RMS phase error < 0.15 (15% of beat period)

### 3.3 Missed Beat Handling

**Test Goal**: Verify RhythmAnalyzer maintains tempo estimate when transients missed.

**Test Procedure:**
1. Play steady beat for 10 seconds (music mode locks)
2. Artificially suppress transient detection (lower mic gain or threshold)
3. Monitor if RhythmAnalyzer BPM stays stable
4. Check if beat likelihood continues to predict beats

**Test Pattern**: `house-4x4` with gradually decreasing transient detection

**Expected Results:**
```json
{
  "pattern": "missed-beats-test",
  "transientDetectionRate": 0.35,
  "rhythmBPMStability": 0.92,
  "beatLikelihoodPeaks": 14,
  "expectedBeats": 16,
  "synthesizedBeats": 11
}
```

**Success Criteria:**
- BPM stays within ±5% even with 50% missed transients
- Beat likelihood continues to peak at beat positions
- Enables virtual beat synthesis

### 3.4 Tempo Change Tracking

**Test Goal**: Verify RhythmAnalyzer adapts to tempo changes without losing lock.

**Test Procedure:**
1. Play pattern at 120 BPM for 10 seconds
2. Gradually ramp to 140 BPM over 5 seconds
3. Hold at 140 BPM for 10 seconds
4. Monitor BPM tracking and phase continuity

**Test Pattern**: `tempo-ramp-120-140`

**Expected Results:**
```json
{
  "pattern": "tempo-ramp",
  "startBPM": 120,
  "endBPM": 140,
  "trackingDelay": 2.3,
  "maxBPMError": 4.2,
  "phaseContinuity": 0.87
}
```

**Success Criteria:**
- Tempo tracking delay < 3 seconds
- Max BPM error < 10% during transition
- Phase continuity > 0.80 (minimal disruption)

---

## Phase 4: MusicMode Integration Testing

### 4.1 Hybrid Tracking (Discrete + Continuous)

**Test Goal**: Verify MusicMode correctly blends IOI-based and autocorrelation-based BPM.

**Test Procedure:**
1. Play pattern with known BPM
2. Capture both MusicMode BPM (IOI) and RhythmAnalyzer BPM (autocorr)
3. Monitor blended BPM in MusicMode
4. Verify blend weight adapts based on transient count

**Expected Behavior:**
- With many transients (>8): Favor IOI (blend weight 0.3)
- With few transients (<4): Favor autocorr (blend weight 0.7)
- When both agree (within 10%): Confidence boost

**Test Pattern**: `house-4x4` with varying transient detection rate

**Metrics:**
```json
{
  "intervalCount": 12,
  "ioiBPM": 124.2,
  "rhythmBPM": 123.8,
  "blendedBPM": 124.0,
  "blendWeight": 0.3,
  "confidenceBoost": 0.12
}
```

### 4.2 Virtual Beat Synthesis

**Test Goal**: Verify MusicMode synthesizes virtual beats when pattern detected but transient missed.

**Test Procedure:**
1. Lock MusicMode to steady pattern (>4 beats)
2. Suppress transient detection (lower threshold)
3. Monitor for virtual beat events (`virtual: 1` in telemetry)
4. Verify virtual beats occur at correct phase

**Test Pattern**: `house-4x4` with artificially suppressed transients

**Expected Results:**
```json
{
  "totalBeats": 32,
  "realTransients": 18,
  "virtualBeats": 14,
  "virtualBeatRate": 0.44,
  "avgVirtualPhaseError": 0.09
}
```

**Success Criteria:**
- Virtual beats triggered when likelihood > threshold (default 0.7)
- Virtual beats occur near expected phase (within 20% of period)
- Music mode stays active despite missed transients

### 4.3 Activation Reliability

**Test Goal**: Compare activation time/success rate with and without RhythmAnalyzer.

**Test Procedure:**
1. **Baseline**: Disable RhythmAnalyzer, run pattern 10 times
2. **Enhanced**: Enable RhythmAnalyzer, run same pattern 10 times
3. Measure: time to activation, success rate

**Test Patterns:**
- `easy-activation`: Clean steady beat (should work both ways)
- `challenging-activation`: Quiet kicks, constant hi-hats (tests improvement)
- `failure-case`: Pattern that fails without RhythmAnalyzer

**Expected Improvement:**
```json
{
  "pattern": "challenging-activation",
  "baseline": {
    "successRate": 0.60,
    "avgActivationTime": 6.2
  },
  "enhanced": {
    "successRate": 0.90,
    "avgActivationTime": 3.1
  },
  "improvement": {
    "successRateDelta": 0.30,
    "activationTimeDelta": -3.1
  }
}
```

**Success Criteria:**
- Activation success rate improvement > 20%
- Activation time reduction > 30%
- No degradation on easy patterns

### 4.4 Sustained Lock Duration

**Test Goal**: Measure how long MusicMode stays locked during challenging sections.

**Test Procedure:**
1. Lock to steady beat for 5 seconds
2. Play pattern with breakdown/bridge (no kicks for 4-8 beats)
3. Resume steady beat
4. Measure if lock maintained through breakdown

**Test Pattern**: `breakdown-test` (steady → silence → steady)

**Expected Results:**
```json
{
  "pattern": "breakdown-test",
  "baseline": {
    "lockDuration": 7.2,
    "lostLockDuring": "breakdown"
  },
  "enhanced": {
    "lockDuration": 22.5,
    "maintainedThroughBreakdown": true
  }
}
```

**Success Criteria:**
- Lock duration > 20 seconds with RhythmAnalyzer
- Maintains lock through 8+ missed beats

---

## Phase 5: Real-World Music Testing

### 5.1 Genre-Specific Test Patterns

#### Electronic Music

| Pattern | BPM | Description | Challenge |
|---------|-----|-------------|-----------|
| `house-4x4` | 124 | Kick on every beat, offbeat hats | Baseline (should be easy) |
| `dnb-break` | 174 | Fast breakbeats, snare on 2&4 | High BPM, complex rhythm |
| `dubstep-drop` | 140 | Half-time feel, bass wobbles | Slow perceived tempo |
| `techno-minimal` | 132 | Minimal kicks, hi-hat patterns | Sparse transients |

#### Rock/Acoustic

| Pattern | BPM | Description | Challenge |
|---------|-----|-------------|-----------|
| `rock-basic` | 120 | Standard rock beat (kick, snare, hats) | Live drums (timing variance) |
| `acoustic-fingerpick` | 90 | Acoustic guitar, no drums | Non-percussive, soft attacks |
| `ballad-sparse` | 72 | Slow tempo, lots of space | Slow BPM, long gaps |

#### Hip-Hop/R&B

| Pattern | BPM | Description | Challenge |
|---------|-----|-------------|-----------|
| `hiphop-lazy` | 90 | Swing timing, ghost notes | Non-quantized, loose feel |
| `trap-rolls` | 140 | Rapid hi-hat rolls | Dense hi-hats, false positives |
| `boom-bap` | 95 | Classic hip-hop, heavy kick/snare | Strong attacks, good baseline |

#### Challenging Edge Cases

| Pattern | BPM | Description | Challenge |
|---------|-----|-------------|-----------|
| `tempo-ramp` | 100→140 | Gradual tempo increase | Tempo tracking |
| `polyrhythm` | 120 | 3-over-4 polyrhythm | Multiple periodicities |
| `rubato` | ~80 | Human-performed, tempo drift | Non-metronomic timing |
| `silence-test` | 120 | Beat → 4 bar silence → beat | Lock retention |

### 5.2 Real-World Success Criteria

**Per-genre targets:**

| Genre | Transient F1 | BPM Error | Activation Rate | Lock Duration |
|-------|--------------|-----------|-----------------|---------------|
| Electronic | >0.90 | <2% | >95% | >30s |
| Rock | >0.85 | <3% | >85% | >20s |
| Hip-Hop | >0.80 | <5% | >80% | >15s |
| Acoustic | >0.75 | <5% | >70% | >10s |

**Overall system target:**
- **Average F1 across all genres**: >0.85
- **BPM accuracy**: <3% error average
- **Activation reliability**: >85% success rate
- **Lock duration**: >20 seconds average

---

## Test Automation

### 6.1 Compact Test Results

**Problem**: Verbose test output wastes tokens and is hard to parse.

**Solution**: Return compact summary, write full details to file.

**MCP `run_test` returns:**
```json
{
  "pattern": "house-4x4",
  "mode": 3,
  "transient": {
    "f1": 0.91,
    "precision": 0.88,
    "recall": 0.94,
    "avgTimingError": 7.2
  },
  "rhythm": {
    "bpmError": 1.2,
    "periodicityStrength": 0.89,
    "convergenceTime": 4.1
  },
  "music": {
    "activationTime": 2.3,
    "beatF1": 0.89,
    "lockDuration": 28.5,
    "virtualBeats": 3
  },
  "detailsFile": "test-results/house-4x4-1735334400.json"
}
```

**Full details file contains:**
- Raw detection events (all transients, rhythm updates, music beats)
- Per-beat timing errors
- Debug diagnostics
- Test parameters
- Audio samples (optional)

### 6.2 Test Suite Runner

**Tool**: `run_suite` MCP function

**Usage:**
```bash
run_suite --patterns house-4x4,dnb-break,hiphop-lazy --modes 3,4 --iterations 2
```

**Returns summary:**
```json
{
  "summary": {
    "totalTests": 12,
    "avgTransientF1": 0.88,
    "avgBpmError": 1.7,
    "avgActivationTime": 2.8,
    "weakestPattern": "hiphop-lazy",
    "bestMode": 4
  },
  "detailsDir": "test-results/suite-1735334400/",
  "modeComparison": {
    "mode3": { "avgF1": 0.86 },
    "mode4": { "avgF1": 0.91 }
  }
}
```

**Suite runner features:**
- Run multiple patterns sequentially
- Test multiple detection modes
- Multiple iterations for statistical significance
- Aggregate results across tests
- Identify best/worst performers

### 6.3 Regression Testing

**Goal**: Ensure changes don't degrade performance.

**Workflow:**
1. **Establish baseline**: Run full suite, save results to `baseline.json`
2. **Make code changes**: Modify detection algorithms, tune parameters
3. **Run regression**: Compare new results to baseline
4. **Flag degradations**: Alert if any metric drops >5%

**Regression report:**
```json
{
  "baseline": "baseline-1735334400.json",
  "current": "test-1735420800.json",
  "regressions": [
    {
      "pattern": "dnb-break",
      "metric": "transient.f1",
      "baseline": 0.89,
      "current": 0.82,
      "delta": -0.07,
      "severity": "HIGH"
    }
  ],
  "improvements": [
    {
      "pattern": "house-4x4",
      "metric": "music.activationTime",
      "baseline": 3.2,
      "current": 2.1,
      "delta": -1.1
    }
  ]
}
```

---

## Key Metrics

### Transient Detection Metrics

| Metric | Formula | Target |
|--------|---------|--------|
| **Precision** | TP / (TP + FP) | >0.85 |
| **Recall** | TP / (TP + FN) | >0.90 |
| **F1 Score** | 2 × (P × R) / (P + R) | >0.88 |
| **Timing Error** | \|detected - expected\| | <10ms avg |

Where:
- TP = True Positives (correct detections)
- FP = False Positives (spurious detections)
- FN = False Negatives (missed beats)

### RhythmAnalyzer Metrics

| Metric | Description | Target |
|--------|-------------|--------|
| **BPM Error** | \|detected - ground truth\| / ground truth | <2% |
| **Periodicity Strength** | Autocorrelation confidence | >0.80 |
| **Convergence Time** | Time to first valid BPM | <5s |
| **Phase Error (RMS)** | RMS error in beat phase | <0.15 |

### MusicMode Metrics

| Metric | Description | Target |
|--------|-------------|--------|
| **Activation Success Rate** | % of tests that activate | >85% |
| **Activation Time** | Time from start to activation | <3s |
| **Beat F1** | F1 score for beat events | >0.85 |
| **Lock Duration** | Time music mode stays active | >20s |
| **Virtual Beat Rate** | % of beats synthesized | 10-30% |

### System-Wide Metrics

| Metric | Description | Target |
|--------|-------------|--------|
| **CPU Usage** | Total music mode CPU % | <5% |
| **RAM Usage** | Total music mode RAM | <10KB |
| **Latency** | Detection to LED response | <50ms |

---

## Implementation Order

### Phase 1: Telemetry (Priority: HIGH)
**Effort**: 2-3 hours
**Deliverables:**
1. Add transient detection telemetry to main loop
2. Add RhythmAnalyzer telemetry output
3. Add MusicMode beat events
4. Verify telemetry via Serial Monitor

**Success Criteria:**
- Clean JSON output on serial (no corruption)
- All required fields present
- 60 Hz output rate sustainable

---

### Phase 2: Test Infrastructure (Priority: HIGH)
**Effort**: 3-4 hours
**Deliverables:**
1. Implement compact test result format
2. Create result file writer (JSON)
3. Update MCP `run_test` to use new format
4. Implement metric calculation (F1, BPM error, etc.)

**Success Criteria:**
- Token usage reduced >80%
- All metrics calculated correctly
- Results reproducible

---

### Phase 3: Mode Testing (Priority: MEDIUM)
**Effort**: 2-3 hours
**Deliverables:**
1. Create test patterns for each mode (0-4)
2. Run baseline tests on all modes
3. Generate mode comparison matrix
4. Identify best mode per pattern type

**Success Criteria:**
- All 5 modes tested
- Clear winner identified for each pattern type
- Hybrid mode validates as best overall (target F1 >0.90)

---

### Phase 4: RhythmAnalyzer Testing (Priority: HIGH)
**Effort**: 3-4 hours
**Deliverables:**
1. BPM accuracy tests (60-200 BPM range)
2. Beat likelihood validation
3. Missed beat handling test
4. Tempo change tracking test

**Success Criteria:**
- BPM error <2% across range
- Beat likelihood aligns with actual beats
- Maintains tempo with 50% missed transients
- Tracks tempo changes within 3 seconds

---

### Phase 5: Integration Testing (Priority: HIGH)
**Effort**: 4-5 hours
**Deliverables:**
1. Hybrid tracking validation
2. Virtual beat synthesis test
3. Activation reliability comparison (baseline vs enhanced)
4. Lock duration stress test

**Success Criteria:**
- Activation time reduced >30%
- Lock duration increased >2x
- Virtual beats <30% of total
- No degradation on easy patterns

---

### Phase 6: Real-World Testing (Priority: MEDIUM)
**Effort**: 5-6 hours
**Deliverables:**
1. Import 50+ drum samples from Ableton library
2. Create 15+ genre-specific test patterns
3. Run full suite across all patterns
4. Generate comprehensive performance report

**Success Criteria:**
- Average F1 >0.85 across all genres
- Activation success >85%
- Lock duration >20s average

---

### Phase 7: Automation & Regression (Priority: LOW)
**Effort**: 2-3 hours
**Deliverables:**
1. Implement `run_suite` batch runner
2. Create baseline results snapshot
3. Implement regression detection
4. Set up CI/CD integration (optional)

**Success Criteria:**
- Full suite runs unattended
- Regression detection <5% delta
- Results archived with timestamps

---

## File Structure

```
test-results/
├── baseline.json                    # Baseline results for regression
├── latest.json                      # Most recent test summary
├── mode-comparison.json             # Performance by detection mode
│
├── transient/                       # Transient detection tests
│   ├── drummer-{pattern}-{ts}.json
│   ├── bass-{pattern}-{ts}.json
│   ├── hfc-{pattern}-{ts}.json
│   ├── flux-{pattern}-{ts}.json
│   └── hybrid-{pattern}-{ts}.json
│
├── rhythm/                          # RhythmAnalyzer tests
│   ├── bpm-accuracy-{ts}.json
│   ├── beat-likelihood-{ts}.json
│   ├── missed-beats-{ts}.json
│   └── tempo-change-{ts}.json
│
├── integration/                     # Integration tests
│   ├── hybrid-tracking-{ts}.json
│   ├── virtual-beats-{ts}.json
│   ├── activation-{ts}.json
│   └── lock-duration-{ts}.json
│
├── realworld/                       # Real-world music tests
│   ├── electronic/
│   ├── rock/
│   ├── hiphop/
│   └── acoustic/
│
└── suites/                          # Batch test results
    ├── suite-{timestamp}/
    │   ├── summary.json
    │   ├── {pattern}-mode{n}-iter{i}.json
    │   └── ...
    └── regression-{timestamp}/
        ├── report.json
        ├── regressions.json
        └── improvements.json
```

---

## Next Steps

1. **Immediate**: Implement Phase 1 telemetry (enables all subsequent testing)
2. **Week 1**: Complete test infrastructure (Phases 2-3)
3. **Week 2**: RhythmAnalyzer validation (Phase 4)
4. **Week 3**: Integration testing (Phase 5)
5. **Week 4**: Real-world testing (Phase 6)
6. **Ongoing**: Regression testing (Phase 7)

---

**End of Testing Plan**
