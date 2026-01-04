# Multi-Hypothesis Tempo Tracking - Implementation Plan

**Author**: Claude Code
**Date**: 2026-01-03
**Status**: Planning
**Related**: MUSIC_MODE_SIMPLIFIED.md, IMPROVEMENT_PLAN.md

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

        // Check if local maximum
        bool isLocalMax = true;
        for (int delta = -2; delta <= 2; delta++) {
            int neighborLag = lag + delta;
            if (neighborLag >= minLag && neighborLag <= maxLag && neighborLag != lag) {
                if (correlationAtLag[neighborLag] >= correlation) {
                    isLocalMax = false;
                    break;
                }
            }
        }

        if (isLocalMax && normCorrelation > 0.3f && numPeaks < 4) {
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
