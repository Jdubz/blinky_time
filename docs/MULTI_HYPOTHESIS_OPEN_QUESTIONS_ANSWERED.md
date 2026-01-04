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
- MUSIC_MODE_SIMPLIFIED.md (architecture overview)
