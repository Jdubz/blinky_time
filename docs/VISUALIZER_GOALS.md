# Visualizer Design Goals

*Last Updated: October 21, 2026*

> **Detector vs. visuals.** The detector's task is **onset detection**: fire on every percussive event (kick, snare, ghost note, hi-hat), on the beat or not (CLAUDE.md). How much each onset *shows* is a visual decision, made in the generators (strength-scaled bursts, thresholds like Fire's `burstthreshold`). The principles below are about the visuals; they don't mean training or scoring the detector on beats.

## Core Philosophy

The goal is **visual quality**, not metric perfection. A musically responsive visualizer doesn't need perfect real-time analysis — it needs to react to obvious musical events and gracefully ignore everything else. Missing a soft hit is nearly invisible to the audience; a false trigger on a pad looks wrong.

## Design Principles

### 1. Big visuals for big hits

The detector reports every percussive onset with a strength. The generators should make strong hits (kicks, snares, big transitions) the big visual events and let weak ones (hats, ghost notes) show small or not at all. A false positive — a burst with no percussive cause — breaks immersion more than a missed soft hit.

### 2. rhythmStrength Is the Key Output

The continuous `rhythmStrength` (0-1) blend between organic and music-reactive mode matters more than perfect phase placement. When the system can't confidently track the rhythm, it should gracefully fall back to organic mode (breathing, slow evolution) rather than guessing wrong. `rhythmStrength` also fades to 0 within 2-4 s of the last onset ("rhythm needs hits"), so room noise doesn't look like music.

### 3. Pattern Quality > BPM Accuracy

The system selects the period that produces the best visual pattern, not the "correct" BPM. A half-time period capturing a kick-snare-kick-snare bar is MORE CORRECT than a beat-level period capturing just a kick — it has more rhythmic structure and drives better visuals. **Octave-matched periods (half-time, double-time) are valid results, not errors.** A stable pattern at any octave is always preferable to a jittery correct BPM.

### 4. False Positives Are the #1 Visual Problem

Random sparks or bursts with no musical cause are the most visible artifact. They break the viewer's sense that the lights are "listening" to the music. A detection on a pad, chord or vocal is a false positive; a detection on a hi-hat is a true onset that the visuals should simply keep small.

### 5. Different Music Needs Different Visual Responses

Not every genre needs beat-sync. The `rhythmStrength` blend handles this automatically when working correctly:

| Music Type | Expected Visual Behavior |
|------------|--------------------------|
| Dance/techno with clear kicks | Music mode, bursts on kicks, strong phase-locked pulsing |
| Ambient/drone | Organic mode, energy-driven breathing, slow color evolution |
| Complex/syncopated | Partial music mode, energy + occasional bursts on strong accents |
| Silence / room noise | Pure organic with minimal activity (rhythm outputs fade after 2-4 s without onsets) |

Tracks like ambient or sparse techno with low rhythm confidence may actually show **correct visual behavior** — the system stays organic on content where confident rhythm tracking isn't possible. (Low onset F1 is still a detector problem worth fixing; see `docs/AUDIO-TUNING-GUIDE.md`.)

## Evaluation Criteria

### What Matters for Visual Quality

1. **No false triggers on pads/swells/noise** — The most visible artifact
2. **Stable rhythmic feel** — Consistent pulsing when music has a clear beat
3. **Graceful degradation** — Smooth organic mode when rhythm tracking is uncertain
4. **Energy responsiveness** — Lights should breathe with the music's overall energy (`energy`, `level`) even without rhythm lock
5. **Clean transitions** — No jarring mode switches between organic and music-reactive

### What Doesn't Matter (Much)

1. **Rhythm lock on ambient tracks** — No lock is correct if the system goes organic
2. **BPM at the "correct" octave** — Half-time or double-time periods often capture MORE rhythmic structure (e.g., a 2-bar pattern) and are the correct choice for visualization. The system intentionally selects the period with the best epoch-fold pattern, not the musicologically "correct" BPM. BPM accuracy scoring uses octave-tolerant ratios.
3. **Missing very quiet onsets** — Fine visually; they'd be small anyway
4. **Small phase offsets** — 50-80ms variation is hard to see at LED update rates

### Track Category Expectations

**High priority (should work well):**
- Four-on-the-floor dance/techno: strong onset recall on kicks/snares, stable period and phase lock
- Music with strong kicks/snares: few detections on non-percussive events

**Medium priority (should degrade gracefully):**
- Syncopated/complex: Some rhythm tracking, mostly energy-reactive
- Sparse/minimal: Organic mode with occasional bursts

**Low priority (organic mode is correct):**
- Ambient/drone: Pure organic, no rhythm lock expected
- Silence: Minimal activity

### Onset Density Target

For smooth visuals, aim for **2-4 major visual events per bar** (kicks and snares). The detector fires on every percussive onset, about 6.5/s in music on the flux path, hi-hats included. Hi-hats and cymbals should NOT drive big visual events; they create overly busy, jittery effects. Keep them small in the generators (strength scaling, burst thresholds); the flux band weights (`bassflux`/`midflux`/`highflux`) and the tempo-adaptive cooldown also shape what fires. Don't tune the detector to drop real onsets.

### Testing Priorities (Ordered by Visual Impact)

All on `blinky-test-player/music/edm/`, scored against `.onsets_consensus.json`:

1. **False positives** — detections on pads, chords, synth stabs and vocals
2. **Strong hits** — kick/snare recall on clear tracks (trance-party, techno-minimal-01, trance-infected-vibes)
3. **Visual weight of hats** — dense hi-hat passages should look calm, not busy
4. **Graceful degradation** — techno-deep-ambience, edm-trap-electro (verify organic fallback)
