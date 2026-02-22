# Visualizer Design Goals

*Last Updated: February 21, 2026*

## Core Philosophy

The goal is **visual quality**, not metric perfection. A musically responsive visualizer doesn't need perfect real-time analysis — it needs to react to obvious musical events and gracefully ignore everything else. Missing a soft beat is invisible to the audience; a false trigger on a pad looks wrong.

## Design Principles

### 1. Precision > Recall

Only fire on obvious strong musical events (kicks, snares, big transitions). Don't chase every onset. A missed beat causes no visual artifact. A false positive creates a random spark/burst with no musical cause — this breaks immersion.

### 2. rhythmStrength Is the Key Output

The continuous `rhythmStrength` (0-1) blend between organic and music-reactive mode matters more than perfect beat placement. When the system can't confidently track beats, it should gracefully fall back to organic mode (breathing, slow evolution) rather than guessing wrong.

### 3. BPM Stability > BPM Accuracy

A stable wrong BPM (e.g., half-time at 60 BPM instead of 120 BPM) looks fine visually — the LEDs pulse in a consistent rhythmic pattern that still feels musical. A jittery correct BPM that jumps between values looks terrible — the visual rhythm constantly breaks and resets.

### 4. False Positives Are the #1 Visual Problem

Random sparks or bursts with no musical cause are the most visible artifact. They break the viewer's sense that the lights are "listening" to the music. All detection tuning should prioritize eliminating false positives over catching more true positives.

### 5. Different Music Needs Different Visual Responses

Not every genre needs beat-sync. The `rhythmStrength` blend handles this automatically when working correctly:

| Music Type | Expected Visual Behavior |
|------------|--------------------------|
| Dance/techno with clear kicks | Beat-sync mode, sparks on kicks, strong phase-locked pulsing |
| Ambient/drone | Organic mode, energy-driven breathing, slow color evolution |
| Complex/syncopated | Partial music mode, energy + occasional bursts on strong accents |
| Silence | Pure organic with minimal activity |

Tracks like ambient, trap, or machine-drum scoring low Beat F1 may actually represent **correct visual behavior** — the system appropriately avoids beat-chasing on content where confident beat tracking isn't possible.

## Evaluation Criteria

### What Matters for Visual Quality

1. **No false triggers on pads/swells/noise** — The most visible artifact
2. **Stable rhythmic feel** — Consistent pulsing when music has a clear beat
3. **Graceful degradation** — Smooth organic mode when beat tracking is uncertain
4. **Energy responsiveness** — Lights should breathe with the music's overall energy even without beat-lock
5. **Clean transitions** — No jarring mode switches between organic and music-reactive

### What Doesn't Matter (Much)

1. **Beat F1 on ambient tracks** — Low F1 is correct if the system goes organic
2. **Exact BPM accuracy** — Half-time or double-time still looks rhythmic
3. **Transient F1 on sparse content** — Missing quiet onsets is fine
4. **Per-track beat offset** — 50-80ms variation is invisible at LED update rates

### Track Category Expectations

**High priority (should work well):**
- Four-on-the-floor dance/techno: Beat F1 > 0.6, stable BPM
- Music with strong kicks/snares: Transient precision > 0.8

**Medium priority (should degrade gracefully):**
- Syncopated/complex: Some beat tracking, mostly energy-reactive
- Sparse/minimal: Organic mode with occasional bursts

**Low priority (organic mode is correct):**
- Ambient/drone: Pure organic, no beat tracking expected
- Silence: Minimal activity

### Testing Priorities (Ordered by Visual Impact)

1. **False positive elimination** — Test pad-rejection, chord-rejection, synth-stabs
2. **Strong beat detection** — Test strong-beats, medium-beats (should be near-perfect)
3. **Hat/cymbal rejection** — Test hat-rejection (should reject cleanly)
4. **Real music with clear beats** — trance-party, minimal-01, infected-vibes
5. **Graceful degradation** — deep-ambience, trap-electro (verify organic fallback)
