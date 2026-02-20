# Common Musical Scenario Test Plan

**Objective**: Evaluate transient detection, rhythm analysis, and phase tracking accuracy in realistic musical contexts. Focus on reliability and consistency, not edge case handling.

## Test Philosophy

- **Common over exotic**: Test patterns that represent 80% of real music, not corner cases
- **Consistency matters**: Run each test 3x minimum to measure variance
- **Multi-metric evaluation**: F1 alone isn't enough; timing accuracy and BPM stability matter equally
- **Baseline establishment**: Document current performance before any changes

---

## Test Suite 1: Core Transient Detection (Drums)

**Purpose**: Verify reliable detection of common drum patterns across typical tempos.

| Test | Pattern | BPM | Runs | Target F1 | Measures |
|------|---------|-----|------|-----------|----------|
| 1.1 | strong-beats | 120 | 3 | ≥0.95 | Baseline - should be near-perfect |
| 1.2 | basic-drums | 120 | 3 | ≥0.90 | Full kit detection |
| 1.3 | full-kit | 115 | 3 | ≥0.85 | Multi-instrument separation |
| 1.4 | mixed-dynamics | 120 | 3 | ≥0.80 | Loudness variation handling |

**Execution**:
```
run_test(pattern: "strong-beats", port: "COM11")  // x3
run_test(pattern: "basic-drums", port: "COM11")   // x3
run_test(pattern: "full-kit", port: "COM11")      // x3
run_test(pattern: "mixed-dynamics", port: "COM11") // x3
```

**Success Criteria**:
- All tests meet target F1
- Variance across 3 runs < 0.05 F1
- No systematic precision/recall imbalance (both > 0.80)

---

## Test Suite 2: Realistic Full Mix

**Purpose**: Test detection accuracy in realistic multi-instrument music where drums coexist with bass, synth, and melodic content.

| Test | Pattern | BPM | Runs | Target F1 | Notes |
|------|---------|-----|------|-----------|-------|
| 2.1 | full-mix | 120 | 5 | ≥0.85 | Drums + bass + synth + lead |
| 2.2 | bass-line | 120 | 3 | ≥0.80 | Kick + bass interaction |
| 2.3 | synth-stabs | 120 | 3 | ≥0.90 | Sharp synth attacks |

**Analysis Focus**:
- Does bass content cause false positives on non-kick hits?
- Do synth attacks trigger correctly or get confused with drums?
- Is precision dropping (FPs) or recall dropping (misses)?

---

## Test Suite 3: BPM Range (Common Tempos)

**Purpose**: Verify accurate BPM tracking across the most common musical tempo range (80-140 BPM covers ~90% of popular music).

| Test | Pattern | Expected BPM | Runs | Target Error | Notes |
|------|---------|--------------|------|--------------|-------|
| 3.1 | steady-90bpm | 90 | 3 | ≤5% | Slow pop/R&B |
| 3.2 | steady-120bpm | 120 | 3 | ≤2% | Center of tempo prior |
| 3.3 | steady-140bpm | 140 | 3 | ≤5% | Fast pop/EDM |

**Execution with BPM validation**:
```
run_test(pattern: "steady-90bpm", port: "COM11")
// Check: music.bpm within 85.5-94.5 (±5%)
// Check: music.confidence > 0.6
// Check: music.activationMs < 3000 (locks within 3 seconds)
```

**Success Criteria**:
- BPM error ≤5% at 90/140, ≤2% at 120
- Activation time < 3 seconds consistently
- Confidence > 0.6 after stabilization
- No half-time or double-time locking (90→45 or 90→180)

---

## Test Suite 4: Phase Synchronization

**Purpose**: Verify that detected transients align with tracked phase (transients should occur near phase=0).

| Test | Pattern | Runs | Measures |
|------|---------|------|----------|
| 4.1 | phase-on-beat | 3 | Phase alignment of detected transients |
| 4.2 | steady-120bpm | 3 | Phase stability (variance) |

**Analysis Method**:
1. Run test, collect streaming data
2. For each detected transient, record current phase value
3. Compute: mean phase at transient, phase variance
4. Expected: Mean phase < 0.15, variance < 0.05

**Manual Analysis Required**:
```
connect(port: "COM11")
stream_start()
// Play steady-120bpm pattern manually
// Record ~30 seconds of streaming data
// Analyze phase values when pulse > 0.3
stream_stop()
disconnect()
```

---

## Test Suite 5: Consistency & Variance

**Purpose**: Measure system reliability by running the same test multiple times and measuring variance.

| Test | Pattern | Runs | Measures |
|------|---------|------|----------|
| 5.1 | strong-beats | 10 | F1 variance, min/max spread |
| 5.2 | full-mix | 10 | F1 variance, min/max spread |

**Success Criteria**:
- Standard deviation < 0.03 F1
- No outlier runs (all within ±0.05 of mean)
- Systematic latency consistent (±10ms)

**This test identifies**:
- Environmental noise sensitivity
- AGC stability
- Detection threshold flapping

---

## Test Suite 6: Medium Difficulty (Known Challenges)

**Purpose**: Establish baseline performance on patterns that are known to be challenging but still represent common music scenarios.

| Test | Pattern | Runs | Current F1 | Notes |
|------|---------|------|------------|-------|
| 6.1 | medium-beats | 3 | ~0.85 | Not as loud as strong-beats |
| 6.2 | soft-beats | 3 | ~0.70? | Low loudness detection limits |
| 6.3 | hat-rejection | 3 | ~0.80? | Verify hats don't trigger |

**Analysis Focus**:
- Where does detection start failing as loudness drops?
- Are hi-hats correctly rejected or causing FPs?
- Is the threshold appropriately balanced?

---

## Excluded Tests (Edge Cases)

The following patterns are explicitly **excluded** from this evaluation as they represent edge cases, not common scenarios:

| Pattern | Reason for Exclusion |
|---------|---------------------|
| sparse | Very few hits, tests recovery time not accuracy |
| tempo-sweep | Multi-tempo, tests adaptation not steady-state |
| tempo-ramp | Continuous change, unrealistic |
| lead-melody | Known algorithmic limitation (needs pitch tracking) |
| pad-rejection | Sustained tones, needs algorithm fix not tuning |
| chord-rejection | Same as pad-rejection |
| cooldown-stress-* | Artificial stress tests |
| non-musical-* | Random noise, not music |
| steady-40bpm | Extremely rare tempo |
| steady-200bpm+ | Extremely rare tempo |

---

## Execution Protocol

### Before Testing

1. **Hardware setup**:
   - Device connected via USB
   - Speaker volume at consistent level (60-70%)
   - Room ambient noise minimal
   - No other audio playing

2. **Firmware state**:
   - Reset to defaults: `reset_defaults()`
   - Save clean state: `save_settings()`
   - Verify connection: `status()`

3. **Document baseline settings**:
   ```
   get_settings()
   // Record: onsetthresh, minconf, agree_1, cooldown, etc.
   ```

### Test Execution Order

Run tests in this order to establish baseline before variants:

1. **Suite 1**: Core drums (establish detection baseline)
2. **Suite 5.1**: Strong-beats 10x (measure variance)
3. **Suite 2**: Full mix (realistic scenario)
4. **Suite 5.2**: Full-mix 10x (measure realistic variance)
5. **Suite 3**: BPM range (tempo tracking)
6. **Suite 4**: Phase sync (phase alignment)
7. **Suite 6**: Medium difficulty (challenge boundaries)

### Between Tests

- Wait 2 seconds between runs (let AGC settle)
- No parameter changes during suite
- Record any anomalies (noise, interruptions)

### Data Recording

For each test run, record:
```json
{
  "suite": "1",
  "test": "1.1",
  "pattern": "strong-beats",
  "run": 1,
  "timestamp": "2026-02-17T...",
  "results": {
    "f1": 0.95,
    "precision": 0.96,
    "recall": 0.94,
    "tp": 32, "fp": 1, "fn": 2,
    "bpm": 120.1,
    "bpmError": 0.08,
    "confidence": 0.82,
    "activationMs": 1500,
    "avgTimingErrorMs": 42
  },
  "notes": ""
}
```

---

## Analysis Framework

### Transient Detection Health

| Metric | Good | Acceptable | Problem |
|--------|------|------------|---------|
| F1 | ≥0.90 | 0.80-0.90 | <0.80 |
| Precision | ≥0.85 | 0.75-0.85 | <0.75 (too many FPs) |
| Recall | ≥0.90 | 0.80-0.90 | <0.80 (missing hits) |
| Variance | <0.03 | 0.03-0.05 | >0.05 (inconsistent) |

### BPM Tracking Health

| Metric | Good | Acceptable | Problem |
|--------|------|------------|---------|
| BPM Error | ≤2% | 2-5% | >5% |
| Activation | <2s | 2-4s | >4s |
| Confidence | >0.7 | 0.5-0.7 | <0.5 |
| Stability | >0.8 | 0.6-0.8 | <0.6 |

### Phase Alignment Health

| Metric | Good | Acceptable | Problem |
|--------|------|------------|---------|
| Mean phase @ transient | <0.1 | 0.1-0.2 | >0.2 |
| Phase variance | <0.03 | 0.03-0.08 | >0.08 |

---

## Expected Findings

Based on the codebase analysis, we expect to find:

### Likely Strengths
1. **Strong drum detection**: F1 > 0.95 on clean drum patterns
2. **120 BPM tracking**: Very accurate due to tempo prior centering
3. **Consistency**: Low variance on calibrated patterns

### Likely Weaknesses
1. **Full mix precision drop**: FPs from bass/synth content
2. **Soft detection limits**: Recall drops below certain loudness
3. **90/140 BPM accuracy**: Slightly worse than 120 BPM
4. **Phase sync after transient corrections**: May see drift

### Unknown (To Be Discovered)
1. Variance on full-mix (realistic music)
2. Phase alignment quality in practice
3. BPM activation time consistency
4. Hat rejection effectiveness

---

## Post-Test Actions

After completing all tests:

1. **Compile results** into summary table
2. **Identify patterns** with F1 < 0.85 (needs attention)
3. **Identify variance** issues (inconsistent results)
4. **Prioritize fixes** based on common scenario impact
5. **Document findings** in PARAMETER_TUNING_HISTORY.md

---

## Quick Reference: MCP Commands

```javascript
// List available patterns
list_patterns()

// Run a single test
run_test(pattern: "strong-beats", port: "COM11")

// Get current settings
connect(port: "COM11")
get_settings()

// Monitor BPM tracking
monitor_music(duration_ms: 10000, expected_bpm: 120)

// Get beat tracker state
get_beat_state()

// Disconnect when done
disconnect()
```
