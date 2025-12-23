# Audio System Improvements Plan

## Executive Summary

This document outlines critical improvements needed for the AdaptiveMic audio processing system. The current implementation contains fundamental misconceptions about transient detection, AGC time constants, and musical responsiveness that prevent it from functioning as an effective music visualizer.

**Date**: 2025-12-23
**Status**: Planning Phase
**Priority**: CRITICAL - Core functionality is broken

---

## Critical Issues Identified

### 1. Transient Detection is Wrongly Implemented as an Envelope ❌ CRITICAL

**Location**: `blinky-things/inputs/AdaptiveMic.cpp:107-127`

**Current Implementation**:
```cpp
transient = 1.0f;  // Set to 1.0
transient -= transientDecay * dt;  // Decay over time (8.0/sec)
```

**Problem**:
- Creates a **decaying envelope** that ramps down over ~125ms (1.0 / 8.0)
- This is a sustained signal, not a momentary trigger
- Fundamentally breaks the intended use case

**Required Behavior**:
- Should be a **single-frame impulse** (trigger/gate signal)
- Fires once when transient is detected, immediately returns to 0
- Visual effects then use this impulse to trigger their own envelopes

**Impact**: Without this fix, the system cannot properly respond to musical transients (kicks, snares, hits).

---

### 2. Software AGC Time Constants are Mismatched ⚠️ HIGH

**Location**: `blinky-things/inputs/AdaptiveMic.cpp:181-186`

**Current Implementation**:
```cpp
float err = agTarget - levelPreGate;
globalGain += agStrength * err * dt;  // agStrength = 0.25
```

**Problems**:
- Simple proportional control without clear time constant
- Adaptation speed varies wildly based on error magnitude
- No defined 5-10 second window for musical phrasing
- Uses instantaneous `levelPreGate` instead of tracked RMS

**Required Behavior**:
- Track RMS level over 5-10 second window using EMA
- Understand musical phrasing dynamics (verse/chorus transitions)
- Preserve dynamics while preventing clipping
- Clear, predictable time constants

**Impact**: AGC responds erratically, either too slow or too fast, doesn't preserve musical dynamics.

---

### 3. Hardware Gain Period is Too Short ⏱️ MEDIUM

**Location**: `blinky-things/inputs/AdaptiveMic.h:49`

**Current**:
```cpp
uint32_t hwCalibPeriodMs = 60000;  // 60 seconds
```

**Required**:
```cpp
uint32_t hwCalibPeriodMs = 180000;  // 3 minutes (180 seconds)
```

**Rationale**:
- Hardware gain should adapt to environmental changes (concert venue vs quiet room)
- Environmental levels change over minutes, not seconds
- Prevents fighting with software AGC

---

### 4. Onset Detection is Overly Simplistic

**Current Approach**:
- Only compares current level to slow baseline
- No frequency awareness (kicks vs snares vs hi-hats)
- No spectral flux or energy novelty function
- Fixed cooldown limits detection rate

**Limitations**:
- Cannot distinguish kick (60-130 Hz) from snare (300-750 Hz)
- Cooldown of 120ms limits to 8.3 hits/sec
- Misses fast patterns: 16th notes at 120 BPM = 8 notes/sec
- 32nd note hi-hats at 125 BPM = 16 notes/sec (requires 60ms spacing)

---

## Research Findings

### Modern Onset Detection Techniques

**Sources**:
- [Onset Detection - Audio Labs Erlangen](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S1_OnsetDetection.html)
- [musicinformationretrieval.com](https://musicinformationretrieval.com/onset_detection.html)
- [Essentia Tutorial](https://essentia.upf.edu/tutorial_rhythm_onsetdetection.html)

**Key Methods**:

1. **Energy Envelope First-Order Difference**
   - Compute energy envelope of signal
   - Take first-order difference (positive changes only)
   - Apply adaptive threshold
   - Peak picking with local maximum detection

2. **Spectral Flux**
   - Measure rate of power spectrum change between frames
   - More robust to noise than simple energy
   - Better for complex musical material

3. **Frequency-Specific Detection**
   - Bass (60-130 Hz): Kick drums
   - Low-mid (301-750 Hz): Snare drums
   - High (8-12 kHz): Hi-hats
   - Allows instrument-specific triggering

4. **Adaptive Thresholding**
   - Threshold based on local statistics
   - Prevents false triggers during loud passages
   - Adapts to changing dynamics

---

### AGC Time Constants - Professional Audio Standards

**Sources**:
- [Q-SYS AGC Documentation](https://q-syshelp.qsc.com/Content/Schematic_Library/leveler.htm)
- [How AGC Works - Wireless Pi](https://wirelesspi.com/how-automatic-gain-control-agc-works/)

**Standard Practice**:

1. **Attack Time**: 20-100ms for music
   - Fast response to signal increases
   - Prevents clipping on transients

2. **Release/Decay Time**: 200ms - 2 seconds
   - Slower response to decreases
   - Preserves musical phrasing
   - Prevents pumping/breathing artifacts

3. **Measurement Window**:
   - RMS detector over appropriate time scale
   - Envelope follower with ballistic characteristics
   - Logarithmic response for perceptual accuracy

4. **Adaptive Time Constants**:
   - Fast attack during transients
   - Slower release during sustained signals
   - Prevents modulation artifacts

**Time Constant Visualization**:
```
Environment (HW Gain):  |=================|  3 minutes
                        very slow adaptation to venue

Musical Phrasing (SW AGC): |======|  5-10 seconds
                        phrase-level dynamics

Transient Detection:    |  <60ms
                        instantaneous impulse
```

---

### Real-Time Beat Tracking

**Sources**:
- [OBTAIN: Real-Time Beat Tracking](https://arxiv.org/pdf/1704.02216)
- [BTrack GitHub](https://github.com/adamstark/BTrack)

**Key Concepts**:

1. **Onset Strength Envelope**
   - Detect onsets first
   - Track beat periodicity from onset pattern
   - Use autocorrelation or comb filtering

2. **Tempo Estimation**
   - Estimate BPM from onset spacing
   - Adapt to tempo changes
   - Handle accelerando/ritardando

3. **Phase Locking**
   - Lock onto beat phase
   - Predictive tracking (anticipate next beat)
   - Useful for synchronized effects

4. **Real-Time Causality**
   - Zero lookahead for live performance
   - Embedded system constraints
   - Low latency critical for responsiveness

---

## Detailed Redesign Recommendations

### 1. Fix Transient Detection - TRUE Impulses

**Priority**: CRITICAL
**Complexity**: Low
**Files**: `AdaptiveMic.h`, `AdaptiveMic.cpp`

**Implementation**:

```cpp
// In AdaptiveMic.h - Replace existing transient variables
public:
  // Enhanced transient detection
  bool     transientImpulse = false;    // One-shot trigger (binary)
  float    transientStrength = 0.0f;    // Strength of detected transient (0-1)

  // Remove old 'transient' and 'transientDecay' members

  // Getters
  bool getTransientImpulse() const { return transientImpulse; }
  float getTransientStrength() const { return transientStrength; }
```

```cpp
// In AdaptiveMic.cpp - Replace transient detection logic
void AdaptiveMic::update(float dt) {
    // ... existing code ...

    // Reset impulse each frame
    transientImpulse = false;

    float x = levelPostAGC;

    // Energy envelope first-order difference
    float energyDiff = maxValue(0.0f, x - slowAvg);

    // Update slow baseline (medium-term average ~150ms)
    slowAvg += slowAlpha * (x - slowAvg);

    uint32_t now = time_.millis();
    bool cooldownExpired = (now - lastTransientMs) > transientCooldownMs;

    // Adaptive threshold based on recent activity
    float adaptiveThreshold = maxValue(loudFloor, slowAvg * transientFactor);

    // Detect onset when:
    // 1. Cooldown has expired
    // 2. Energy difference exceeds threshold
    // 3. Current level is above baseline (rising edge)
    if (cooldownExpired && energyDiff > adaptiveThreshold && x > slowAvg * 1.2f) {
        transientImpulse = true;
        transientStrength = minValue(1.0f, energyDiff / adaptiveThreshold);
        lastTransientMs = now;
    }
}
```

**Usage in Effects**:
```cpp
// In fire effect or other visual effects
if (mic->getTransientImpulse()) {
    // Trigger envelope, flash, particle burst, etc.
    sparkIntensity = mic->getTransientStrength();
    triggerFlash();
}

// Continuous effects use getLevel()
flameHeight = mic->getLevel() * maxHeight;
```

**Testing**:
- Play single kick drum hit: should see exactly one impulse
- Play steady beat: impulses should align perfectly with beats
- Verify `transientImpulse` is true for only one frame per detection

---

### 2. Fix Software AGC with Proper Time Constant

**Priority**: HIGH
**Complexity**: Medium
**Files**: `AdaptiveMic.h`, `AdaptiveMic.cpp`

**Implementation**:

```cpp
// In AdaptiveMic.h - Add new constants and state
namespace MicConstants {
    constexpr float AGC_TAU_SECONDS = 7.0f;      // 5-10 second window
    constexpr float AGC_ATTACK_TAU = 2.0f;       // Faster attack (2s)
    constexpr float AGC_RELEASE_TAU = 10.0f;     // Slower release (10s)
}

class AdaptiveMic {
    // ... existing members ...

private:
    float trackedLevel = 0.0f;  // Tracked RMS level over AGC window
};
```

```cpp
// In AdaptiveMic.cpp - Replace autoGainTick()
void AdaptiveMic::autoGainTick(float dt) {
    // Use adaptive time constant (faster attack, slower release)
    float tau = (levelPreGate > trackedLevel)
        ? MicConstants::AGC_ATTACK_TAU
        : MicConstants::AGC_RELEASE_TAU;
    float alpha = 1.0f - expf(-dt / tau);

    // Track the level with EMA
    trackedLevel += alpha * (levelPreGate - trackedLevel);

    // Compute gain error based on tracked level (not instantaneous)
    float err = agTarget - trackedLevel;

    // Apply gain adjustment with defined time constant
    float gainAlpha = 1.0f - expf(-dt / MicConstants::AGC_TAU_SECONDS);
    globalGain += gainAlpha * err * globalGain;  // Proportional adjustment

    // Clamp to limits
    globalGain = constrainValue(globalGain, agMin, agMax);

    // Dwell tracking (existing logic)
    if (fabsf(levelPreGate) < 1e-6f && globalGain >= agMax * 0.999f) {
        dwellAtMax += dt;
    } else if (globalGain >= agMax * 0.999f) {
        dwellAtMax += dt;
    } else if (dwellAtMax > 0.0f) {
        dwellAtMax = maxValue(0.0f, dwellAtMax - dt * (1.0f/limitDwellRelaxSec));
    }

    if (levelPreGate >= 0.98f && globalGain <= agMin * 1.001f) {
        dwellAtMin += dt;
    } else if (globalGain <= agMin * 1.001f) {
        dwellAtMin += dt;
    } else if (dwellAtMin > 0.0f) {
        dwellAtMin = maxValue(0.0f, dwellAtMin - dt * (1.0f/limitDwellRelaxSec));
    }
}
```

**Testing**:
- Play silence then sudden loud music: AGC should settle in ~7 seconds
- Play loud then quiet: should take ~10 seconds to increase gain
- Verify no pumping/breathing artifacts during music
- Check that dynamics are preserved (quiet parts still quieter than loud parts)

---

### 3. Fix Hardware Gain Time Constant

**Priority**: MEDIUM
**Complexity**: Trivial
**Files**: `AdaptiveMic.h`

**Implementation**:

```cpp
// In AdaptiveMic.h
public:
  // Hardware gain (minutes scale) - CHANGED from 60000
  uint32_t hwCalibPeriodMs = 180000;  // 3 minutes (180 seconds)
```

**Rationale**:
- Allows adaptation to environmental changes over minutes
- Prevents interference with software AGC (different time scales)
- Handles: quiet room → loud concert, outdoor → indoor, etc.

**Testing**:
- Start in quiet room, verify hardware gain increases slowly
- Move to loud environment, verify hardware gain decreases over ~3 minutes
- Confirm software AGC handles second-to-second dynamics

---

### 4. Reduce Transient Cooldown for Musical Timing

**Priority**: HIGH
**Complexity**: Trivial
**Files**: `AdaptiveMic.h`

**Implementation**:

```cpp
// In AdaptiveMic.h - CHANGED from 120
uint32_t transientCooldownMs = 60;  // 16.7 hits/sec (supports 32nd notes at 125 BPM)
```

**Rationale**:

Musical timing requirements:
- 120 BPM, 16th notes: 8 notes/sec = 125ms spacing
- 120 BPM, 32nd notes: 16 notes/sec = 62.5ms spacing
- Fast hi-hat patterns can exceed 8 hits/sec

Current 120ms cooldown misses fast patterns.
New 60ms cooldown supports up to 16.7 hits/sec.

**Testing**:
- Play fast hi-hat pattern (16th notes at 140 BPM)
- Verify all hits are detected
- Check for false triggers (should be prevented by threshold)

---

### 5. Add Frequency-Aware Detection (Advanced - Optional)

**Priority**: NICE-TO-HAVE
**Complexity**: High
**Files**: `AdaptiveMic.h`, `AdaptiveMic.cpp`

**Rationale**:
- Distinguish kicks (bass) from snares (mid) from hi-hats (high)
- Enable frequency-specific visual effects
- Better musical responsiveness

**Lightweight Implementation** (without FFT):

```cpp
// In AdaptiveMic.h
public:
  // Frequency-specific transient detection
  bool  kickImpulse = false;   // Bass transient (60-130 Hz)
  bool  snareImpulse = false;  // Mid transient (300-750 Hz)
  float kickStrength = 0.0f;
  float snareStrength = 0.0f;

  // Frequency band tracking
  float bassLevel = 0.0f;
  float midLevel = 0.0f;
  float bassBaseline = 0.0f;
  float midBaseline = 0.0f;

  // Thresholds
  float kickThreshold = 1.8f;   // Kicks are very pronounced in bass
  float snareThreshold = 1.6f;  // Snares in mid range
```

```cpp
// In AdaptiveMic.cpp - Add to update() after consuming ISR
void AdaptiveMic::update(float dt) {
    // ... existing code to get avgAbs ...

    // Simple first-order IIR filters to approximate frequency bands
    // (Proper implementation would use biquad filters)

    // Bass: low-pass filter (approximate 130 Hz cutoff at 16kHz sample rate)
    float bassAlpha = 0.05f;  // Slower for low frequencies
    bassLevel += bassAlpha * (avgAbs - bassLevel);

    // Mid: faster response for mid frequencies
    float midAlpha = 0.15f;
    midLevel += midAlpha * (avgAbs - midLevel);

    // Update baselines (very slow)
    bassBaseline += 0.01f * (bassLevel - bassBaseline);
    midBaseline += 0.01f * (midLevel - midBaseline);

    // Reset impulses
    kickImpulse = false;
    snareImpulse = false;

    uint32_t now = time_.millis();

    // Detect kick: bass energy spike
    if ((now - lastKickMs) > transientCooldownMs) {
        if (bassLevel > bassBaseline * kickThreshold) {
            kickImpulse = true;
            kickStrength = minValue(1.0f, bassLevel / (bassBaseline * kickThreshold));
            lastKickMs = now;
        }
    }

    // Detect snare: mid energy spike
    if ((now - lastSnareMs) > transientCooldownMs) {
        if (midLevel > midBaseline * snareThreshold) {
            snareImpulse = true;
            snareStrength = minValue(1.0f, midLevel / (midBaseline * snareThreshold));
            lastSnareMs = now;
        }
    }

    // ... continue with existing code ...
}
```

**Advanced Implementation** (with proper filters):
- Use biquad IIR filters for accurate frequency bands
- Implement proper band-pass filters
- Consider lightweight FFT if MCU has DSP instructions

**Trade-offs**:
- Adds CPU overhead (minimal with IIR, moderate with FFT)
- Requires tuning thresholds and filter coefficients
- May need different settings for different music genres

**Testing**:
- Play isolated kick drum: verify only kickImpulse fires
- Play isolated snare: verify only snareImpulse fires
- Play full drum pattern: verify both detect independently
- Test with electronic music (synthetic drums) vs acoustic

---

## Architecture Changes

### Clear Separation of Outputs

The AdaptiveMic should provide three distinct outputs:

1. **Continuous Energy** (`getLevel()` → `levelPostAGC`)
   - Range: 0.0 to 1.0
   - Updates every frame
   - Use for: flame height, color intensity, continuous effects

2. **Transient Impulses** (`getTransientImpulse()` → `transientImpulse`)
   - Binary: true or false
   - True for exactly ONE frame when transient detected
   - Use for: spark bursts, flashes, discrete events

3. **Transient Strength** (`getTransientStrength()` → `transientStrength`)
   - Range: 0.0 to 1.0
   - Indicates strength of detected transient
   - Use for: scaling triggered effects

**Optional frequency-specific:**
- `getKickImpulse()` / `getKickStrength()`
- `getSnareImpulse()` / `getSnareStrength()`

### Effect Integration Pattern

```cpp
// In fire effect update loop
void FireEffect::update(float dt) {
    AdaptiveMic* mic = getMic();

    // Continuous effect - flame height tracks music level
    float energy = mic->getLevel();
    baseFlameHeight = minHeight + (energy * (maxHeight - minHeight));

    // Discrete events - sparks on transients
    if (mic->getTransientImpulse()) {
        float strength = mic->getTransientStrength();
        triggerSparks(strength * maxSparks);
        flashIntensity = strength;
    }

    // Optional: frequency-specific effects
    if (mic->getKickImpulse()) {
        // Boost bass colors, expand flame base
        bassBoost = mic->getKickStrength();
    }
    if (mic->getSnareImpulse()) {
        // Trigger bright flashes, spawn particles
        triggerSnareFlash(mic->getSnareStrength());
    }

    // ... rest of effect logic ...
}
```

---

## Testing and Validation Plan

### Unit Tests

**Test Cases to Add** (`tests/unit/test_audio.cpp`):

1. **Transient Impulse Duration**
   ```cpp
   void testTransientImpulseDuration() {
       // Trigger transient
       // Verify impulse is true for exactly 1 frame
       // Verify subsequent frames return false
   }
   ```

2. **AGC Time Constant Validation**
   ```cpp
   void testAGCTimeConstant() {
       // Apply step input (0 → 1)
       // Measure time to 63.2% of final value
       // Should be ~7 seconds
   }
   ```

3. **Hardware Gain Period**
   ```cpp
   void testHardwareGainPeriod() {
       // Verify calibration only runs every 180 seconds
   }
   ```

4. **Transient Cooldown**
   ```cpp
   void testTransientCooldown() {
       // Trigger transients at high rate
       // Verify minimum spacing of 60ms
   }
   ```

### Integration Tests

**Test Audio Files Needed**:

1. **Isolated Kick Drum**
   - Single kick hits at various intensities
   - Verify transient detection accuracy
   - Measure false positive rate

2. **Isolated Snare Drum**
   - Single snare hits
   - Test frequency-specific detection (if implemented)

3. **Metronome / Click Track**
   - Precise timing reference
   - Verify transient alignment with beats
   - Measure jitter/latency

4. **Fast Hi-Hat Pattern**
   - 16th notes at 140 BPM
   - Verify all hits detected (no misses due to cooldown)

5. **Full Drum Kit Pattern**
   - Mixed kicks, snares, hi-hats
   - Verify complex pattern tracking
   - Check for false triggers

6. **Dynamic Music (Quiet → Loud)**
   - Test AGC adaptation
   - Measure settling time (~7 seconds expected)
   - Verify dynamics preserved

7. **Sustained Notes (No Transients)**
   - String pad, organ, drone
   - Verify no false transient triggers
   - Confirm energy level tracks correctly

### Live Testing Protocol

1. **Setup**:
   - Connect to serial console for debugging
   - Enable audio visualization on LEDs
   - Prepare music with known characteristics

2. **Verification Steps**:
   - Play test audio files
   - Log outputs: `levelPostAGC`, `transientImpulse`, `globalGain`, `hwGain`
   - Record LED behavior on video
   - Compare to expected behavior

3. **Metrics to Track**:
   - Transient detection accuracy (% of beats detected)
   - False positive rate (spurious triggers)
   - AGC settling time (seconds)
   - Visual responsiveness (subjective, 1-10 scale)

4. **Environment Tests**:
   - Quiet room (library levels)
   - Normal conversation
   - Loud music (concert levels)
   - Measure hardware gain adaptation over time

---

## Performance Considerations

### CPU Overhead Estimates

**Current Implementation**:
- ISR: ~50-100μs per callback (buffer processing)
- Update: ~200-300μs per frame (envelope, AGC, transient)
- Total: <1% CPU at 16kHz sample rate, 60 FPS

**After Improvements**:

1. **Basic Fixes** (impulse + AGC):
   - +20-30μs (EMA calculations)
   - Still <1% CPU
   - No significant impact

2. **With Frequency Detection** (IIR filters):
   - +50-100μs (additional filtering)
   - ~1-2% CPU
   - Acceptable overhead

3. **With FFT** (not recommended):
   - +2-5ms (FFT computation)
   - 10-25% CPU
   - May impact LED rendering
   - Only consider if MCU has hardware DSP

### Memory Overhead

**Current**: ~200 bytes (state variables)

**After Improvements**:
- Basic fixes: +16 bytes (trackedLevel, transientStrength)
- Frequency detection: +48 bytes (band levels, baselines, timestamps)
- Total: ~264 bytes (negligible on nRF52840 with 256KB RAM)

### Real-Time Constraints

**Critical Requirements**:
- ISR must complete in <200μs (to avoid buffer overruns)
- Update must complete in <5ms (for 60 FPS LED rendering)
- Zero dynamic memory allocation (no malloc in ISR)
- No blocking operations

**All proposed changes meet these constraints.**

---

## Implementation Priority

### Phase 1: Critical Fixes (Immediate)

1. **Fix transient detection to be impulses** ✓ HIGH IMPACT
   - Changes: `AdaptiveMic.h`, `AdaptiveMic.cpp`
   - Testing: Isolated kick drum, metronome
   - Expected: Perfect beat alignment

2. **Reduce transient cooldown to 60ms** ✓ QUICK WIN
   - Change: 1 line in `AdaptiveMic.h`
   - Testing: Fast hi-hat pattern
   - Expected: No missed hits

### Phase 2: AGC Improvements (High Priority)

3. **Implement proper AGC time constants** ✓ HIGH IMPACT
   - Changes: `AdaptiveMic.h`, `AdaptiveMic.cpp`
   - Testing: Dynamic music, silence→loud
   - Expected: Smooth 5-10 sec adaptation

4. **Increase hardware gain period to 3 minutes** ✓ QUICK WIN
   - Change: 1 line in `AdaptiveMic.h`
   - Testing: Environmental change test
   - Expected: Slow environmental adaptation

### Phase 3: Advanced Features (Optional)

5. **Add frequency-aware detection**
   - Changes: Major additions to both files
   - Testing: Isolated kick/snare, full drum pattern
   - Expected: Instrument-specific triggering

6. **Add beat tracking/tempo estimation**
   - Changes: New module or extensive additions
   - Testing: Various tempo music
   - Expected: Predictive beat sync

---

## Success Criteria

### Minimum Viable (Phase 1 + 2)

- ✓ Transients fire exactly on musical beats
- ✓ No missed hits on fast patterns (16th notes, 140 BPM)
- ✓ No false triggers during sustained notes
- ✓ AGC adapts in 5-10 seconds to dynamic changes
- ✓ Hardware gain adapts in ~3 minutes to environmental changes
- ✓ Visual effects respond musically (subjective quality)

### Ideal (Phase 3)

- ✓ Kick and snare detected independently
- ✓ Effects can respond differently to different instruments
- ✓ Beat tracking predicts next beat (for synchronized effects)
- ✓ Tempo estimation available for tempo-scaled effects
- ✓ Works across genres (EDM, rock, classical, jazz)

---

## Known Limitations and Future Work

### Current Limitations

1. **No True Frequency Analysis**
   - IIR filters are approximations
   - Cannot distinguish specific frequency content accurately
   - True FFT would be expensive on embedded system

2. **Fixed Thresholds**
   - Transient thresholds may need tuning per-genre
   - No automatic adaptation to music style
   - Future: machine learning-based detection?

3. **No Tempo/Beat Tracking**
   - Current system is reactive, not predictive
   - Cannot anticipate next beat for synchronized effects
   - Future: implement beat tracker (BTrack, OBTAIN)

4. **Mono Audio Only**
   - No stereo, no spatial information
   - Future: could use stereo for directional effects

### Future Enhancements

1. **Adaptive Thresholding**
   - Learn optimal thresholds from music characteristics
   - Adapt to genre (EDM needs different settings than jazz)

2. **Multi-Band Analysis**
   - Full spectrum analysis (bass, low-mid, mid, high-mid, high)
   - Enable rich frequency-reactive effects

3. **Beat/Tempo Tracking**
   - Predict next beat for synchronized effects
   - Estimate tempo for tempo-scaled animations
   - Downbeat detection for phrase-level effects

4. **Genre Detection**
   - Automatically adjust parameters based on music style
   - Optimize thresholds for EDM vs rock vs classical

5. **Harmonic Analysis**
   - Detect musical key/chord changes
   - Color effects based on harmonic content

---

## References and Resources

### Academic Papers

- [Onset Detection - Audio Labs Erlangen](https://www.audiolabs-erlangen.de/resources/MIR/FMP/C6/C6S1_OnsetDetection.html)
- [OBTAIN: Real-Time Beat Tracking in Audio Signals](https://arxiv.org/pdf/1704.02216)
- [Real-Time Beat Tracking with Zero Latency](https://transactions.ismir.net/articles/10.5334/tismir.189)

### Tutorials and Guides

- [musicinformationretrieval.com - Onset Detection](https://musicinformationretrieval.com/onset_detection.html)
- [musicinformationretrieval.com - Novelty Functions](https://musicinformationretrieval.com/novelty_functions.html)
- [Onset Detection – Cycfi Research](https://www.cycfi.com/2021/01/onset-detection/)
- [Essentia - Onset Detection Tutorial](https://essentia.upf.edu/tutorial_rhythm_onsetdetection.html)

### Open Source Implementations

- [BTrack - Real-Time Beat Tracker](https://github.com/adamstark/BTrack)
- [Beat-and-Tempo-Tracking - ANSI C Library](https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking)
- [BeatNet - State-of-the-art Beat Tracking](https://github.com/mjhydri/BeatNet)

### Professional Audio Standards

- [Q-SYS AGC Documentation](https://q-syshelp.qsc.com/Content/Schematic_Library/leveler.htm)
- [How AGC Works - Wireless Pi](https://wirelesspi.com/how-automatic-gain-control-agc-works/)
- [Understanding Automatic Gain Control](https://www.allaboutcircuits.com/technical-articles/understanding-automatic-gain-control/)

### DSP Resources

- [Spectral Flux - Wikipedia](https://en.wikipedia.org/wiki/Spectral_flux)
- [Automatic Gain Control - Wikipedia](https://en.wikipedia.org/wiki/Automatic_gain_control)

---

## Appendix: Code Locations

### Files to Modify

1. **blinky-things/inputs/AdaptiveMic.h**
   - Line 72: Change transient to transientImpulse (bool)
   - Line 80: Change transientCooldownMs to 60
   - Line 49: Change hwCalibPeriodMs to 180000
   - Add: trackedLevel state variable
   - Add: AGC time constant definitions

2. **blinky-things/inputs/AdaptiveMic.cpp**
   - Lines 107-127: Replace transient detection logic
   - Lines 181-203: Replace AGC logic with EMA-based tracking
   - Add: Frequency detection logic (optional)

3. **blinky-things/blinky-things.ino**
   - Lines 106-107: Update to use getTransientImpulse()
   - Lines 360-361: Update to use getTransientImpulse()

4. **tests/unit/test_audio.cpp**
   - Add: Unit tests for new functionality
   - Add: Time constant validation tests

### Configuration Files

1. **blinky-things/config/ConfigStorage.cpp**
   - Lines 71-73: Update config for new transient parameters
   - Add: New AGC parameters if exposing to config

---

## Change Log

- **2025-12-23**: Initial analysis and plan created
- **TBD**: Phase 1 implementation
- **TBD**: Phase 2 implementation
- **TBD**: Phase 3 evaluation

---

**Status**: Ready for implementation approval
**Next Step**: Implement Phase 1 critical fixes
