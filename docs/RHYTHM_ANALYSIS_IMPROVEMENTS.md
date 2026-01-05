# Rhythm Analysis Improvement Plan

**Created:** January 2026
**Based on:** Test Session 2026-01-04

---

## Executive Summary

Comprehensive testing of the rhythm analysis system revealed several critical issues and areas for improvement. The primary finding was that the **tempo prior was disabled by default**, causing severe half-time/double-time tracking confusion. Additional issues include missing parameter persistence, incomplete serial exposure, and fundamental algorithmic limitations with extreme tempos.

---

## 1. Test Results Summary

### 1.1 Tempo Prior Impact on BPM Tracking

| Configuration | 60 BPM Pattern | 120 BPM Pattern | 180 BPM Pattern |
|---------------|----------------|-----------------|-----------------|
| **Prior OFF** | 82.5 BPM (37% error) | 82.5 BPM (31% error) | ~60 BPM (67% error) |
| **Prior ON (width=40)** | 82.2 BPM (37% error) | **121.3 BPM (1% error)** | 95.5 BPM (47% error) |
| **Prior ON (width=50)** | 83.2 BPM (39% error) | **123.3 BPM (3% error)** | ~95 BPM (47% error) |
| **Prior ON (width=60)** | **60.8 BPM (1% error)** | ~120 BPM | ~95 BPM (47% error) |

**Key Finding:** Tempo prior is essential for 120 BPM tracking but cannot solve extreme tempo (60, 180 BPM) disambiguation due to fundamental autocorrelation harmonics.

### 1.2 Beat Stability Results (Inconclusive)

| Pattern | Expected Stability | Actual Stability | BPM Tracked |
|---------|-------------------|------------------|-------------|
| perfect-timing | >0.9 | 0.709 | 84.6 (wrong) |
| humanized-timing | 0.6-0.9 | 0.000 | 100.1 |
| unstable-timing | <0.5 | 0.827 | 72.4 (wrong) |

**Key Finding:** Beat stability metric is coupled to BPM tracking accuracy. When BPM is wrong, stability becomes meaningless. Fix BPM tracking first.

### 1.3 Test Pattern Issues Found

**Original Problem:** Test patterns used random sample selection per hit, breaking autocorrelation periodicity detection.

**Fix Applied:** Updated music mode patterns to use `deterministicHit()` with alternating sample variants (kick_hard_2/1, snare_hard_1/2) to mimic real music phrase structure.

---

## 2. Critical Issues Identified

### 2.1 Tempo Prior Disabled by Default

**Severity:** CRITICAL
**Impact:** 30-67% BPM tracking error on all patterns
**Root Cause:** `tempoPriorEnabled` defaults to `true` in code, but ConfigStorage doesn't persist it, and flash-stored value was `false`

**Fix Required:**
1. Add tempo prior parameters to `StoredMusicParams` in ConfigStorage.h
2. Ensure `tempoPriorEnabled = true` in `loadDefaults()`
3. Persist all tempo prior settings on save

### 2.2 Parameters Not Persisted to Flash

The following parameters are exposed via serial but NOT saved to flash:

| Category | Parameters | Impact |
|----------|------------|--------|
| **Tempo Prior** | `priorenabled`, `priorcenter`, `priorwidth`, `priorstrength` | Settings lost on reboot |
| **Stability** | `stabilitywin` | Settings lost on reboot |
| **Lookahead** | `lookahead` | Settings lost on reboot |
| **Tempo Smoothing** | `temposmooth`, `tempochgthresh` | Settings lost on reboot |
| **Hypothesis** | All 13 parameters | Settings lost on reboot |
| **Pulse Modulation** | `pulseboost`, `pulsesuppress`, `energyboost` | Settings lost on reboot |

### 2.3 Parameters Not Exposed via Serial

The following parameters exist in code but are NOT accessible via serial:

| Component | Parameters | Recommended Action |
|-----------|------------|-------------------|
| **AudioController** | `pulseNearBeatThreshold`, `pulseFarFromBeatThreshold` | Add to `rhythm` category |
| **EnsembleFusion** | `cooldownMs`, `minConfidence` | Add to `ensemble` category |

### 2.4 Algorithmic Limitations

**Half-Time/Double-Time Confusion:**
- Autocorrelation naturally produces stronger peaks at subharmonics
- At 60 BPM, the 120 BPM harmonic peak is often 2x stronger
- At 180 BPM, the 90 BPM harmonic peak is often 2x stronger
- Tempo prior can only weight peaks, not eliminate them

**Future Algorithm Improvements Needed:**
1. Harmonic relationship detection (if peak at N BPM, check N/2 and 2N)
2. Peak ratio analysis (subharmonic should be exactly 2x period)
3. Prefer hypothesis closest to prior center when strengths are similar
4. Dynamic prior adjustment based on detected tempo confidence

---

## 3. Recommended Default Values

### 3.1 Tempo Prior (NEW - Must be enabled)

```cpp
bool tempoPriorEnabled = true;      // MUST be true for correct BPM tracking
float tempoPriorCenter = 120.0f;    // Typical music tempo
float tempoPriorWidth = 50.0f;      // Balanced: covers 70-170 BPM well
float tempoPriorStrength = 0.5f;    // 50% blend between prior and autocorrelation
```

### 3.2 Multi-Hypothesis Tracker (Validated)

```cpp
float minPeakStrength = 0.3f;       // OK - filters noise peaks
float minRelativePeakHeight = 0.7f; // OK - rejects weak secondaries
float bpmMatchTolerance = 0.05f;    // OK - ±5% matching
float promotionThreshold = 0.15f;   // OK - prevents oscillation
uint16_t minBeatsBeforePromotion = 8; // OK - requires ~4s evidence
float phraseHalfLifeBeats = 32.0f;  // OK - 8 bars decay
float minStrengthToKeep = 0.1f;     // OK - cleanup threshold
uint32_t silenceGracePeriodMs = 3000; // OK - 3s grace
float silenceDecayHalfLifeSec = 5.0f; // OK - gradual decay
float strengthWeight = 0.5f;        // OK - balanced confidence
float consistencyWeight = 0.3f;     // OK
float longevityWeight = 0.2f;       // OK
```

### 3.3 Stability & Tempo Smoothing (Validated)

```cpp
float stabilityWindowBeats = 8.0f;  // OK - 2 bars of history
float beatLookaheadMs = 50.0f;      // OK - visual anticipation
float tempoSmoothingFactor = 0.85f; // OK - smooth changes
float tempoChangeThreshold = 0.1f;  // OK - 10% change detection
```

### 3.4 Ensemble Fusion (From Previous Testing)

```cpp
uint16_t cooldownMs = 80;           // Validated - reduces false positives
float minConfidence = 0.3f;         // OK - filters low-confidence detections
```

---

## 4. Firmware Improvements

### 4.1 ConfigStorage Updates (HIGH PRIORITY)

**File:** `blinky-things/config/ConfigStorage.h`

Add to `StoredMusicParams`:
```cpp
struct StoredMusicParams {
    // Existing
    float activationThreshold;
    float bpmMin;
    float bpmMax;
    float phaseAdaptRate;

    // NEW: Tempo prior parameters
    bool tempoPriorEnabled;
    float tempoPriorCenter;
    float tempoPriorWidth;
    float tempoPriorStrength;

    // NEW: Beat modulation
    float pulseBoostOnBeat;
    float pulseSuppressOffBeat;
    float energyBoostOnBeat;

    // NEW: Stability and smoothing
    float stabilityWindowBeats;
    float beatLookaheadMs;
    float tempoSmoothingFactor;
    float tempoChangeThreshold;
};
```

**Note:** This requires incrementing `CONFIG_VERSION` and updating the `static_assert` size check.

### 4.2 SerialConsole Updates (MEDIUM PRIORITY)

**File:** `blinky-things/inputs/SerialConsole.cpp`

Add missing parameter exposure:
```cpp
// Pulse modulation thresholds (category: rhythm)
settings_.registerFloat("pulsenear", &audioCtrl_->pulseNearBeatThreshold, "rhythm",
    "Near-beat threshold for pulse boost", 0.0f, 0.5f);
settings_.registerFloat("pulsefar", &audioCtrl_->pulseFarFromBeatThreshold, "rhythm",
    "Far-from-beat threshold for pulse suppress", 0.2f, 0.5f);

// Ensemble fusion (category: ensemble)
settings_.registerUint16("ensemblecooldown", &audioCtrl_->getEnsemble().getFusion().cooldownMs_, "ensemble",
    "Cooldown between detections (ms)", 20, 200);
settings_.registerFloat("ensembleminconf", &audioCtrl_->getEnsemble().getFusion().minConfidence_, "ensemble",
    "Minimum confidence for detection", 0.0f, 1.0f);
```

### 4.3 Default Value Fixes (HIGH PRIORITY)

**File:** `blinky-things/config/ConfigStorage.cpp`

In `loadDefaults()`:
```cpp
// Ensure tempo prior is ENABLED by default
data_.music.tempoPriorEnabled = true;
data_.music.tempoPriorCenter = 120.0f;
data_.music.tempoPriorWidth = 50.0f;
data_.music.tempoPriorStrength = 0.5f;
```

### 4.4 Algorithm Improvements (LOW PRIORITY - Future)

**Harmonic Disambiguation:**
1. After finding autocorrelation peaks, check for harmonic relationships
2. If peak at lag L has strength S, check lag 2L:
   - If lag 2L has strength > 0.5*S, prefer lag 2L (it's the fundamental)
3. Prefer hypothesis closest to `tempoPriorCenter` when multiple have similar confidence

---

## 5. Testing Suite Improvements

### 5.1 Pattern Improvements (COMPLETED)

Music mode patterns updated to use deterministic samples:
- `steady-120bpm`: Alternating kick_hard_2/1, snare_hard_1/2
- `steady-60bpm`: Same structure
- `steady-90bpm`: Same structure
- `steady-180bpm`: Same structure
- `perfect-timing`: Metronomic with consistent samples
- `humanized-timing`: ±20ms jitter with consistent samples
- `unstable-timing`: ±100ms jitter with consistent samples
- `tempo-ramp`: Gradual tempo change 80→160 BPM

### 5.2 Test Framework Improvements

**File:** `blinky-test-player/src/music-mode-runner.ts`

1. **Pre-test state clearing:**
   ```typescript
   async clearRhythmState(): Promise<void> {
     // Send command to reset all hypotheses
     await this.sendCommand('reset rhythm');
     // Wait for hypotheses to clear
     await this.delay(500);
   }
   ```

2. **Expected BPM validation:**
   - Pattern definitions should include `expectedBpm` field
   - Test runner should calculate BPM error automatically
   - Fail tests where BPM error > 10%

3. **Hypothesis state capture:**
   - Capture `json hypotheses` before and after each test
   - Include in test results for debugging
   - Track hypothesis creation/promotion events

### 5.3 New Test Commands

Add to `blinky-serial-mcp`:

```typescript
// Reset rhythm tracking state
async resetRhythm(): Promise<void> {
  await this.sendCommand('reset rhythm');
}

// Get detailed hypothesis state
async getHypotheses(): Promise<HypothesisState[]> {
  const response = await this.sendCommand('json hypotheses');
  return JSON.parse(response).hypotheses;
}

// Run test with state isolation
async runIsolatedTest(pattern: string, options: TestOptions): Promise<TestResult> {
  await this.resetRhythm();
  await this.delay(500);
  return await this.runTest(pattern, options);
}
```

### 5.4 Parameter Sweep Automation

**New tuner command:** `npm run tuner -- sweep-tempo-prior`

Sweep tempo prior parameters and record BPM accuracy:
- `priorcenter`: [100, 110, 120, 130]
- `priorwidth`: [30, 40, 50, 60, 70]
- `priorstrength`: [0.3, 0.4, 0.5, 0.6, 0.7]

Test patterns: `steady-60bpm`, `steady-120bpm`, `steady-180bpm`

Optimization goal: Minimize average BPM error across all tempos

---

## 6. Implementation Priority

### Phase 1: Critical Fixes (Immediate)

1. **Verify tempo prior is enabled on device** ✓ (Done: saved to flash)
2. Add tempo prior to ConfigStorage persistence
3. Set `tempoPriorEnabled = true` in `loadDefaults()`
4. Increment CONFIG_VERSION

### Phase 2: Settings Persistence (1-2 days)

1. Expand `StoredMusicParams` with all rhythm parameters
2. Update `loadConfiguration()` and `saveConfiguration()` to include new params
3. Update static_assert size checks
4. Test save/load cycle

### Phase 3: Serial Exposure (1 day)

1. Add missing parameter registrations
2. Document all settings in help text
3. Verify all categories show correctly

### Phase 4: Test Suite (2-3 days)

1. Add state isolation to test runner
2. Add hypothesis capture to test results
3. Create tempo prior sweep command
4. Validate all music mode patterns

### Phase 5: Algorithm Improvements (Future)

1. Research harmonic disambiguation approaches
2. Implement peak ratio analysis
3. Add adaptive prior center based on detection history

---

## 7. Validation Checklist

After implementing Phase 1-3, verify:

- [ ] `show tempoprior` returns `priorenabled = on` after fresh boot
- [ ] Changing `priorenabled` persists across power cycle
- [ ] `steady-120bpm` tracks within 5 BPM error
- [ ] All settings visible via `show` command
- [ ] All settings modifiable via `set` command
- [ ] `save` command persists all settings

After implementing Phase 4, verify:

- [ ] Test runner clears state between tests
- [ ] BPM error calculated for all music mode tests
- [ ] Hypothesis state included in test results
- [ ] Tempo prior sweep produces consistent results

---

## Appendix: Complete Settings Inventory

### Currently Exposed AND Persisted

| Setting | Category | Persisted | Default |
|---------|----------|-----------|---------|
| musicthresh | rhythm | Yes | 0.4 |
| phaseadapt | rhythm | Yes | 0.15 |
| bpmmin | rhythm | Yes | 60 |
| bpmmax | rhythm | Yes | 200 |

### Currently Exposed but NOT Persisted

| Setting | Category | Persisted | Default | Action |
|---------|----------|-----------|---------|--------|
| priorenabled | tempoprior | **No** | true | Add to StoredMusicParams |
| priorcenter | tempoprior | **No** | 120 | Add to StoredMusicParams |
| priorwidth | tempoprior | **No** | 40→50 | Add to StoredMusicParams |
| priorstrength | tempoprior | **No** | 0.5 | Add to StoredMusicParams |
| stabilitywin | stability | **No** | 8 | Add to StoredMusicParams |
| lookahead | lookahead | **No** | 50 | Add to StoredMusicParams |
| temposmooth | tempo | **No** | 0.85 | Add to StoredMusicParams |
| tempochgthresh | tempo | **No** | 0.1 | Add to StoredMusicParams |
| pulseboost | rhythm | **No** | 1.3 | Add to StoredMusicParams |
| pulsesuppress | rhythm | **No** | 0.6 | Add to StoredMusicParams |
| energyboost | rhythm | **No** | 0.3 | Add to StoredMusicParams |
| minpeakstr | hypothesis | **No** | 0.3 | Add to StoredMusicParams |
| minrelheight | hypothesis | **No** | 0.7 | Add to StoredMusicParams |
| bpmmatchtol | hypothesis | **No** | 0.05 | Add to StoredMusicParams |
| promothresh | hypothesis | **No** | 0.15 | Add to StoredMusicParams |
| minbeats | hypothesis | **No** | 8 | Add to StoredMusicParams |
| phrasehalf | hypothesis | **No** | 32 | Add to StoredMusicParams |
| minstr | hypothesis | **No** | 0.1 | Add to StoredMusicParams |
| silencegrace | hypothesis | **No** | 3000 | Add to StoredMusicParams |
| silencehalf | hypothesis | **No** | 5.0 | Add to StoredMusicParams |
| strweight | hypothesis | **No** | 0.5 | Add to StoredMusicParams |
| consweight | hypothesis | **No** | 0.3 | Add to StoredMusicParams |
| longweight | hypothesis | **No** | 0.2 | Add to StoredMusicParams |

### NOT Exposed (Should Be)

| Parameter | Location | Recommended Category |
|-----------|----------|---------------------|
| pulseNearBeatThreshold | AudioController | rhythm |
| pulseFarFromBeatThreshold | AudioController | rhythm |
| cooldownMs | EnsembleFusion | ensemble |
| minConfidence | EnsembleFusion | ensemble |

---

*Document generated from test session 2026-01-04*
