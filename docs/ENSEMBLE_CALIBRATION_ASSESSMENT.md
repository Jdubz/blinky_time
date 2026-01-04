# Ensemble Detector Calibration Assessment

## Executive Summary

The new ensemble detector architecture requires updates to both the firmware serial interface and the test framework to enable full calibration sweeps. The core detection mechanism works, but the tuning infrastructure needs adaptation for the new paradigm.

**Key finding**: The calibration framework was designed around mutually-exclusive detection modes. The ensemble architecture runs ALL detectors simultaneously with weighted fusion. This is a fundamental shift that requires rethinking the calibration strategy.

---

## Current State Analysis

### Firmware (What Works)

| Feature | Status | Notes |
|---------|--------|-------|
| Ensemble detection | ✅ Working | 6 detectors run simultaneously |
| Transient output in streaming | ✅ Working | `{"a":{"t":<ensemble_strength>}}` |
| Debug streaming (agreement) | ✅ Working | `{"a":{..,"agree":<n>,"conf":<c>}}` when debug enabled |
| Legacy detectmode command | ⚠️ Partial | Stored but doesn't affect ensemble behavior |

### Firmware (What's Missing)

| Feature | Priority | Impact |
|---------|----------|--------|
| Per-detector weight commands | HIGH | Cannot tune ensemble weights via serial |
| Per-detector threshold commands | HIGH | Cannot tune individual detector sensitivity |
| Per-detector enable/disable | MEDIUM | Cannot isolate detectors for individual testing |
| Agreement boost commands | MEDIUM | Cannot tune agreement scaling |
| Per-detector streaming output | LOW | Cannot see which detectors fired |

### Test Framework (What Works)

| Feature | Status | Notes |
|---------|--------|-------|
| Pattern playback | ✅ Working | 40+ patterns with ground truth |
| Transient detection capture | ✅ Working | Captures `t` field from streaming |
| F1/precision/recall calculation | ✅ Working | ±350ms tolerance matching |
| Binary search optimization | ✅ Working | Fast-tune mode |
| Parameter sweep infrastructure | ✅ Working | Exhaustive sweeps |

### Test Framework (What's Missing)

| Feature | Priority | Impact |
|---------|----------|--------|
| Ensemble mode support | HIGH | No 'ensemble' mode defined in types.ts |
| New detector parameters | HIGH | No params for ComplexDomain, MelFlux |
| Ensemble weight parameters | HIGH | No ens_*_wt parameters |
| Agreement boost parameters | MEDIUM | No agree_* parameters |
| Updated serial commands | HIGH | runner.ts uses old command format |

---

## Detailed Gap Analysis

### 1. Firmware Serial Commands

**Current SerialConsole commands for detection:**
```
set detectmode <0-4>       # Legacy, stored but ignored
show mode                  # Shows ensemble status
stream fast                # Streams {"a":{"l":...,"t":<ensemble>}}
```

**Needed ensemble commands:**
```
# Per-detector configuration
set detector <name> weight <0.0-1.0>
set detector <name> threshold <value>
set detector <name> enable <0|1>

# Shortcuts for common operations
set ens_drummer_wt <value>
set ens_spectral_wt <value>
set ens_hfc_wt <value>
set ens_bass_wt <value>
set ens_complex_wt <value>
set ens_mel_wt <value>

# Detector thresholds (override BaseDetector defaults)
set drummer_thresh <value>
set spectral_thresh <value>
set hfc_thresh <value>
set bass_thresh <value>
set complex_thresh <value>
set mel_thresh <value>

# Agreement boost configuration
set agree_1 <0.0-1.0>      # Single detector boost
set agree_2 <0.0-1.0>      # Two detector boost
set agree_3 <0.0-1.0>      # Three detector boost
...

# Status
show detectors             # List all detector weights/thresholds
show ensemble              # Show fusion configuration
```

**Implementation location**: `SerialConsole.cpp` - add to `registerAudioSettings()` or new `registerEnsembleSettings()` function.

### 2. Streaming Output Enhancement

**Current streaming (DEBUG mode):**
```json
{"a":{"l":0.45,"t":0.85,"pk":0.32,"vl":0.04,"raw":0.12,"h":32,"alive":1,"z":0.15,"agree":3,"conf":1.0}}
```

**Additional fields for calibration (optional extended mode):**
```json
{"a":{...},
 "det":{
   "d":{"s":0.8,"c":0.9,"f":1},   // drummer: strength, confidence, fired
   "s":{"s":0.75,"c":0.85,"f":1}, // spectral
   "h":{"s":0.2,"c":0.6,"f":0},   // hfc
   "b":{"s":0.9,"c":0.95,"f":1},  // bass
   "x":{"s":0.1,"c":0.5,"f":0},   // complex
   "m":{"s":0.6,"c":0.7,"f":1}    // mel
 }}
```

This would allow the calibration framework to:
- Identify which detectors fire on which patterns
- Calculate per-detector F1 scores
- Optimize individual detector thresholds
- Analyze detector agreement patterns

### 3. Test Framework Types (types.ts)

**Current DetectionMode type:**
```typescript
export type DetectionMode = 'drummer' | 'spectral' | 'hybrid' | 'bass' | 'hfc';
```

**Updated for ensemble:**
```typescript
// Individual detector tuning (disable others)
export type DetectorType = 'drummer' | 'spectral' | 'hfc' | 'bass' | 'complex' | 'mel';

// Full ensemble mode (all detectors, tune weights)
export type EnsembleMode = 'ensemble';

// Combined for backward compatibility
export type DetectionMode = DetectorType | EnsembleMode;
```

**New parameters to add:**
```typescript
// --- Ensemble Detector Thresholds ---
drumthresh: {
  name: 'drummer_thresh',
  mode: 'ensemble',  // or 'drummer' for isolated testing
  min: 1.0, max: 10.0, default: 2.5,
  sweepValues: [1.5, 2.0, 2.5, 3.0, 4.0, 5.0],
  description: 'Drummer detector threshold',
  targetPatterns: ['strong-beats', 'attack-sharp'],
},

spectralthresh: {
  name: 'spectral_thresh',
  mode: 'ensemble',
  min: 0.5, max: 5.0, default: 1.4,
  sweepValues: [0.8, 1.0, 1.4, 2.0, 2.5, 3.0],
  description: 'Spectral flux threshold',
},

// New detectors
complexthresh: {
  name: 'complex_thresh',
  mode: 'ensemble',
  min: 1.0, max: 5.0, default: 2.0,
  sweepValues: [1.0, 1.5, 2.0, 2.5, 3.0, 4.0],
  description: 'Complex domain threshold',
  targetPatterns: ['soft-beats', 'attack-gradual'],  // Good for soft onsets
},

melthresh: {
  name: 'mel_thresh',
  mode: 'ensemble',
  min: 1.0, max: 5.0, default: 2.5,
  sweepValues: [1.5, 2.0, 2.5, 3.0, 4.0],
  description: 'Mel flux threshold',
},

// --- Ensemble Weights ---
ens_drummer_wt: {
  name: 'ens_drummer_wt',
  mode: 'ensemble',
  min: 0.0, max: 0.5, default: 0.22,
  sweepValues: [0.1, 0.15, 0.2, 0.25, 0.3, 0.35],
  description: 'Drummer detector weight in ensemble',
},
// ... similar for other 5 detectors

// --- Agreement Boost Values ---
agree_1: {
  name: 'agree_1',
  mode: 'ensemble',
  min: 0.3, max: 0.8, default: 0.6,
  sweepValues: [0.4, 0.5, 0.6, 0.7, 0.8],
  description: 'Boost when single detector fires (suppress false positives)',
},
agree_2: {
  name: 'agree_2',
  mode: 'ensemble',
  min: 0.6, max: 1.0, default: 0.85,
  sweepValues: [0.7, 0.8, 0.85, 0.9, 0.95],
  description: 'Boost when two detectors agree',
},
// ... agree_3, agree_4, agree_5, agree_6
```

### 4. Test Framework Runner (runner.ts)

**Current setMode():**
```typescript
async setMode(mode: DetectionMode): Promise<void> {
  await this.sendCommand(`set detectmode ${MODE_IDS[mode]}`);
}
```

**Updated for ensemble:**
```typescript
async setMode(mode: DetectionMode): Promise<void> {
  if (mode === 'ensemble') {
    // Enable all detectors
    await this.sendCommand('set detector drummer enable 1');
    await this.sendCommand('set detector spectral enable 1');
    await this.sendCommand('set detector hfc enable 1');
    await this.sendCommand('set detector bass enable 1');
    await this.sendCommand('set detector complex enable 1');
    await this.sendCommand('set detector mel enable 1');
  } else {
    // Solo mode: enable only one detector for isolated testing
    const detectors = ['drummer', 'spectral', 'hfc', 'bass', 'complex', 'mel'];
    for (const det of detectors) {
      await this.sendCommand(`set detector ${det} enable ${det === mode ? 1 : 0}`);
    }
  }
}
```

---

## Recommended Calibration Strategy

### Phase 1: Individual Detector Threshold Calibration

**Goal**: Find optimal threshold for each detector in isolation

**Process**:
1. For each detector (drummer, spectral, hfc, bass, complex, mel):
   a. Disable all other detectors
   b. Run representative patterns
   c. Sweep threshold values
   d. Record F1 at each threshold
   e. Find optimal threshold

**Output**: Per-detector optimal thresholds
```
drummer_thresh: 2.5
spectral_thresh: 1.4
hfc_thresh: 3.0
bass_thresh: 3.0
complex_thresh: 2.0
mel_thresh: 2.5
```

### Phase 2: Weight Optimization

**Goal**: Find optimal detector weights for ensemble fusion

**Process**:
1. Enable all detectors with Phase 1 thresholds
2. Start with equal weights (0.167 each)
3. Run full pattern suite
4. Calculate per-detector contribution to F1:
   - Patterns where detector X fired on true positives
   - Patterns where detector X caused false positives
5. Adjust weights based on contribution
6. Iterate until convergence

**Output**: Optimized detector weights
```
ens_drummer_wt: 0.22
ens_spectral_wt: 0.20
ens_hfc_wt: 0.15
ens_bass_wt: 0.18
ens_complex_wt: 0.13
ens_mel_wt: 0.12
```

### Phase 3: Agreement Boost Optimization

**Goal**: Find optimal agreement scaling values

**Process**:
1. Use Phase 1 thresholds and Phase 2 weights
2. Focus on precision/recall tradeoff:
   - Higher agree_1 = fewer false positives but may miss soft hits
   - Lower agree_1 = catches more but more false positives
3. Sweep agree_1, agree_2 (most impactful values)
4. Validate that agree_3+ values don't cause issues

**Output**: Agreement boost curve
```
agree_1: 0.6  (suppress single-detector)
agree_2: 0.85 (moderate confidence)
agree_3: 1.0  (full confidence)
agree_4: 1.1  (slight boost)
agree_5: 1.15
agree_6: 1.2  (strong consensus)
```

### Phase 4: Validation

**Goal**: Confirm no regressions vs baseline

**Process**:
1. Run ALL patterns with optimal ensemble config
2. Compare to best legacy mode (hybrid F1: 0.705)
3. Target: Ensemble F1 >= 0.75

---

## Implementation Priority

### Critical (Must Have for Calibration)

1. **Firmware**: Add per-detector threshold commands
   - `set drummer_thresh <value>`
   - `set spectral_thresh <value>`
   - ... (6 total)
   - Implementation: ~50 lines in SerialConsole.cpp

2. **Firmware**: Add per-detector enable commands
   - `set detector <name> enable <0|1>`
   - Implementation: ~30 lines in SerialConsole.cpp

3. **Test Framework**: Add new parameters to types.ts
   - 6 detector threshold params
   - 6 detector weight params
   - 6 agreement boost params
   - Implementation: ~200 lines

4. **Test Framework**: Update runner.ts for ensemble mode
   - Implementation: ~50 lines

### Important (Enables Advanced Calibration)

5. **Firmware**: Add per-detector weight commands
   - `set ens_drummer_wt <value>`
   - Implementation: ~30 lines

6. **Firmware**: Add agreement boost commands
   - `set agree_1 <value>`
   - Implementation: ~20 lines

7. **Test Framework**: Add ensemble-specific calibration suite
   - New suite config in suite.ts
   - Implementation: ~100 lines

### Nice to Have (Detailed Analysis)

8. **Firmware**: Per-detector streaming output
   - Extended JSON with per-detector results
   - Implementation: ~50 lines (may impact performance)

9. **Test Framework**: Per-detector F1 calculation
   - Analyze which detectors contribute to hits
   - Implementation: ~100 lines

---

## Estimated Effort

| Component | Effort | Lines of Code |
|-----------|--------|---------------|
| Firmware serial commands | 2-3 hours | ~150 lines |
| Test framework types | 1 hour | ~200 lines |
| Test framework runner | 1 hour | ~50 lines |
| New calibration suite | 1-2 hours | ~100 lines |
| Testing & validation | 2-3 hours | - |
| **Total** | **8-12 hours** | **~500 lines** |

---

## Quick Start: Minimal Changes for Basic Calibration

If we want to run calibration ASAP with minimal changes:

1. **Firmware**: Expose detector thresholds via existing AdaptiveMic params (hack)
   - Map hitthresh → drummer threshold
   - Map fluxthresh → spectral threshold
   - Map bassthresh → bass threshold
   - Map hfcthresh → hfc threshold
   - Add complexthresh, melthresh as new params

2. **Test Framework**: Keep existing parameter definitions
   - Existing params work for drummer/spectral/bass/hfc
   - Add only complexthresh and melthresh

3. **Calibration**: Run hybrid mode sweeps
   - The ensemble output will already reflect combined detection
   - We can tune thresholds even without individual weight control

**Limitation**: Cannot tune weights or agreement without full implementation.

---

## Appendix: File Locations

### Firmware
- `blinky-things/inputs/SerialConsole.cpp` - Serial command handling
- `blinky-things/audio/EnsembleDetector.h` - Detector access methods
- `blinky-things/audio/EnsembleFusion.h` - Weight/boost configuration
- `blinky-things/audio/IDetector.h` - Per-detector configuration

### Test Framework
- `blinky-test-player/src/param-tuner/types.ts` - Parameter definitions
- `blinky-test-player/src/param-tuner/runner.ts` - Serial communication
- `blinky-test-player/src/param-tuner/suite.ts` - Suite configurations
- `blinky-test-player/src/patterns.ts` - Test patterns
