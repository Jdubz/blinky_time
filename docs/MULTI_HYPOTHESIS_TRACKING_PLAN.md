# Multi-Hypothesis Tempo Tracking - Implementation Plan

**Author**: Claude Code
**Date**: 2026-01-03
**Status**: ✅ IMPLEMENTED (January 3, 2026)
**Related**: AUDIO_ARCHITECTURE.md, IMPROVEMENT_PLAN.md

**Implementation Note**: This plan has been fully implemented as AudioController v3. See commits:
- `8d3bfd5` - Implement multi-hypothesis tempo tracking (AudioController v3)
- `656ed59` - Fix 9 critical/high/medium bugs in multi-hypothesis tempo tracking
- `7d50f3f` - Address all PR #36 code review feedback
- `8c58486` - Fix static analysis issues (cppcheck)

---

## Executive Summary

This document outlines the implementation plan for **multi-hypothesis tempo tracking** in the AudioController. The current single-hypothesis autocorrelation approach works well for consistent rhythms but struggles with:

- **Tempo ambiguity**: Half-time vs. double-time confusion (60 BPM vs. 120 BPM)
- **Tempo changes**: Abrupt transitions between song sections
- **Polyrhythmic patterns**: Multiple valid tempos simultaneously
- **Genre switching**: DJ mixes, live performances with tempo shifts

Multi-hypothesis tracking maintains 4 concurrent tempo hypotheses, allowing the system to gracefully handle tempo changes and ambiguity.

---

## 1. Motivation and Design Goals

### 1.1 Current System Limitations

The single-hypothesis autocorrelation approach (AudioController v2) has these issues:

1. **Tempo ambiguity**: Autocorrelation peak at lag=60 could mean 120 BPM @ 60Hz or 60 BPM @ 30Hz
2. **Slow adaptation**: Takes 6 seconds to switch from one tempo to another
3. **No memory**: Cannot quickly return to a previous tempo after a breakdown/bridge
4. **Fragile to tempo changes**: Gradual tempo drift or sudden shifts cause rhythm tracking to "lose lock"

### 1.2 Design Goals

1. **Track 4 concurrent hypotheses**: Primary, secondary, tertiary, and candidate
2. **LRU eviction strategy**: Replace least-recently-used hypothesis when creating new ones
3. **Phrase-aware decay**: Decay based on beat count, not wall-clock time
4. **Tempo history**: Remember recent tempos for quick re-acquisition
5. **Memory efficient**: <1 KB additional RAM usage
6. **CPU efficient**: <2% additional CPU @ 64 MHz

### 1.3 Inspiration from Literature

This design draws from:

- **Ellis (2007)**: Dynamic Programming beat tracker with tempo transition matrix
- **Davies & Plumbley (2007)**: Multiple hypothesis beat tracking
- **OBTAIN (2017)**: Probabilistic multi-agent tracking with particle filters

Our simplified approach uses fixed-slot hypothesis tracking instead of particle filters for memory efficiency.

---

## 2. Architecture Design

### 2.1 Data Structures

```cpp
/**
 * TempoHypothesis - A single tempo tracking hypothesis
 */
struct TempoHypothesis {
    // Tempo estimate
    float bpm;                      // BPM estimate (60-200)
    float beatPeriodMs;             // Beat period in milliseconds

    // Phase tracking
    float phase;                    // Current beat phase (0-1)

    // Confidence and evidence
    float strength;                 // Periodicity strength (0-1)
    float confidence;               // Overall confidence (0-1)

    // Timing and history
    uint32_t lastUpdateMs;          // Last update timestamp
    uint16_t beatCount;             // Number of beats tracked
    uint32_t createdMs;             // Creation timestamp

    // Autocorrelation evidence
    float correlationPeak;          // Peak autocorrelation value
    int lagSamples;                 // Lag in samples (for verification)

    // State
    bool active;                    // Is this slot in use?
    uint8_t priority;               // 0=primary, 1=secondary, 2=tertiary, 3=candidate

    TempoHypothesis()
        : bpm(120.0f)
        , beatPeriodMs(500.0f)
        , phase(0.0f)
        , strength(0.0f)
        , confidence(0.0f)
        , lastUpdateMs(0)
        , beatCount(0)
        , createdMs(0)
        , correlationPeak(0.0f)
        , lagSamples(0)
        , active(false)
        , priority(3)
    {}
};

/**
 * MultiHypothesisTracker - Manages 4 concurrent tempo hypotheses
 */
class MultiHypothesisTracker {
public:
    static constexpr int MAX_HYPOTHESES = 4;

    // Hypothesis slots: [0]=primary, [1]=secondary, [2]=tertiary, [3]=candidate
    TempoHypothesis hypotheses[MAX_HYPOTHESES];

    // Current active hypothesis (0-3)
    uint8_t activeHypothesis = 0;

    // ...methods defined below...
};
```

### 2.2 Memory Requirements

```
Per TempoHypothesis:
- 4 floats (bpm, beatPeriodMs, phase, strength)        = 16 bytes
- 4 floats (confidence, correlationPeak)               = 16 bytes
- 3 uint32_t (lastUpdateMs, createdMs)                 = 12 bytes
- 1 uint16_t (beatCount)                               = 2 bytes
- 1 int (lagSamples)                                   = 4 bytes
- 2 uint8_t (active, priority)                         = 2 bytes
                                                  Total = 52 bytes

MultiHypothesisTracker:
- 4 hypotheses                                         = 208 bytes
- 1 uint8_t activeHypothesis                           = 1 byte
- Overhead/alignment                                   = ~15 bytes
                                                  Total = ~224 bytes
```

**Total additional memory**: ~224 bytes (well within our <1 KB goal)

### 2.3 Hypothesis Management Strategy

#### Hypothesis Priority Slots

```
[0] PRIMARY:    Current best hypothesis, drives phase output
[1] SECONDARY:  Strong alternative (e.g., half-time/double-time)
[2] TERTIARY:   Weaker alternative or recent previous tempo
[3] CANDIDATE:  New hypothesis being evaluated
```

#### LRU Eviction Strategy

When creating a new hypothesis:
1. Check if any slot is inactive → use that slot
2. If all slots active, find slot with oldest `lastUpdateMs`
3. If oldest slot is PRIMARY and still strong, evict TERTIARY instead
4. Replace evicted slot with new hypothesis

#### Promotion Strategy

Each frame:
1. Update all active hypotheses with new autocorrelation data
2. Compute confidence for each hypothesis based on:
   - Periodicity strength (autocorrelation peak)
   - Phase consistency (how well predicted phase matches data)
   - Longevity (beat count)
3. If non-primary hypothesis has confidence > primary + threshold:
   - Promote non-primary to PRIMARY
   - Demote old primary to SECONDARY

### 2.4 Phrase-Aware Decay

Instead of time-based decay (5-second half-life), use **beat-count decay**:

```cpp
// Decay strength based on beats since last significant update
void decayHypothesis(TempoHypothesis& hypo, uint32_t nowMs, float dt) {
    // Advance beat count (fractional beats)
    float beatIncrement = dt * 1000.0f / hypo.beatPeriodMs;
    float beatsSinceUpdate = beatIncrement;  // Accumulate in a float member

    // Decay per beat: half-life = 32 beats (8 bars of 4/4)
    // decay_factor = exp(-ln(2) * beats / 32) = exp(-0.02166 * beats)
    float decayFactor = expf(-0.02166f * beatsSinceUpdate);
    hypo.strength *= decayFactor;

    // Stop tracking if strength drops below threshold
    if (hypo.strength < 0.1f) {
        hypo.active = false;
    }
}
```

**Why phrase-aware?**
- Musical phrases are typically 4, 8, 16, or 32 bars
- A 32-beat (8-bar) half-life ensures hypotheses persist through typical song sections
- Prevents premature eviction during breakdowns or quiet sections

---

## 3. Integration with AudioController

### 3.1 Current AudioController Flow

```
update(dt):
  1. Update microphone (AdaptiveMic)
  2. Feed samples to ensemble detector
  3. Run ensemble detector → transients
  4. Compute multi-band RMS energy → OSS buffer
  5. Run autocorrelation every 500ms → single BPM estimate
  6. Update single phase tracker
  7. Synthesize outputs (energy, pulse, phase, rhythmStrength)
```

### 3.2 Proposed Multi-Hypothesis Flow

```
update(dt):
  1. Update microphone (AdaptiveMic)
  2. Feed samples to ensemble detector
  3. Run ensemble detector → transients
  4. Compute multi-band RMS energy → OSS buffer
  5. Run autocorrelation every 500ms → extract ALL peaks (not just max)
  6. For each significant peak:
       - Check if matches existing hypothesis → update it
       - Else create new hypothesis (evicting LRU if needed)
  7. Update all active hypotheses:
       - Advance phase
       - Update confidence
       - Apply phrase-aware decay
  8. Promote best hypothesis to PRIMARY if needed
  9. Use PRIMARY hypothesis for outputs
```

### 3.3 Modified runAutocorrelation()

**Current implementation** (AudioController.cpp:177-347):
- Finds single best lag with maximum correlation
- Updates single bpm_, phase_, periodicityStrength_

**Proposed implementation**:
- Find top 3-4 peaks in autocorrelation function
- For each peak:
  - Check if it's within tolerance of an existing hypothesis
  - If yes: update that hypothesis with new evidence
  - If no: create new hypothesis (evicting LRU if needed)
- Update all hypotheses' strength values based on correlation peaks

```cpp
void AudioController::runAutocorrelation(uint32_t nowMs) {
    // ... existing setup code ...

    // Find ALL significant peaks (not just maximum)
    struct Peak {
        int lag;
        float correlation;
        float normCorrelation;
    };
    Peak peaks[4];  // Track up to 4 peaks
    int numPeaks = 0;

    // First pass: find all local maxima above threshold
    for (int lag = minLag; lag <= maxLag; lag++) {
        // ... compute correlation ...

        // Check if local maximum (using strict inequality to handle plateaus)
        // On a plateau, only the center sample will be considered a peak
        bool isLocalMax = true;
        bool hasStrictlyLowerLeft = false;
        bool hasStrictlyLowerRight = false;

        for (int delta = -2; delta <= 2; delta++) {
            int neighborLag = lag + delta;
            if (neighborLag >= minLag && neighborLag <= maxLag && neighborLag != lag) {
                if (correlationAtLag[neighborLag] > correlation) {
                    // Neighbor is strictly greater - not a local max
                    isLocalMax = false;
                    break;
                }
                // Track if we have strictly lower neighbors on both sides
                if (delta < 0 && correlationAtLag[neighborLag] < correlation) {
                    hasStrictlyLowerLeft = true;
                }
                if (delta > 0 && correlationAtLag[neighborLag] < correlation) {
                    hasStrictlyLowerRight = true;
                }
            }
        }

        // Accept as peak if: no higher neighbors AND at least one strictly lower neighbor
        // This handles plateaus by preferring the first rising edge
        if (isLocalMax && (hasStrictlyLowerLeft || hasStrictlyLowerRight) &&
            normCorrelation > 0.3f && numPeaks < 4) {
            peaks[numPeaks++] = {lag, correlation, normCorrelation};
        }
    }

    // Sort peaks by strength (descending)
    // ... simple bubble sort ...

    // Update or create hypotheses for each peak
    multiHypothesis_.updateWithPeaks(peaks, numPeaks, nowMs);
}
```

### 3.4 Modified updatePhase()

**Current implementation**:
- Updates single phase_ and periodicityStrength_

**Proposed implementation**:
- Update phase for ALL active hypotheses
- Use primary hypothesis phase for output

```cpp
void AudioController::updatePhase(float dt, uint32_t nowMs) {
    // Update all active hypotheses
    for (int i = 0; i < MultiHypothesisTracker::MAX_HYPOTHESES; i++) {
        if (multiHypothesis_.hypotheses[i].active) {
            multiHypothesis_.updateHypothesisPhase(i, dt, nowMs);
        }
    }

    // Promote best hypothesis if needed
    multiHypothesis_.promoteBestHypothesis();

    // Use primary hypothesis for output
    TempoHypothesis& primary = multiHypothesis_.getPrimary();
    phase_ = primary.phase;
    bpm_ = primary.bpm;
    periodicityStrength_ = primary.strength;
}
```

---

## 4. Implementation Roadmap

### Phase 1: Data Structures and Basic Framework (1-2 hours coding)

**Tasks**:
1. Create `TempoHypothesis` struct in AudioController.h
2. Create `MultiHypothesisTracker` class skeleton
3. Add `MultiHypothesisTracker multiHypothesis_;` member to AudioController
4. Implement basic methods:
   - `createHypothesis(bpm, strength, nowMs)`
   - `findLRUSlot()`
   - `getPrimary()`
   - `getHypothesis(index)`

**Validation**:
- Compiles successfully
- Memory usage check: should add ~224 bytes
- Can create and retrieve hypotheses

### Phase 2: Autocorrelation Peak Extraction (2-3 hours coding)

**Tasks**:
1. Modify `runAutocorrelation()` to find multiple peaks
2. Implement peak sorting and filtering
3. Implement `updateWithPeaks()` method:
   - Match peaks to existing hypotheses (within ±5% BPM tolerance)
   - Create new hypotheses for unmatched peaks
   - Update matched hypotheses' strength values
4. Add debug output to SerialConsole for hypothesis tracking

**Validation**:
- Serial output shows 2-4 hypotheses being tracked
- BPM values match expected peaks (60, 120, 180 for half/full/double time)
- Hypothesis creation and eviction working correctly

### Phase 3: Phase Tracking and Promotion (2-3 hours coding)

**Tasks**:
1. Implement `updateHypothesisPhase(index, dt, nowMs)`:
   - Advance phase based on tempo
   - Compute phase consistency (compare to autocorrelation-derived phase)
   - Update confidence
2. Implement `promoteBestHypothesis()`:
   - Compare all active hypotheses' confidence
   - Promote if non-primary exceeds primary + threshold (e.g., 0.15)
   - Swap priority slots
3. Use primary hypothesis for output (phase_, bpm_, periodicityStrength_)

**Validation**:
- Phase advances correctly for all hypotheses
- Promotion occurs when appropriate
- Output uses primary hypothesis values

### Phase 4: Phrase-Aware Decay (1-2 hours coding)

**Tasks**:
1. Add `beatsSinceUpdate` float member to TempoHypothesis
2. Implement phrase-aware decay in `updateHypothesisPhase()`:
   - Accumulate fractional beats
   - Apply exp(-0.02166 * beats) decay factor
   - Deactivate if strength < 0.1
3. Remove old time-based decay (5-second half-life)

**Validation**:
- Hypotheses persist through 8-bar phrases
- Weak hypotheses deactivate after ~32 beats without evidence
- Memory of previous tempo for quick re-acquisition

### Phase 5: Testing and Tuning (3-4 hours)

**Tasks**:
1. Test with simple-beat pattern (120 BPM, steady)
   - Should maintain single primary hypothesis
   - Secondary/tertiary may track half/double time
2. Test with tempo-change pattern (120→140 BPM transition)
   - Should create new hypothesis for 140 BPM
   - Should promote 140 BPM to primary after ~4 bars of evidence
3. Test with breakdown pattern (music stops, then resumes)
   - Should maintain hypotheses during silence (phrase-aware decay)
   - Should quickly re-acquire tempo when music resumes
4. Tune parameters:
   - Peak detection threshold (currently 0.3)
   - Promotion threshold (currently 0.15)
   - Phrase-aware decay half-life (currently 32 beats)
   - BPM matching tolerance (currently ±5%)

**Validation**:
- F1 score improves for tempo-change patterns
- No regressions on simple steady-tempo patterns
- Visual inspection via serial debug output

### Phase 6: Documentation and SerialConsole Integration (1-2 hours)

**Tasks**:
1. Add serial commands for hypothesis inspection:
   - `show hypotheses` - print all active hypotheses
   - `show primary` - print primary hypothesis details
2. Update AUDIO-TUNING-GUIDE.md with multi-hypothesis tuning section
3. Update IMPROVEMENT_PLAN.md with completed status
4. Add inline comments explaining key design decisions

**Validation**:
- Serial commands work correctly
- Documentation is clear and accurate

---

## 5. Tuning Parameters

```cpp
class MultiHypothesisTracker {
public:
    // Peak detection
    float minPeakStrength = 0.3f;       // Minimum normalized correlation to create hypothesis
    float minPeakSeparation = 0.1f;     // Minimum correlation difference between adjacent peaks

    // Hypothesis matching
    float bpmMatchTolerance = 0.05f;    // ±5% BPM tolerance for matching peaks to hypotheses

    // Promotion
    float promotionThreshold = 0.15f;   // Confidence advantage needed to promote (0-1)
    uint16_t minBeatsBeforePromotion = 8;  // Minimum beats before promoting a new hypothesis

    // Decay
    float phraseHalfLifeBeats = 32.0f;  // Half-life in beats (8 bars of 4/4)
    float minStrengthToKeep = 0.1f;     // Deactivate hypotheses below this strength

    // Confidence weighting
    float strengthWeight = 0.5f;        // Weight of periodicity strength in confidence
    float consistencyWeight = 0.3f;     // Weight of phase consistency in confidence
    float longevityWeight = 0.2f;       // Weight of beat count in confidence
};
```

---

## 6. Testing Strategy

### 6.1 Unit Tests (via blinky-test-player)

1. **Test: Single steady tempo (120 BPM)**
   - Expected: Single primary hypothesis at 120 BPM
   - Metric: F1 score > 0.85, primary hypothesis stable

2. **Test: Tempo doubling (60 BPM → 120 BPM)**
   - Expected: New hypothesis created at 120 BPM, promoted after 8 bars
   - Metric: Transition latency < 4 bars, F1 score > 0.80

3. **Test: Tempo ambiguity (120 BPM with strong half-time feel)**
   - Expected: Primary at 120 BPM, secondary at 60 BPM
   - Metric: Primary remains stable, no spurious promotions

4. **Test: Breakdown and resume**
   - Expected: Hypotheses persist during silence, quick re-acquisition
   - Metric: Re-lock latency < 2 bars after music resumes

5. **Test: Gradual tempo drift (120 BPM → 125 BPM over 32 bars)**
   - Expected: Primary hypothesis BPM updates gradually
   - Metric: Continuous tracking, no loss of lock

### 6.2 Integration Tests (live testing)

1. Test with DJ mix (multiple tempo changes)
2. Test with live drums (variable tempo)
3. Test with polyrhythmic music (complex patterns)

### 6.3 Success Criteria

- No regressions on existing test patterns (simple-beat, simultaneous, etc.)
- Improved F1 scores for tempo-change patterns (target: >0.80)
- Stable tracking through 8+ bar phrases
- Quick re-acquisition after breakdowns (<2 bars)

---

## 7. Future Enhancements

### 7.1 Meter Detection
- Track time signature (3/4, 4/4, 6/8, 7/8) for each hypothesis
- Use beat strength patterns to infer meter
- Improves phase locking for non-4/4 music

### 7.2 Downbeat Detection
- Identify measure boundaries (bar lines)
- Use spectral/harmonic changes to detect downbeats
- Enables bar-level synchronization for generators

### 7.3 Genre Classification
- Classify music genre based on tempo distribution and spectral features
- Adjust detector weights and fusion parameters per genre
- Improves accuracy for genre-specific patterns (EDM vs. jazz vs. rock)

### 7.4 Particle Filter Implementation
- Replace fixed-slot hypotheses with particle filter
- More flexible, handles arbitrary number of hypotheses
- Higher memory cost (~1-2 KB)

---

## 8. Risk Assessment

### 8.1 Performance Risks

**Risk**: Multi-hypothesis tracking increases CPU usage beyond budget
**Mitigation**:
- Profile each new function
- Optimize autocorrelation peak extraction (early exit if max peaks found)
- Limit to 4 hypotheses (not 8 or 16)

**Risk**: Memory usage exceeds available RAM
**Mitigation**:
- Pre-calculated memory budget: 224 bytes
- Monitor global variable usage during testing
- Consider reducing OSS buffer if needed (unlikely)

### 8.2 Accuracy Risks

**Risk**: Too many hypotheses cause spurious tempo switches
**Mitigation**:
- Use promotion threshold (0.15 confidence advantage)
- Require minimum beat count before promotion (8 beats)
- Test with diverse music styles

**Risk**: Phrase-aware decay too aggressive, loses tempo during breakdowns
**Mitigation**:
- Tune half-life parameter (start with 32 beats)
- Add silence detection: pause beat counting during silence

### 8.3 Integration Risks

**Risk**: Breaking existing rhythm tracking functionality
**Mitigation**:
- Develop in feature branch
- Run full test suite after each phase
- Keep single-hypothesis fallback mode for debugging

---

## 9. Open Questions

1. **Peak detection**: Should we use adaptive threshold based on correlation variance?
2. **Confidence metric**: Should we weight recent beats more heavily than old beats?
3. **Silence handling**: Should we freeze beat counting during silence (zero OSS energy)?
4. **Initial hypothesis**: Should we seed with common tempos (120, 128, 140 BPM)?
5. **Debug output**: How verbose should serial debug be? (impacts performance)

---

## 10. References

1. **Ellis, D. P. W. (2007)**. "Beat Tracking by Dynamic Programming."
   Journal of New Music Research, 36(1), 51-60.

2. **Davies, M. E., & Plumbley, M. D. (2007)**. "Context-Dependent Beat Tracking of Musical Audio."
   IEEE Transactions on Audio, Speech, and Language Processing, 15(3), 1009-1020.

3. **Böck, S., Krebs, F., & Widmer, G. (2016)**. "Joint Beat and Downbeat Tracking with Recurrent Neural Networks."
   Proceedings of ISMIR 2016.

4. **Oliveira, J. L., et al. (2017)**. "OBTAIN: Online Beat Tracking with Adaptive Instability Normalization."
   Proceedings of the AES International Conference on Semantic Audio.

5. **Scheirer, E. D. (1998)**. "Tempo and Beat Analysis of Acoustic Musical Signals."
   Journal of the Acoustical Society of America, 103(1), 588-601.

---

## Appendix A: Code Snippets

### A.1 TempoHypothesis Full Implementation

```cpp
struct TempoHypothesis {
    // Tempo estimate
    float bpm;
    float beatPeriodMs;

    // Phase tracking
    float phase;

    // Confidence and evidence
    float strength;
    float confidence;

    // Timing and history
    uint32_t lastUpdateMs;
    uint16_t beatCount;
    uint32_t createdMs;
    float beatsSinceUpdate;

    // Autocorrelation evidence
    float correlationPeak;
    int lagSamples;

    // State
    bool active;
    uint8_t priority;

    TempoHypothesis()
        : bpm(120.0f)
        , beatPeriodMs(500.0f)
        , phase(0.0f)
        , strength(0.0f)
        , confidence(0.0f)
        , lastUpdateMs(0)
        , beatCount(0)
        , createdMs(0)
        , beatsSinceUpdate(0.0f)
        , correlationPeak(0.0f)
        , lagSamples(0)
        , active(false)
        , priority(3)
    {}

    void updatePhase(float dt) {
        float phaseIncrement = dt * 1000.0f / beatPeriodMs;
        phase += phaseIncrement;

        // Wrap phase to [0, 1)
        phase = fmodf(phase, 1.0f);
        if (phase < 0.0f) phase += 1.0f;

        // Accumulate beats
        beatsSinceUpdate += phaseIncrement;
        beatCount = static_cast<uint16_t>(beatCount + phaseIncrement);
    }

    void applyDecay() {
        // Phrase-aware decay: half-life = 32 beats
        float decayFactor = expf(-0.02166f * beatsSinceUpdate);
        strength *= decayFactor;
        beatsSinceUpdate = 0.0f;  // Reset accumulator

        // Deactivate if too weak
        if (strength < 0.1f) {
            active = false;
        }
    }

    float computeConfidence(float strengthWeight, float consistencyWeight, float longevityWeight) const {
        // Normalize beat count to [0, 1] over 32 beats
        float normalizedLongevity = (beatCount > 32) ? 1.0f : (float)beatCount / 32.0f;

        // Phase consistency (placeholder - would use actual phase error)
        float phaseConsistency = 0.8f;  // TODO: compute from phase error

        // Weighted combination
        return strengthWeight * strength +
               consistencyWeight * phaseConsistency +
               longevityWeight * normalizedLongevity;
    }
};
```

### A.2 MultiHypothesisTracker Skeleton

```cpp
class MultiHypothesisTracker {
public:
    static constexpr int MAX_HYPOTHESES = 4;

    TempoHypothesis hypotheses[MAX_HYPOTHESES];
    uint8_t activeHypothesis = 0;

    // Tuning parameters
    float minPeakStrength = 0.3f;
    float bpmMatchTolerance = 0.05f;
    float promotionThreshold = 0.15f;

    // Create a new hypothesis in the best available slot
    int createHypothesis(float bpm, float strength, uint32_t nowMs) {
        int slot = findBestSlot();
        if (slot < 0) return -1;  // No slot available

        TempoHypothesis& hypo = hypotheses[slot];
        hypo.bpm = bpm;
        hypo.beatPeriodMs = 60000.0f / bpm;
        hypo.phase = 0.0f;
        hypo.strength = strength;
        hypo.lastUpdateMs = nowMs;
        hypo.createdMs = nowMs;
        hypo.beatCount = 0;
        hypo.beatsSinceUpdate = 0.0f;
        hypo.active = true;
        hypo.priority = slot;

        return slot;
    }

    // Find best slot for new hypothesis (LRU eviction)
    int findBestSlot() {
        // First, check for inactive slots
        for (int i = 0; i < MAX_HYPOTHESES; i++) {
            if (!hypotheses[i].active) {
                return i;
            }
        }

        // All slots active - find LRU
        int oldestSlot = 0;
        uint32_t oldestTime = hypotheses[0].lastUpdateMs;

        for (int i = 1; i < MAX_HYPOTHESES; i++) {
            if (hypotheses[i].lastUpdateMs < oldestTime) {
                oldestTime = hypotheses[i].lastUpdateMs;
                oldestSlot = i;
            }
        }

        // Don't evict primary if it's still strong
        if (oldestSlot == 0 && hypotheses[0].strength > 0.5f) {
            // Evict tertiary instead
            return 2;
        }

        return oldestSlot;
    }

    // Get primary hypothesis
    TempoHypothesis& getPrimary() {
        return hypotheses[0];
    }

    // Update all hypotheses with autocorrelation peaks
    void updateWithPeaks(const Peak* peaks, int numPeaks, uint32_t nowMs, float samplesPerMs) {
        // For each peak, find matching hypothesis or create new one
        for (int i = 0; i < numPeaks; i++) {
            float bpm = 60000.0f / (peaks[i].lag / samplesPerMs);

            // Find matching hypothesis
            int matchSlot = findMatchingHypothesis(bpm);

            if (matchSlot >= 0) {
                // Update existing hypothesis
                hypotheses[matchSlot].strength = peaks[i].normCorrelation;
                hypotheses[matchSlot].lastUpdateMs = nowMs;
                hypotheses[matchSlot].correlationPeak = peaks[i].correlation;
                hypotheses[matchSlot].lagSamples = peaks[i].lag;
            } else if (peaks[i].normCorrelation > minPeakStrength) {
                // Create new hypothesis
                createHypothesis(bpm, peaks[i].normCorrelation, nowMs);
            }
        }
    }

    // Find hypothesis matching a given BPM (within tolerance)
    int findMatchingHypothesis(float bpm) {
        for (int i = 0; i < MAX_HYPOTHESES; i++) {
            if (!hypotheses[i].active) continue;

            float error = fabsf(bpm - hypotheses[i].bpm) / hypotheses[i].bpm;
            if (error < bpmMatchTolerance) {
                return i;
            }
        }
        return -1;  // No match
    }

    // Promote best non-primary hypothesis if it's significantly better
    void promoteBestHypothesis() {
        int bestSlot = 0;
        float bestConfidence = hypotheses[0].confidence;

        for (int i = 1; i < MAX_HYPOTHESES; i++) {
            if (!hypotheses[i].active) continue;

            if (hypotheses[i].confidence > bestConfidence) {
                bestConfidence = hypotheses[i].confidence;
                bestSlot = i;
            }
        }

        // Promote if significantly better and has enough history
        if (bestSlot != 0 &&
            bestConfidence > hypotheses[0].confidence + promotionThreshold &&
            hypotheses[bestSlot].beatCount >= 8) {

            // Swap priority
            uint8_t oldPriority = hypotheses[0].priority;
            hypotheses[0].priority = hypotheses[bestSlot].priority;
            hypotheses[bestSlot].priority = oldPriority;

            // Swap slots
            TempoHypothesis temp = hypotheses[0];
            hypotheses[0] = hypotheses[bestSlot];
            hypotheses[bestSlot] = temp;
        }
    }
};
```

---

## End of Document

**Next Steps**:
1. Review this plan with user
2. Clarify any open questions
3. Begin Phase 1 implementation
4. Iterate based on testing results
# Multi-Hypothesis Tracking - Open Questions Answered

**Date**: 2026-01-03
**Status**: Design Decisions
**Related**: MULTI_HYPOTHESIS_TRACKING_PLAN.md

---

## Question 1: Peak Detection Threshold

**Should we use adaptive threshold based on correlation variance?**

### Current System
- Fixed threshold: `normCorrelation > 0.3` to create hypothesis (plan line 296)
- Fixed activation threshold: `periodicityStrength > 0.25` to update tempo (AudioController.cpp:328)
- Works well for tested music patterns

### Analysis

**Option A: Fixed Threshold (0.3)**
- Pros:
  - Simple, predictable behavior
  - No additional CPU overhead
  - Current system uses fixed thresholds successfully
- Cons:
  - May miss weak but valid tempos in quiet music
  - May create spurious hypotheses in noisy/arrhythmic music

**Option B: Adaptive Threshold (mean + k*stddev)**
- Pros:
  - Adapts to signal characteristics (loud vs quiet, clean vs noisy)
  - More robust across diverse music genres
- Cons:
  - Requires computing variance over autocorrelation function (+15-20% CPU in autocorr step)
  - More parameters to tune (k multiplier)
  - Risk of instability (threshold changes frame-to-frame)

### Decision: **Fixed Threshold with Signal-Normalized Correlation**

**Recommendation**: Keep fixed threshold (0.3) but ensure correlation is properly normalized.

**Rationale**:
1. Current normalization is robust: `normCorrelation = maxCorrelation / (avgEnergy + 0.001f)` (AudioController.cpp:304)
2. This already adapts to signal strength (quiet music with weak correlation normalized by weak signal energy)
3. No additional CPU overhead
4. Proven to work in current single-hypothesis system

**Parameters**:
```cpp
// MultiHypothesisTracker tuning parameters
float minPeakStrength = 0.3f;           // Normalized correlation threshold
float minRelativePeakHeight = 0.7f;    // Peak must be >70% of max peak (reject weak secondaries)
```

**Future Enhancement**: If testing reveals issues with quiet music or noisy environments, revisit adaptive threshold in Phase 5.

---

## Question 2: Confidence Metric

**Should we weight recent beats more heavily than old beats?**

### Current System
- Phase tracking uses exponential smoothing: `phase_ += phaseDiff * phaseAdaptRate * dt * 10.0f` (AudioController.cpp:382)
- BPM smoothing: `bpm_ = bpm_ * 0.8f + newBpm * 0.2f` (AudioController.cpp:333)
- This implicitly weights recent data more heavily

### Analysis

**Option A: Uniform Weighting (all beats equal)**
- Pros: Simple, stable, captures long-term tempo
- Cons: Slow to detect tempo changes, sensitive to old data

**Option B: Exponential Decay (recent beats weighted higher)**
- Pros: Faster adaptation, matches current smoothing approach
- Cons: May be too reactive to transient tempo fluctuations

**Option C: Windowed Average (last N beats only)**
- Pros: Clean semantics, predictable behavior
- Cons: Discontinuity when old beat drops out of window

### Decision: **Exponential Decay with Separate Weights for Strength vs Longevity**

**Recommendation**: Use exponential decay for strength updates, uniform counting for longevity.

**Rationale**:
1. **Strength** (periodicity evidence) should favor recent autocorrelation peaks → exponential decay
2. **Longevity** (beat count) should count all beats equally → simple counter
3. **Phase consistency** should use recent phase errors → exponential smoothing

This matches the current system's architecture and provides both stability (longevity) and adaptability (strength).

**Implementation**:
```cpp
// In TempoHypothesis::computeConfidence()
float computeConfidence(float strengthWeight, float consistencyWeight, float longevityWeight) const {
    // Strength: already uses exponential-weighted autocorrelation updates
    float normalizedStrength = strength;

    // Longevity: uniform count, normalized over 32 beats (8 bars)
    float normalizedLongevity = (beatCount > 32) ? 1.0f : (float)beatCount / 32.0f;

    // Phase consistency: exponentially-weighted phase error (computed in updatePhase)
    // TODO: Track running average of phase error with decay factor
    float phaseConsistency = 1.0f - clampf(avgPhaseError_, 0.0f, 1.0f);

    return strengthWeight * normalizedStrength +
           consistencyWeight * phaseConsistency +
           longevityWeight * normalizedLongevity;
}
```

**Default Weights** (from plan):
- `strengthWeight = 0.5` (recent autocorrelation evidence)
- `consistencyWeight = 0.3` (recent phase tracking accuracy)
- `longevityWeight = 0.2` (historical stability)

---

## Question 3: Silence Handling

**Should we freeze beat counting during silence (zero OSS energy)?**

### Current System
- Tracks `lastSignificantAudioMs` when `onsetStrength > 0.1 || level > 0.1` (AudioController.cpp:137-139)
- Applies time-based decay after 3s silence: `periodicityStrength *= exp(-0.138629 * dt)` (AudioController.cpp:399)
- 3-second grace period before decay starts (AudioController.cpp:398)
- 5-second half-life after grace period

### Analysis

The current system already handles silence well, but multi-hypothesis tracking needs to decide:

**Option A: Freeze All Updates (stop phase, stop counting, no decay)**
- Pros: Preserves exact tempo/phase during breakdowns
- Cons: May lose sync if silence duration unknown (phase drift)

**Option B: Continue Phase, Freeze Beat Count, Apply Time Decay**
- Pros: Maintains predictions, natural decay of confidence
- Cons: Phase may drift during long silence

**Option C: Freeze Beat Count, Continue Phase, Apply Beat-Count Decay Only**
- Pros: Prevents decay during musical breakdowns (which are intentional)
- Cons: Hypotheses persist indefinitely during non-musical silence

### Decision: **Hybrid - Freeze Beat Counting, Continue Phase, Dual Decay**

**Recommendation**: Combine beat-count decay (phrase-aware) with time-based decay (long silence).

**Rationale**:
1. **Musical breakdowns** (4-8 bars) are common and intentional → freeze beat counting preserves hypotheses
2. **Long silence** (15+ seconds) indicates end of music → time-based decay clears hypotheses
3. **Phase prediction continues** during silence → enables instant sync when music resumes

**Implementation**:
```cpp
void MultiHypothesisTracker::updateHypothesis(int index, float dt, uint32_t nowMs, bool hasSignificantAudio) {
    TempoHypothesis& hypo = hypotheses[index];

    // Always advance phase (for prediction)
    hypo.updatePhase(dt);

    if (hasSignificantAudio) {
        // Active music: apply beat-count decay
        hypo.beatsSinceUpdate += dt * 1000.0f / hypo.beatPeriodMs;

        // Phrase-aware decay: half-life = 32 beats
        if (hypo.beatsSinceUpdate > 1.0f) {
            float decayFactor = expf(-0.02166f * hypo.beatsSinceUpdate);
            hypo.strength *= decayFactor;
            hypo.beatsSinceUpdate = 0.0f;
        }
    } else {
        // Silence: apply time-based decay after grace period
        int32_t silenceMs = (int32_t)(nowMs - hypo.lastUpdateMs);
        if (silenceMs > 3000) {
            // 5-second half-life decay: exp(-ln(2) * dt / 5.0)
            float decayFactor = expf(-0.138629f * dt);
            hypo.strength *= decayFactor;
        }
        // Note: beat counting frozen during silence (beatsSinceUpdate not incremented)
    }

    // Deactivate if too weak
    if (hypo.strength < 0.1f) {
        hypo.active = false;
    }
}
```

**Thresholds**:
```cpp
// Silence detection
float minOnsetStrength = 0.1f;    // OSS threshold for "significant audio"
float minLevel = 0.1f;            // Level threshold for "significant audio"
uint32_t silenceGracePeriod = 3000;  // 3 seconds before time-based decay starts
float silenceDecayHalfLife = 5.0f;   // 5 seconds (matches current system)
```

---

## Question 4: Initial Hypothesis Seeding

**Should we seed with common tempos (120, 128, 140 BPM)?**

### Current System
- Starts with `bpm_ = 120.0f` (AudioController.cpp:43)
- Waits for first autocorrelation result to detect actual tempo
- Requires 3 seconds of data minimum (AudioController.cpp:215)

### Analysis

**Option A: No Seeding (wait for autocorrelation)**
- Pros: Only creates hypotheses based on real data
- Cons: 3+ second startup latency

**Option B: Seed Common Tempos (120, 128, 140 BPM)**
- Pros: Instant response if music matches seed
- Cons: Spurious hypotheses clutter tracker, need eventual eviction

**Option C: Seed Single Default (120 BPM)**
- Pros: Provides baseline prediction during startup
- Cons: Wrong for music outside 100-140 BPM range

### Decision: **No Pre-Seeding - Data-Driven Hypothesis Creation**

**Recommendation**: Wait for autocorrelation data before creating hypotheses.

**Rationale**:
1. **3-second startup latency is acceptable** for a lighting effect (not a real-time DJ tool)
2. **Spurious hypotheses waste slots**: Seeded tempos may never match actual music
3. **Current system already waits 3s** (AudioController.cpp:215) - no regression
4. **LRU eviction would quickly remove seeds anyway** once real data arrives

**Startup Behavior**:
```cpp
// AudioController::begin() - NO seeding
bpm_ = 120.0f;              // Default fallback (not a hypothesis)
periodicityStrength_ = 0.0f; // No rhythm detected yet
// multiHypothesis_ starts with all slots inactive

// After 3 seconds: runAutocorrelation() creates first hypothesis from data
```

**Future Enhancement**: If user testing reveals desire for instant response, add optional seeding via configuration:
```cpp
bool seedCommonTempos = false;  // Default: off
float seedTempos[] = {120.0f, 128.0f, 140.0f};  // Common EDM/pop tempos
```

---

## Question 5: Debug Output Verbosity

**How verbose should serial debug be? (impacts performance)**

### Current System
- Uses `SerialConsole::getGlobalLogLevel()` to gate debug output (AudioController.cpp:259)
- Rate-limited to every 2 seconds (AudioController.cpp:260)
- Two debug messages: `RHYTHM_DEBUG` (signal stats) and `RHYTHM_DEBUG2` (correlation results)
- Total: ~120 bytes every 2s when DEBUG enabled

### Analysis

Multi-hypothesis tracking will generate significantly more debug data:
- 4 hypotheses × (BPM, phase, strength, confidence, beatCount) = ~20 values
- Hypothesis creation/eviction events
- Promotion events

**Estimated Debug Volume**:
- Per-frame updates: 4 hypotheses × 50 bytes = 200 bytes @ 60 Hz = **12 KB/s**
- Rate-limited (2s): 200 bytes every 2s = **100 bytes/s** (acceptable)

### Decision: **Tiered Debug Levels with Rate Limiting**

**Recommendation**: Use existing log level system with additional gates for verbose output.

**Implementation**:
```cpp
enum class HypothesisDebugLevel {
    OFF = 0,        // No debug output
    EVENTS = 1,     // Hypothesis creation, promotion, eviction only
    SUMMARY = 2,    // Primary hypothesis status every 2s
    DETAILED = 3    // All hypotheses every 2s
};

// In AudioController.h
HypothesisDebugLevel hypothesisDebugLevel = HypothesisDebugLevel::SUMMARY;
```

**Output Examples**:

**EVENTS** (instant, when events occur):
```json
{"type":"HYPO_CREATE","slot":1,"bpm":140.2,"strength":0.67}
{"type":"HYPO_PROMOTE","from":2,"to":0,"bpm":140.2,"conf":0.82}
{"type":"HYPO_EVICT","slot":3,"bpm":128.1,"age_ms":8234}
```

**SUMMARY** (every 2s):
```json
{"type":"HYPO_PRIMARY","bpm":140.2,"phase":0.34,"strength":0.82,"beatCount":47}
```

**DETAILED** (every 2s):
```json
{"type":"HYPO_ALL","hypotheses":[
  {"slot":0,"pri":"PRIMARY","bpm":140.2,"phase":0.34,"str":0.82,"conf":0.85,"beats":47},
  {"slot":1,"pri":"SECONDARY","bpm":70.1,"phase":0.68,"str":0.45,"conf":0.52,"beats":23},
  {"slot":2,"pri":"INACTIVE"},
  {"slot":3,"pri":"INACTIVE"}
]}
```

**Control via SerialConsole**:
```cpp
// New commands
show hypotheses        // Print current state (same as DETAILED)
set hypodebug 0        // OFF
set hypodebug 1        // EVENTS
set hypodebug 2        // SUMMARY (default)
set hypodebug 3        // DETAILED
```

**Performance Impact**:
- EVENTS: ~50 bytes per event, negligible (events are rare)
- SUMMARY: ~80 bytes every 2s = negligible
- DETAILED: ~200 bytes every 2s = negligible
- All levels: <1% CPU impact due to rate limiting

---

## Summary of Decisions

| Question | Decision | Rationale |
|----------|----------|-----------|
| **1. Peak Threshold** | Fixed (0.3), signal-normalized | Current normalization is robust, no CPU overhead |
| **2. Confidence Weighting** | Exponential (strength/consistency), uniform (longevity) | Balances adaptability and stability |
| **3. Silence Handling** | Freeze beat count, dual decay (phrase + time) | Preserves hypotheses during breakdowns, clears after long silence |
| **4. Initial Seeding** | No pre-seeding, data-driven only | Avoid spurious hypotheses, 3s startup acceptable |
| **5. Debug Verbosity** | Tiered levels with rate limiting | Flexible debugging without performance impact |

---

## Implementation Checklist

### Phase 1: Data Structures
- [x] Question 2 resolved → confidence formula defined
- [x] Question 3 resolved → silence handling defined
- [ ] Add `avgPhaseError_` member to TempoHypothesis (for consistency metric)
- [ ] Add `HypothesisDebugLevel` enum to AudioController

### Phase 2: Autocorrelation
- [x] Question 1 resolved → fixed threshold 0.3
- [ ] Implement multi-peak extraction with relative height filter (0.7)
- [ ] Add debug output at EVENTS level (hypothesis creation)

### Phase 3: Phase Tracking
- [x] Question 2 resolved → use exponential-weighted phase error
- [x] Question 3 resolved → continue phase during silence
- [ ] Implement phase consistency tracking in updateHypothesisPhase()
- [ ] Add debug output at SUMMARY level (primary hypothesis)

### Phase 4: Decay
- [x] Question 3 resolved → dual decay strategy
- [ ] Implement beat-count decay (active music)
- [ ] Implement time-based decay (silence)
- [ ] Add silence detection to update() flow

### Phase 5: Testing
- [x] Question 4 resolved → no pre-seeding needed
- [ ] Test startup latency (should be 3s as current)
- [ ] Test breakdown patterns (hypotheses should persist 8+ bars)
- [ ] Test long silence (hypotheses should decay after 8s total)

### Phase 6: Documentation
- [x] Question 5 resolved → tiered debug levels
- [ ] Document serial commands (show hypotheses, set hypodebug)
- [ ] Update AUDIO-TUNING-GUIDE.md with multi-hypothesis tuning section
- [ ] Archive this document (design decisions captured)

---

## Next Steps

1. **Review these decisions** with user/team
2. **Update MULTI_HYPOTHESIS_TRACKING_PLAN.md** with answers (or reference this doc)
3. **Begin Phase 1 implementation** with clarified design
4. **Profile early** (Phase 2) to validate CPU budget

---

## References

- MULTI_HYPOTHESIS_TRACKING_PLAN.md (original open questions)
- AudioController.cpp:137-401 (current silence handling and phase tracking)
- AUDIO_ARCHITECTURE.md (architecture overview)
