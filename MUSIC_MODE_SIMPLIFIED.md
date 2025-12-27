# Music Mode - Simplified Design for LED Effects

## Core Concept

**Music mode provides 3 things to LED generators:**
1. **Beat events** - "A beat just happened"
2. **Phase** - "Where are we in the beat?" (0.0 → 1.0)
3. **BPM** - "How fast is the music?" (80-180 BPM typical)

That's it. Everything else is the generator's creative interpretation.

---

## Minimal Architecture

```
AdaptiveMic (existing)
    ↓ (onset events)
MusicMode
    ├─ BPM tracking (autocorrelation)
    ├─ Phase tracking (PLL)
    └─ Confidence (activation/deactivation)
    ↓ (beat, phase, bpm)
Fire Generator
    ├─ Sync spark bursts to beats
    ├─ Breathe cooling with phase
    └─ Intensity follows tempo
```

---

## Single Class: MusicMode

```cpp
class MusicMode {
public:
    // State
    bool active = false;
    float bpm = 120.0f;           // Beats per minute
    float phase = 0.0f;           // 0.0-1.0 within current beat
    uint32_t beatNumber = 0;      // Increments on each beat

    // Events (set for ONE frame, then cleared)
    bool beatHappened = false;    // TRUE for one frame when beat occurs
    bool quarterNote = false;     // Every beat
    bool halfNote = false;        // Every 2 beats
    bool wholeNote = false;       // Every 4 beats

    // Constructor
    MusicMode(ISystemTime& time);

    // Called every frame
    void update(float dt);

    // Called when AdaptiveMic detects onset
    void onOnsetDetected(uint32_t timestampMs, bool isLowBand);

    // Getters for generators
    inline float getPhase() const { return phase; }
    inline float getBPM() const { return bpm; }
    inline bool isActive() const { return active; }

    // Tunable parameters (exposed via serial console)
    float activationThreshold = 0.6f;    // Confidence needed to activate
    uint8_t minBeatsToActivate = 4;      // Stable beats required
    uint8_t maxMissedBeats = 8;          // Missed beats before deactivation
    float bpmMin = 60.0f;                // Tempo range
    float bpmMax = 200.0f;

private:
    ISystemTime& time_;

    // Tempo estimation (autocorrelation)
    static constexpr uint8_t MAX_ONSETS = 64;
    uint32_t onsetTimes_[MAX_ONSETS];
    uint8_t onsetIndex_ = 0;
    uint32_t lastOnsetTime_ = 0;

    // Beat tracking (PLL)
    float beatPeriodMs_ = 500.0f;  // Period of one beat (ms)
    float phaseError_ = 0.0f;
    float errorIntegral_ = 0.0f;

    // Confidence tracking
    float confidence_ = 0.0f;
    uint8_t stableBeats_ = 0;
    uint8_t missedBeats_ = 0;

    // Internal methods
    void estimateTempo();
    void updatePhase(float dt);
    void adjustPeriod(float error);
    void updateConfidence();
};
```

---

## How Generators Use It

### Example: Fire Generator

```cpp
class Fire {
    // ... existing code ...

    void update(float dt, const MusicMode& music) {
        if (music.isActive()) {
            // === BEAT SYNC: Burst sparks on beats ===
            if (music.beatHappened) {
                // Stronger bursts on downbeats
                uint8_t sparkCount = music.wholeNote ? burstSparks * 2 : burstSparks;
                injectSparks(sparkCount);
            }

            // === PHASE SYNC: Breathe cooling with beat ===
            // Phase 0.0 → 1.0 maps to one beat cycle
            // Use sine wave for smooth breathing
            float breathe = sinf(music.phase * 2.0f * PI);  // -1 to 1
            uint8_t coolingMod = breathe * 15.0f;  // ±15 variation
            applyCooling(baseCooling + coolingMod);

            // === TEMPO SYNC: Adjust decay speed ===
            // Faster music = faster decay (more energetic)
            float tempoScale = music.bpm / 120.0f;  // 1.0 at 120 BPM
            float adjustedDecay = heatDecay * tempoScale;
            applyDecay(adjustedDecay);

        } else {
            // Free-running mode - use existing random sparks
            updateNormal(dt);
        }
    }
};
```

### Example: Future Color Effect

```cpp
class ColorPulse {
    void update(float dt, const MusicMode& music) {
        if (music.isActive()) {
            // Hue rotates over 4 beats (one bar)
            float hue = fmodf(music.beatNumber / 4.0f, 1.0f);

            // Brightness pulses with beat phase
            float brightness = 0.5f + 0.5f * sinf(music.phase * 2.0f * PI);

            setColor(hue, 1.0f, brightness);
        }
    }
};
```

---

## Algorithm Details

### 1. Tempo Estimation (Simple Autocorrelation)

```cpp
void MusicMode::estimateTempo() {
    // Only run every 8 onsets (reduces CPU)
    if (onsetIndex_ % 8 != 0) return;

    // Calculate inter-onset intervals (IOIs)
    uint8_t numIOIs = min(onsetIndex_, MAX_ONSETS - 1);
    if (numIOIs < 4) return;  // Need at least 4 intervals

    // Find most common interval (simple histogram)
    uint16_t histogram[40] = {0};  // 60-200 BPM in 10ms bins
    for (uint8_t i = 1; i < numIOIs; i++) {
        uint32_t ioi = onsetTimes_[i] - onsetTimes_[i-1];

        // Convert to BPM bin (60000ms/min ÷ ioi)
        if (ioi >= 300 && ioi <= 1000) {  // 60-200 BPM
            uint8_t bin = (ioi - 300) / 20;  // 20ms bins
            histogram[bin]++;
        }
    }

    // Find peak in histogram
    uint8_t peakBin = 0;
    uint16_t peakValue = 0;
    for (uint8_t i = 0; i < 40; i++) {
        if (histogram[i] > peakValue) {
            peakValue = histogram[i];
            peakBin = i;
        }
    }

    // Convert bin back to BPM
    if (peakValue >= 3) {  // Need at least 3 matching intervals
        uint32_t ioi = 300 + (peakBin * 20);
        float newBPM = 60000.0f / ioi;

        // Smooth update
        bpm = bpm * 0.8f + newBPM * 0.2f;
        beatPeriodMs_ = 60000.0f / bpm;

        confidence_ = min(confidence_ + 0.2f, 1.0f);
    }
}
```

### 2. Phase Tracking (Phase-Locked Loop)

```cpp
void MusicMode::updatePhase(float dt) {
    // Advance phase
    phase += dt / beatPeriodMs_;

    // Wrap phase (0.0 - 1.0)
    if (phase >= 1.0f) {
        phase -= 1.0f;
        beatNumber++;
        beatHappened = true;

        // Set note triggers
        quarterNote = true;
        halfNote = (beatNumber % 2 == 0);
        wholeNote = (beatNumber % 4 == 0);
    }
}

void MusicMode::onOnsetDetected(uint32_t timestampMs, bool isLowBand) {
    // Store onset time
    onsetTimes_[onsetIndex_] = timestampMs;
    onsetIndex_ = (onsetIndex_ + 1) % MAX_ONSETS;

    // Calculate phase error (how far off are we?)
    // Expected: onset at phase ~0.0 or ~1.0
    float error = phase;
    if (error > 0.5f) error -= 1.0f;  // Wrap to -0.5 to 0.5

    // PLL correction (Kp = 0.1, Ki = 0.01)
    errorIntegral_ += error;
    float correction = 0.1f * error + 0.01f * errorIntegral_;

    // Adjust beat period
    beatPeriodMs_ *= (1.0f - correction);
    bpm = 60000.0f / beatPeriodMs_;

    // Clamp BPM
    bpm = constrain(bpm, bpmMin, bpmMax);
    beatPeriodMs_ = 60000.0f / bpm;

    // Update confidence
    if (abs(error) < 0.2f) {  // Onset within 20% of expected
        stableBeats_++;
        missedBeats_ = 0;
        confidence_ = min(confidence_ + 0.1f, 1.0f);
    } else {
        missedBeats_++;
        confidence_ = max(confidence_ - 0.1f, 0.0f);
    }

    // Estimate tempo periodically
    estimateTempo();

    lastOnsetTime_ = timestampMs;
}
```

### 3. Activation Logic

```cpp
void MusicMode::update(float dt) {
    // Clear one-shot events
    beatHappened = false;
    quarterNote = false;
    halfNote = false;
    wholeNote = false;

    // Update phase
    updatePhase(dt);

    // Check for missed beats (no onsets for too long)
    uint32_t timeSinceOnset = time_.millis() - lastOnsetTime_;
    if (timeSinceOnset > beatPeriodMs_ * 2.0f) {
        missedBeats_++;
        confidence_ = max(confidence_ - 0.05f, 0.0f);
    }

    // Activate music mode
    if (!active && confidence_ >= activationThreshold &&
        stableBeats_ >= minBeatsToActivate) {
        active = true;
        Serial.println(F("[MUSIC] Mode activated"));
    }

    // Deactivate music mode
    if (active && (confidence_ < activationThreshold * 0.5f ||
                   missedBeats_ >= maxMissedBeats)) {
        active = false;
        stableBeats_ = 0;
        missedBeats_ = 0;
        Serial.println(F("[MUSIC] Mode deactivated"));
    }
}
```

---

## Resource Usage

| Component | RAM | CPU @ 60fps | Notes |
|-----------|-----|-------------|-------|
| MusicMode | ~512 bytes | ~3% | Autocorrelation every 8th onset |
| **Total** | **0.5KB** | **3%** | **Minimal!** |

**Current headroom:** ~250KB RAM, ~30% CPU available

---

## Serial Console Integration

```cpp
// In SerialConsole::registerSettings()
settings_.registerFloat("musicthresh", &music_->activationThreshold, "music",
    "Music mode activation threshold (0-1)", 0.0f, 1.0f);
settings_.registerUint8("musicbeats", &music_->minBeatsToActivate, "music",
    "Stable beats to activate", 2, 16);
settings_.registerFloat("bpmmin", &music_->bpmMin, "music",
    "Minimum BPM", 40.0f, 120.0f);
settings_.registerFloat("bpmmax", &music_->bpmMax, "music",
    "Maximum BPM", 120.0f, 240.0f);

// Status display
if (strcmp(cmd, "music") == 0) {
    Serial.print(F("Active: ")); Serial.println(music_->active ? "YES" : "NO");
    Serial.print(F("BPM: ")); Serial.println(music_->bpm);
    Serial.print(F("Phase: ")); Serial.println(music_->phase);
    Serial.print(F("Beat #: ")); Serial.println(music_->beatNumber);
    Serial.print(F("Confidence: ")); Serial.println(music_->confidence_);
}
```

---

## Testing Plan

### Phase 1: Core Algorithm (Week 1)
1. Implement MusicMode class
2. Wire up onset events from AdaptiveMic
3. Test BPM detection with metronome app (60, 120, 180 BPM)
4. Tune PLL parameters for stable phase tracking
5. Test activation/deactivation logic

### Phase 2: Generator Integration (Week 2)
1. Add music mode query to Fire::update()
2. Implement beat-synced spark bursts
3. Implement phase-synced cooling breathe
4. Test with various music genres
5. Tune for visual appeal

### Phase 3: Polish (Week 3)
1. Add serial console commands
2. Optimize CPU usage
3. Add visual feedback (LED patterns?)
4. Document parameter tuning guide

---

## Generator Design Patterns

### Pattern 1: Beat Trigger
```cpp
if (music.beatHappened) {
    // Do something instantaneous
    flashLEDs();
    burstSparks();
}
```

### Pattern 2: Phase Modulation (Continuous)
```cpp
// Sine wave breathing (smooth)
float breathe = sinf(music.phase * TWO_PI);

// Triangle wave (linear ramp up/down)
float triangle = 1.0f - abs(music.phase * 2.0f - 1.0f);

// Sawtooth (ramp up, sharp drop)
float saw = music.phase;

// Square wave (on/off)
float square = music.phase < 0.5f ? 1.0f : 0.0f;
```

### Pattern 3: Multi-Beat Patterns
```cpp
if (music.wholeNote) {
    // Every 4 beats - major accent
    bigExplosion();
} else if (music.halfNote) {
    // Every 2 beats - medium accent
    mediumBurst();
} else if (music.quarterNote) {
    // Every beat - subtle pulse
    smallPulse();
}
```

### Pattern 4: Tempo-Scaled Parameters
```cpp
// Scale any parameter by tempo (1.0 = 120 BPM baseline)
float tempoScale = music.bpm / 120.0f;

// Faster music = faster effects
float decayRate = baseDecay * tempoScale;
float sparkChance = baseChance * tempoScale;
```

---

## Key Simplifications from Full Design

**Removed:**
- ❌ Complex pattern recognition (4-on-floor, breakbeat, etc.)
- ❌ Multiple LFO objects
- ❌ Pattern prediction
- ❌ RNN/DTW algorithms

**Kept (Essential):**
- ✅ BPM detection (autocorrelation)
- ✅ Phase tracking (PLL)
- ✅ Beat events (quarterNote, halfNote, wholeNote)
- ✅ Confidence-based activation

**Result:** 85% fewer lines of code, same creative potential for LED effects!

---

## Next Steps

Ready to implement? The order would be:

1. **Create `blinky-things/music/MusicMode.h` and `MusicMode.cpp`**
2. **Wire up to AdaptiveMic** (call `music.onOnsetDetected()` on transients)
3. **Test BPM tracking** with metronome/test patterns
4. **Integrate with Fire generator** (one simple effect first)
5. **Tune and polish**

Should I create the skeleton implementation?
