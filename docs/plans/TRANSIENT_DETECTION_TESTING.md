# Transient Detection Testing Infrastructure

## Overview

This plan outlines improvements to the transient detection system and testing infrastructure to enable reliable, reproducible testing and tuning of the onset detection algorithm.

**Goal**: Build a robust transient detection system that works across varied audio sources, not one tuned to specific test sounds.

**Date**: 2024-12-27

---

## Current Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│ ARDUINO FIRMWARE (blinky-things)                                    │
│                                                                     │
│  PDM Mic → ISR (16kHz) → Biquad Filters → Energy Accumulator        │
│                             ↓                                       │
│                   detectOnsets() @ ~50Hz (main loop)                │
│                             ↓                                       │
│                   Serial JSON @ 20Hz (50ms period)                  │
│                   {"a":{"l":0.5,"lo":1,"los":0.8,...}}              │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ USB Serial
┌──────────────────────────────▼──────────────────────────────────────┐
│ MCP SERVER (blinky-serial-mcp)                                      │
│                                                                     │
│  Serial Parser → Event Emitter → Tool Handlers                      │
│       ↓                                                             │
│  run_test() spawns blinky-test-player                               │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────────────┐
│ TEST PLAYER (blinky-test-player)                                    │
│                                                                     │
│  Playwright → Chromium → Web Audio API → Speaker Output             │
│                             ↓                                       │
│                   Ground Truth JSON to stdout                       │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Issues Identified

### Arduino Sketch Issues

#### 1. Serial Streaming Too Slow (20Hz)
- **Location**: `SerialConsole.h:67`
- **Problem**: At 20Hz (50ms period), timing resolution is ±25ms. With 80ms cooldown, we may miss detections. Ground truth matching becomes unreliable.
- **Impact**: High - prevents accurate timing analysis

#### 2. No Debug Telemetry for Onset Detection
- **Location**: `SerialConsole.cpp:294-317`
- **Current Output**: Only `lo`, `hi`, `los`, `his` (boolean flags and normalized strength)
- **Missing**: Raw band energies, baseline values, rise ratios
- **Impact**: Critical - cannot diagnose why detections fail

#### 3. Hardcoded Detection Constants
- **Locations**:
  - `AdaptiveMic.cpp:36-39`: `BASELINE_TAU`, `ONSET_FLOOR`, `MAX_ONSET_RATIO`
  - `MicConstants.h:19`: `ONSET_COOLDOWN_MS`
- **Problem**: Cannot tune these values without recompiling
- **Impact**: Medium - slows iteration

#### 4. Filter Frequencies Hardcoded
- **Location**: `AdaptiveMic.cpp:327-333`
- **Values**: Low 100Hz Q=0.7, High 4kHz Q=0.5
- **Impact**: Low - current values are reasonable

#### 5. No Test Mode Support
- **Problem**: Hardware gain adapts during tests, baselines adapt to silence before tests
- **Impact**: High - tests not reproducible

### Test Pipeline Issues

#### 1. ~3 Second Timing Offset
- **Location**: `blinky-serial-mcp/src/index.ts:608`
- **Problem**: `testStartTime = Date.now()` set when process spawns, not when audio plays
- **Evidence**: First ground truth at 0ms, first detection at ~3188ms
- **Impact**: Critical - makes timing comparison meaningless

#### 2. Hardcoded Pattern List
- **Location**: `blinky-serial-mcp/src/index.ts:554-562`
- **Problem**: New patterns (realistic-track, etc.) not listed
- **Impact**: Low - easy fix

#### 3. No Metrics Calculation
- **Problem**: `run_test` returns raw data, no F1/precision/recall
- **Impact**: Medium - requires manual analysis

#### 4. Misleading Field Names
- **Location**: `blinky-serial-mcp/src/index.ts:57-63`
- **Problem**: `lowEnergy`/`highEnergy` fields actually contain strength values
- **Impact**: Low - confusing but functional

#### 5. No Parameter Sweep Support
- **Problem**: Testing different thresholds requires manual iteration
- **Impact**: Medium - slows tuning workflow

---

## Implementation Plan

### Phase 1: Visibility (Debug what's happening)
**Priority: Critical - Must complete first**

| # | Component | Change | Status |
|---|-----------|--------|--------|
| 1.1 | Arduino | Add debug stream mode with raw energies/baselines | **Done** |
| 1.2 | Arduino | Add `stream fast` command (100Hz) | **Done** |
| 1.3 | MCP | Fix timing offset using ground truth `startedAt` | **Done** |
| 1.4 | MCP | Rename fields (strength vs energy) | **Done** |

### Phase 2: Control (Reproducible tests)
**Priority: High**

| # | Component | Change | Status |
|---|-----------|--------|--------|
| 2.1 | Arduino | Add `test lock hwgain <value>` command | **Done** |
| 2.2 | Arduino | Add `test reset baselines` command | **Done** |
| 2.3 | Arduino | Make `cooldown` configurable via settings | **Done** |
| 2.4 | Arduino | Make `baselinetau` configurable via settings | **Done** |

### Phase 3: Analysis (Metrics & iteration)
**Priority: Medium**

| # | Component | Change | Status |
|---|-----------|--------|--------|
| 3.1 | MCP | Add F1/precision/recall calculation | **Done** |
| 3.2 | MCP | Update pattern list dynamically | **Done** |
| 3.3 | MCP | Add parameter sweep tool | Pending |
| 3.4 | Player | Add sync pulse option | Pending |

### Phase 4: Algorithm (Detection improvements)
**Priority: Low - requires Phase 1-3 data first**

| # | Component | Change | Status |
|---|-----------|--------|--------|
| 4.1 | Arduino | Multi-frame rise detection (compare to N-frame average) | Pending |
| 4.2 | Arduino | Consider spectral flux approach | Pending |
| 4.3 | Arduino | Configurable filter frequencies | Pending |

---

## Detailed Specifications

### 1.1 Debug Stream Mode

Add new fields to audio JSON when debug mode is enabled:

```json
{
  "a": {
    "l": 0.5,
    "lo": 1,
    "hi": 0,
    "los": 0.8,
    "his": 0.0,
    "loe": 0.045,   // NEW: raw low energy (pre-normalization)
    "hie": 0.012,   // NEW: raw high energy
    "lob": 0.018,   // NEW: low baseline
    "hib": 0.008,   // NEW: high baseline
    "lor": 2.5,     // NEW: low rise ratio (energy / prev_energy)
    "hir": 1.1      // NEW: high rise ratio
  }
}
```

**Implementation**:
- Add `streamDebug_` flag to SerialConsole
- Add `stream debug` / `stream normal` commands
- Expose internal state via new getters in AdaptiveMic

### 1.2 Fast Stream Mode

```cpp
// SerialConsole.h
static const uint16_t STREAM_PERIOD_MS = 50;       // Normal: ~20Hz
static const uint16_t STREAM_FAST_PERIOD_MS = 10;  // Fast: ~100Hz
bool streamFast_ = false;

// Commands:
// stream fast   - Enable 100Hz streaming
// stream normal - Return to 20Hz
```

### 1.3 Timing Offset Fix

```typescript
// In run_test handler, after parsing ground truth:
const groundTruth = JSON.parse(stdout);
const patternStartTime = new Date(groundTruth.startedAt).getTime();

// Adjust detection timestamps relative to pattern start
const adjustedDetections = detections.map(d => ({
  ...d,
  timestampMs: d.timestampMs - (patternStartTime - testStartTime)
}));
```

### 2.1-2.2 Test Mode Commands

```cpp
// SerialConsole.cpp - new commands

if (strcmp(cmd, "test lock hwgain") == 0 || strncmp(cmd, "test lock hwgain ", 17) == 0) {
    // Parse optional value, lock hardware gain adaptation
    testHwGainLocked_ = true;
    if (strlen(cmd) > 17) {
        int gain = atoi(cmd + 17);
        mic_->currentHardwareGain = constrain(gain, 0, 80);
        pdm_.setGain(mic_->currentHardwareGain);
    }
    return true;
}

if (strcmp(cmd, "test unlock hwgain") == 0) {
    testHwGainLocked_ = false;
    return true;
}

if (strcmp(cmd, "test reset baselines") == 0) {
    mic_->resetBaselines();  // New method to add
    return true;
}
```

### 3.1 Metrics Calculation

```typescript
interface TestMetrics {
  precision: number;      // TP / (TP + FP)
  recall: number;         // TP / (TP + FN)
  f1Score: number;        // 2 * (P * R) / (P + R)
  truePositives: number;
  falsePositives: number;
  falseNegatives: number;
  avgTimingErrorMs: number;
  maxTimingErrorMs: number;
}

function calculateMetrics(
  groundTruth: GroundTruthHit[],
  detections: TransientEvent[],
  toleranceMs: number = 50
): TestMetrics {
  // For each ground truth hit, find closest detection within tolerance
  // Mark matched pairs as TP
  // Unmatched ground truth = FN
  // Unmatched detections = FP
}
```

---

## Test Workflow (After Implementation)

```
1. Connect to device
   > mcp: connect COM5

2. Lock hardware gain for reproducibility
   > mcp: send_command "test lock hwgain 40"

3. Reset baselines
   > mcp: send_command "test reset baselines"

4. Enable debug streaming
   > mcp: send_command "stream debug"

5. Run test with metrics
   > mcp: run_test "realistic-track"

   Returns:
   {
     "metrics": { "f1Score": 0.85, "precision": 0.90, "recall": 0.80 },
     "detections": [...],
     "audioSamples": [...with debug fields...]
   }

6. Sweep parameters to find optimal values
   > mcp: sweep_parameter "onsetthresh" [2.0, 2.5, 3.0] "realistic-track"

   Returns F1 scores for each value
```

---

## Success Criteria

1. **Timing accuracy**: Detection timestamps align with ground truth within ±20ms
2. **Debug visibility**: Can see raw energies, baselines, and rise ratios
3. **Reproducibility**: Same test yields same results (±5% F1 score)
4. **Metrics**: F1 score ≥ 0.80 on realistic-track pattern
5. **Iteration speed**: Full test cycle < 30 seconds

---

## Files to Modify

### Arduino (blinky-things)
- `inputs/AdaptiveMic.h` - Add debug getters, resetBaselines()
- `inputs/AdaptiveMic.cpp` - Implement resetBaselines()
- `inputs/SerialConsole.h` - Add debug/fast stream flags, test mode state
- `inputs/SerialConsole.cpp` - Add commands and debug JSON fields

### MCP Server (blinky-serial-mcp)
- `src/index.ts` - Fix timing, add metrics, update patterns
- `src/types.ts` - Add metric types, debug sample types

### Test Player (blinky-test-player)
- No changes for Phase 1-2
