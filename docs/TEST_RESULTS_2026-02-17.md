# Audio System Test Results - February 17, 2026

## Executive Summary

Comprehensive testing of the transient detection, rhythm analysis, and phase tracking systems reveals **strong performance on clean drum patterns** but **significant issues with realistic multi-instrument music**.

### Key Findings

| Area | Status | Details |
|------|--------|---------|
| Clean drum detection | ✅ Excellent | F1=1.0 on strong-beats (100% consistency) |
| Hi-hat rejection | ✅ Excellent | F1=0.99 avg, no false positives from hats |
| BPM tracking (clean) | ✅ Excellent | <2% error on steady patterns |
| Full-mix detection | ⚠️ Acceptable | F1=0.87, consistent but 7-8 FPs |
| BPM tracking (full-mix) | ❌ Poor | 14-31% error, highly erratic |
| Bass-line detection | ❌ Poor | F1=0.73, missing 25-35% of bass hits |
| Synth-stabs | ❌ Unreliable | F1 varies 0.50-0.92 (extreme variance) |
| Fast tempo (150 BPM) | ⚠️ Limited | F1=0.76, cooldown blocking ~36% of hits |

---

## Test Results by Suite

### Suite 1: Core Drums (3 runs each)

| Pattern | Run 1 | Run 2 | Run 3 | Mean F1 | Variance | BPM Err |
|---------|-------|-------|-------|---------|----------|---------|
| strong-beats | 1.000 | 1.000 | 1.000 | **1.000** | 0.000 | 1.3% |
| basic-drums | 0.818 | 0.925 | 0.909 | 0.884 | **0.107** | 1.2% |
| full-kit | 0.789 | 0.806 | 0.750 | 0.782 | 0.056 | 10.0% |
| mixed-dynamics | 0.836 | 0.857 | 0.857 | 0.850 | 0.021 | 1.1% |

**Issues identified**:
- basic-drums has high variance (0.107 F1 spread)
- full-kit BPM tracking inconsistent (8-20% error)

### Suite 5.1: Consistency (strong-beats × 10)

| Metric | Value |
|--------|-------|
| F1 scores | 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.984 |
| Mean | 0.998 |
| Std Dev | 0.005 |
| Total hits | 320 expected, 319 detected |
| Miss rate | **0.3%** |

✅ **Excellent consistency** on calibrated pattern.

### Suite 2: Full Mix

| Pattern | Runs | F1 Range | Mean | Key Issue |
|---------|------|----------|------|-----------|
| full-mix | 5 | 0.873-0.886 | 0.878 | 7-8 FPs per run |
| bass-line | 3 | 0.703-0.789 | 0.732 | **14 FNs** - missing bass |
| synth-stabs | 3 | 0.500-0.919 | 0.655 | **Extreme variance** |

**Critical issues**:
- bass-line: Only 65-75% recall - bass notes not triggering detection
- synth-stabs: Highly inconsistent - environmental factors?

### Suite 5.2: Full-mix Consistency (× 10)

| Metric | Value |
|--------|-------|
| F1 scores | 0.873 (×9), 0.886 (×1) |
| Mean | 0.874 |
| Std Dev | **0.004** (excellent!) |
| BPM range | 83.3 - 115.5 (expected: 120) |
| BPM Std Dev | **10.7 BPM** (poor) |

**Paradox**: F1 detection is very consistent, but BPM tracking is wildly inconsistent.

### Suite 3: BPM Range

| Tempo | F1 Mean | BPM Error | Detection | Tracking |
|-------|---------|-----------|-----------|----------|
| 90 BPM | 1.000 | 0.4% | ✅ Perfect | ✅ Excellent |
| 120 BPM | 0.997 | 1.0% | ✅ Perfect | ✅ Excellent |
| 150 BPM | 0.762 | 2.0% | ⚠️ 36% missed | ✅ Good |

**Issue at 150 BPM**: At 400ms between beats, cooldown (250ms) may be blocking consecutive hits on patterns with 16th-note elements.

### Suite 4: Phase Synchronization

Manual analysis of test data shows:
- Phase cycles correctly (0→1 over beat period)
- Phase wraparound occurs ~200ms after expected beat time
- This matches the measured audio latency (~194-217ms)

✅ **Phase tracking is working correctly** with expected latency compensation.

### Suite 6: Medium Difficulty

| Pattern | F1 Mean | Notes |
|---------|---------|-------|
| medium-beats | N/A | Pattern definition bug (expected=0) |
| soft-beats | N/A | Pattern definition bug (expected=0) |
| hat-rejection | 0.990 | Excellent hi-hat rejection |

**Test infrastructure issue**: medium-beats and soft-beats patterns have `expectTrigger: false` for all hits, making them untestable.

---

## System Shortcomings

### 1. Bass Detection Gap (CRITICAL)

**Symptom**: bass-line pattern achieves only 65-75% recall
**Cause**: Bass notes (low frequency, slower attack) may not trigger HFC or Drummer detectors
**Impact**: Real music with prominent bass lines will have missed beats
**Severity**: High - affects common music scenarios

### 2. BPM Tracking Instability on Complex Audio (HIGH)

**Symptom**: Same 120 BPM pattern produces BPM readings from 83-118 across runs
**Cause**: Multi-instrument content creates noisy OSS signal; autocorrelation finds different peaks
**Impact**: Phase-locked visual effects will be unreliable on real music
**Severity**: High - core feature unreliable

### 3. Synth Transient Variance (MEDIUM)

**Symptom**: synth-stabs F1 varies from 0.50 to 0.92 across runs
**Cause**: Possibly AGC instability or environmental noise sensitivity
**Impact**: Unpredictable detection of synth-heavy music
**Severity**: Medium - affects electronic music scenarios

### 4. Fast Tempo Cooldown Blocking (MEDIUM)

**Symptom**: At 150 BPM, 36% of expected hits are missed
**Cause**: 250ms cooldown vs 400ms beat interval leaves little headroom for timing variance
**Impact**: Fast electronic music will have detection gaps
**Severity**: Medium - affects specific tempo range

### 5. Test Pattern Definition Bugs (LOW)

**Symptom**: medium-beats and soft-beats show expected=0
**Cause**: Pattern definitions likely have `expectTrigger: false` incorrectly set
**Impact**: Cannot test loudness sensitivity
**Severity**: Low - infrastructure issue, not algorithm issue

---

## Recommendations

### Immediate Actions

1. **Fix pattern definitions** for medium-beats and soft-beats
2. **Investigate bass detection** - may need to adjust detector weights or add bass-focused detector
3. **Add OSS signal smoothing** before autocorrelation to reduce BPM jitter

### Algorithm Improvements

1. **Bass band emphasis** in ensemble detector weights
2. **BPM tracking hysteresis** - resist changing BPM unless strong evidence
3. **Cooldown adaptation** - shorter cooldown at higher detected tempos

### Testing Infrastructure

1. **Add multi-run averaging** to test harness for variance measurement
2. **Add AGC stability monitoring** during tests
3. **Create bass-focused test patterns** with isolated bass content

---

## Raw Data Summary

### Transient Detection Performance

| Category | Mean F1 | Variance | Status |
|----------|---------|----------|--------|
| Clean drums (strong-beats) | 0.998 | 0.005 | ✅ |
| Basic drums (basic-drums) | 0.884 | 0.107 | ⚠️ |
| Full kit (full-kit) | 0.782 | 0.056 | ⚠️ |
| Mixed dynamics | 0.850 | 0.021 | ✓ |
| Full mix | 0.874 | 0.004 | ✓ |
| Bass line | 0.732 | 0.086 | ❌ |
| Synth stabs | 0.655 | 0.419 | ❌ |
| Hat rejection | 0.990 | 0.031 | ✅ |
| 90 BPM | 1.000 | 0.000 | ✅ |
| 120 BPM | 0.997 | 0.008 | ✅ |
| 150 BPM | 0.762 | 0.034 | ⚠️ |

### BPM Tracking Performance

| Pattern | Expected | Mean Tracked | Error % | Variance |
|---------|----------|--------------|---------|----------|
| strong-beats | 120 | 121.4 | 1.2% | Low |
| full-mix | 120 | 101.2 | 15.7% | **High** |
| bass-line | 120 | 77.5 | 35.4% | High |
| steady-90 | 90 | 90.4 | 0.4% | Low |
| steady-120 | 120 | 121.5 | 1.3% | Low |
| fast-tempo | 150 | 148.8 | 0.8% | Low |

---

## Conclusion

The audio system performs **excellently on clean, calibrated drum patterns** (F1 > 0.99) but has **significant gaps in realistic music scenarios**:

1. **Bass detection is weak** - needs algorithm attention
2. **BPM tracking is unstable on complex audio** - needs smoothing/hysteresis
3. **High-variance patterns exist** - synth-stabs needs investigation

The system is production-ready for **drum-focused content** but needs improvement for **full-band music**.
