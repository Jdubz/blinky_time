# Rhythm Analysis Test Plan

**Created:** January 2026
**Target:** AudioController v3 with tempo prior, beat stability, and continuous tempo estimation

---

## Executive Summary

This test plan validates the new rhythm analysis features added in January 2026:
1. **Tempo Prior Distribution** - Reduces half-time/double-time confusion
2. **Beat Stability Metric** - Measures inter-beat interval consistency
3. **Beat Lookahead Prediction** - Zero-latency visual sync
4. **Continuous Tempo Estimation** - Kalman-like smoothing for tempo changes

**Estimated Duration:** 3-4 hours
**Required Equipment:** Device on COM port, speakers near mic, quiet environment

---

## Test Infrastructure

### Available Tools

| Tool | Purpose | Usage |
|------|---------|-------|
| `blinky-serial-mcp` | Device communication | `run_test`, `monitor_music`, `get_settings` |
| `MusicModeRunner` | BPM/phase metrics | `runPattern()`, `runPatterns()` |
| `HypothesisValidator` | Hypothesis tracking | `validateHypothesis()`, `runHypothesisValidationSuite()` |
| `blinky-test-player` | Pattern playback | `play <pattern>` |

### Key Serial Commands

```bash
# View rhythm state (human-readable)
rhythm

# Get rhythm state as JSON (for automation)
json rhythm

# Get hypothesis state as JSON
json hypotheses

# Set new parameters
set priorenabled 1
set priorcenter 120
set priorwidth 40
set priorstrength 0.5
set stabilitywin 8
set lookahead 50
set temposmooth 0.85
set tempochgthresh 0.1
```

### Test Patterns for Rhythm Analysis

| Pattern | BPM | Purpose |
|---------|-----|---------|
| `steady-60bpm` | 60 | Tempo prior subharmonic rejection (double-time) |
| `steady-90bpm` | 90 | Tempo prior intermediate weighting |
| `steady-120bpm` | 120 | Baseline reference |
| `steady-160bpm` | 160 | Fast tempo tracking |
| `steady-180bpm` | 180 | Tempo prior half-time rejection |
| `tempo-ramp` | 80→160 | Continuous tempo change adaptation |
| `tempo-sudden` | varies | Abrupt tempo change handling |
| `half-time-ambiguity` | 120 | Half-time/double-time disambiguation |
| `perfect-timing` | 120 | Beat stability reference (should be ~1.0) |
| `humanized-timing` | 120 | Beat stability with ±20ms jitter |
| `unstable-timing` | 120 | Beat stability with ±100ms jitter (should be low) |
| `silence-gaps` | 120 | Hypothesis decay during silence |
| `phase-on-beat` | 120 | Phase accuracy validation |

---

## Phase 1: Baseline Measurement (30 min)

### 1.1 Current State Capture

**Objective:** Establish baseline metrics before tuning

```bash
# Connect and get current settings
mcp: status
mcp: get_settings

# Run hypothesis validation suite
cd blinky-test-player
npm run tuner -- validate-hypothesis --port COM5 --gain 40
```

**Record These Metrics:**
- [ ] BPM error at 60, 90, 120, 160, 180 BPM
- [ ] Beat stability for perfect-timing pattern
- [ ] Time to first hypothesis (steady-120bpm)
- [ ] Promotion count for tempo changes
- [ ] Silence decay behavior

### 1.2 Identify Current Issues

Run full music mode validation:

```bash
npm run tuner -- music-validate --port COM5 --gain 40
```

**Expected Issues to Find:**
- Octave errors at 60 BPM or 180 BPM (tempo prior should fix)
- Low stability scores with humanized timing
- Slow adaptation to tempo changes

---

## Phase 2: Tempo Prior Validation (45 min)

### 2.1 Subharmonic Rejection Test (60 BPM)

**Hypothesis:** At 60 BPM, autocorrelation has strong peaks at 120 BPM (double-time). Tempo prior should weight 60 BPM correctly.

```bash
# Test with tempo prior disabled
mcp: set_setting --name priorenabled --value 0
mcp: run_test --pattern steady-60bpm --port COM5 --gain 40

# Record: Detected BPM (expect: may report 120)
# Record: BPM error

# Test with tempo prior enabled
mcp: set_setting --name priorenabled --value 1
mcp: set_setting --name priorcenter --value 100
mcp: run_test --pattern steady-60bpm --port COM5 --gain 40

# Record: Detected BPM (expect: closer to 60)
# Record: BPM error
```

**Success Criteria:**
- With prior enabled: BPM error < 5 at 60 BPM
- Improvement > 30% compared to prior disabled

### 2.2 Half-Time Rejection Test (180 BPM)

**Hypothesis:** At 180 BPM, autocorrelation has strong peaks at 90 BPM (half-time). Tempo prior should weight 180 BPM correctly.

```bash
# Similar test with steady-180bpm pattern
mcp: run_test --pattern steady-180bpm --port COM5 --gain 40

# Record: Detected BPM
# Record: tempoPriorWeight from json rhythm
```

**Success Criteria:**
- BPM error < 10 at 180 BPM
- No prolonged tracking at 90 BPM

### 2.3 Tempo Prior Parameter Sweep

**Parameters to sweep:**
- `priorcenter`: 100, 110, 120, 130 BPM
- `priorwidth`: 20, 30, 40, 50, 60 BPM
- `priorstrength`: 0.2, 0.3, 0.5, 0.7, 1.0

**Test patterns:** steady-60bpm, steady-90bpm, steady-120bpm, steady-180bpm

```bash
# Manual sweep example
mcp: set_setting --name priorcenter --value 110
mcp: set_setting --name priorwidth --value 30
mcp: set_setting --name priorstrength --value 0.5

# Run all tempo patterns and record avgBpmError
```

**Optimization Goal:** Minimize average BPM error across all tempo patterns

**Expected Optimal Values:**
- `priorcenter`: 110-120 BPM (typical music tempo)
- `priorwidth`: 30-50 BPM (not too narrow, not too wide)
- `priorstrength`: 0.3-0.6 (balance prior vs autocorrelation)

---

## Phase 3: Beat Stability Validation (30 min)

### 3.1 Stability Metric Baseline

**Objective:** Verify beat stability metric produces expected values

```bash
# Perfect timing should produce stability near 1.0
mcp: run_test --pattern perfect-timing --port COM5 --gain 40
# After pattern completes:
mcp: send_command --command "json rhythm"
# Record: beatStability

# Humanized timing should produce stability 0.6-0.9
mcp: run_test --pattern humanized-timing --port COM5 --gain 40
mcp: send_command --command "json rhythm"
# Record: beatStability

# Unstable timing should produce stability < 0.5
mcp: run_test --pattern unstable-timing --port COM5 --gain 40
mcp: send_command --command "json rhythm"
# Record: beatStability
```

**Success Criteria:**
| Pattern | Expected Stability |
|---------|-------------------|
| perfect-timing | > 0.9 |
| humanized-timing | 0.6 - 0.9 |
| unstable-timing | < 0.5 |

### 3.2 Stability Window Parameter Sweep

**Parameter:** `stabilitywin` (number of beats to track)

**Values to test:** 4, 6, 8, 12, 16 beats

```bash
for win in 4 6 8 12 16; do
  mcp: set_setting --name stabilitywin --value $win
  # Run humanized-timing and record stability variance
done
```

**Trade-offs:**
- Smaller window: More responsive but noisier
- Larger window: Smoother but slower to adapt

**Expected Optimal:** 6-10 beats

---

## Phase 4: Multi-Hypothesis Tracking (45 min)

### 4.1 Half-Time Ambiguity Resolution

**Objective:** Verify that half-time-ambiguity pattern is correctly resolved

```bash
mcp: run_test --pattern half-time-ambiguity --port COM5 --gain 40

# During playback, monitor hypotheses:
mcp: send_command --command "json hypotheses"
```

**Expected Behavior:**
1. Two hypotheses created: 60 BPM and 120 BPM
2. 120 BPM hypothesis should gain confidence faster
3. Final primary should be 120 BPM (or 60 BPM if pattern is truly half-time)

### 4.2 Hypothesis Promotion Test

**Objective:** Verify hypotheses are promoted correctly after tempo changes

```bash
mcp: run_test --pattern tempo-sudden --port COM5 --gain 40
```

**Monitor During Playback:**
- Number of hypothesis creations
- Promotion events (visible in debug output)
- Time to switch primary after tempo change

**Success Criteria:**
- Promotion occurs within 8-16 beats of tempo change
- No oscillation between hypotheses

### 4.3 Silence Decay Test

**Objective:** Verify hypotheses decay appropriately during silence gaps

```bash
mcp: run_test --pattern silence-gaps --port COM5 --gain 40
```

**Expected Behavior:**
1. Hypothesis maintains strength during music
2. Grace period (3s) before decay starts
3. After extended silence, hypotheses become inactive
4. Quick re-lock when music resumes

**Parameters to verify:**
- `silenceGracePeriodMs`: 3000
- `silenceDecayHalfLifeSec`: 5.0

---

## Phase 5: Continuous Tempo Estimation (30 min)

### 5.1 Tempo Ramp Tracking

**Objective:** Verify smooth tracking of gradual tempo changes

```bash
mcp: run_test --pattern tempo-ramp --port COM5 --gain 40

# Monitor tempo velocity during playback:
mcp: send_command --command "json rhythm"
# Record: tempoVelocity (should be non-zero during ramp)
```

**Expected Behavior:**
- `tempoVelocity` positive during acceleration
- `tempoVelocity` negative during deceleration
- BPM tracks within 5% of actual tempo throughout

### 5.2 Tempo Smoothing Parameter Sweep

**Parameters:**
- `temposmooth`: 0.7, 0.8, 0.85, 0.9, 0.95
- `tempochgthresh`: 0.05, 0.1, 0.15, 0.2

**Test pattern:** tempo-ramp

```bash
# Sweep temposmooth
for smooth in 0.7 0.8 0.85 0.9 0.95; do
  mcp: set_setting --name temposmooth --value $smooth
  # Run tempo-ramp and record:
  # - Average BPM error during ramp
  # - BPM jitter (stddev)
done
```

**Trade-offs:**
- Lower smoothing: Faster response, more jitter
- Higher smoothing: Slower response, less jitter

**Expected Optimal:** 0.80-0.90

---

## Phase 6: Beat Lookahead Validation (20 min)

### 6.1 Next Beat Prediction Accuracy

**Objective:** Verify `nextBeatMs` predictions are accurate

```bash
# Start streaming
mcp: stream_start

# Play steady-120bpm and monitor:
# - nextBeatMs values
# - Compare predicted vs actual beat times

mcp: stream_stop
```

**Analysis:**
- Extract `nextBeatMs` from JSON stream
- Compare to actual beat times (500ms apart at 120 BPM)
- Calculate prediction error

**Success Criteria:**
- Average prediction error < 50ms
- No predictions in the past (nextBeatMs >= nowMs)

### 6.2 Lookahead Parameter Test

**Parameter:** `lookahead` (ms)

**Values to test:** 0, 25, 50, 75, 100 ms

```bash
for look in 0 25 50 75 100; do
  mcp: set_setting --name lookahead --value $look
  # Run steady-120bpm and observe visual sync
done
```

**Subjective Assessment:**
- Visual effect fires slightly before audio beat
- No perceivable delay between beat and visual

---

## Phase 7: Integration Testing (30 min)

### 7.1 Full Music Mode Validation

**Objective:** Verify all components work together

```bash
npm run tuner -- music-validate --port COM5 --gain 40 \
  --patterns steady-60bpm,steady-90bpm,steady-120bpm,steady-160bpm,steady-180bpm,tempo-ramp,tempo-sudden,half-time-ambiguity,perfect-timing,humanized-timing,unstable-timing,silence-gaps
```

**Record Full Results:**
- Per-pattern BPM accuracy
- Per-pattern phase stability
- Per-pattern lock time
- Overall average metrics

### 7.2 Parameter Interaction Test

**Objective:** Verify parameters don't conflict

Test combinations:
1. High tempo prior + low smoothing
2. Wide stability window + fast tempo
3. Long lookahead + unstable timing

---

## Phase 8: Optimal Configuration (20 min)

### 8.1 Determine Best Settings

Based on test results, determine optimal values:

| Parameter | Tested Range | Optimal | Justification |
|-----------|--------------|---------|---------------|
| `priorenabled` | 0, 1 | 1 | Reduces octave errors |
| `priorcenter` | 100-130 | TBD | Minimizes avg BPM error |
| `priorwidth` | 20-60 | TBD | Balance specificity/coverage |
| `priorstrength` | 0.2-1.0 | TBD | Balance prior/data |
| `stabilitywin` | 4-16 | TBD | Balance response/noise |
| `lookahead` | 0-100 | TBD | Visual sync preference |
| `temposmooth` | 0.7-0.95 | TBD | Balance response/jitter |
| `tempochgthresh` | 0.05-0.2 | TBD | Change sensitivity |

### 8.2 Save and Verify

```bash
# Set all optimal values
mcp: set_setting --name priorcenter --value <optimal>
# ... repeat for all parameters ...

# Verify settings
mcp: get_settings

# Save to flash
mcp: save_settings

# Final validation run
npm run tuner -- music-validate --port COM5 --gain 40
```

---

## Success Criteria Summary

| Metric | Target | Current (2026-01-04) | Status |
|--------|--------|----------------------|--------|
| Avg BPM error (all tempos) | < 5 BPM | ~20 BPM | **PARTIAL** - 120 BPM works, extremes fail |
| BPM error at 60 BPM | < 5 BPM | 23 BPM (83 detected) | **FAIL** - Prior biases toward 120 |
| BPM error at 120 BPM | < 5 BPM | **3 BPM (123 detected)** | **PASS** |
| BPM error at 180 BPM | < 10 BPM | 82 BPM (98 detected) | **FAIL** - Strong half-time bias |
| Beat stability (perfect) | > 0.9 | 0.709 | **FAIL** - Coupled to BPM accuracy |
| Beat stability (humanized) | 0.6-0.9 | 0.000 | **INCONCLUSIVE** |
| Time to lock (120 BPM) | < 4s | ~15ms | **PASS** |
| Half-time resolution | > 90% correct | ~50% | **FAIL** - Algorithm limitation |

### Key Findings (2026-01-04)

1. **Root Cause Fixed:** Tempo prior was disabled by default. Enabling it fixed 120 BPM tracking.
2. **Settings Saved:** Optimal tempo prior settings saved to flash (center=120, width=50, strength=0.5)
3. **Algorithm Limitation:** Autocorrelation cannot reliably distinguish extreme tempos (60, 180 BPM) from harmonics
4. **Stability Metric Issue:** Beat stability is coupled to BPM tracking accuracy - meaningless when BPM is wrong

### Next Steps

See [RHYTHM_ANALYSIS_IMPROVEMENTS.md](./RHYTHM_ANALYSIS_IMPROVEMENTS.md) for comprehensive improvement plan.

---

## Appendix: Quick Test Commands

```bash
# Single pattern quick test
mcp: run_test --pattern steady-120bpm --port COM5 --gain 40

# Monitor rhythm state live
mcp: stream_start
# (observe JSON output)
mcp: stream_stop

# Get current rhythm metrics
mcp: send_command --command "json rhythm"

# Get hypothesis state
mcp: send_command --command "json hypotheses"

# Full hypothesis validation suite
npm run tuner -- validate-hypothesis --port COM5 --gain 40

# Music mode validation
npm run tuner -- music-validate --port COM5 --gain 40
```
