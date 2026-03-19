# Blinky Time Glossary

Terminology is a recurring source of design confusion in this project. This glossary defines terms precisely to prevent circular reasoning from overloaded words.

## Musical Terms

| Term | Definition | Example |
|------|-----------|---------|
| **Beat** | A metrical grid position. Abstract, periodic, exists even during silence. | Beat 1 of a 4/4 bar, whether or not there's a kick |
| **Pulse** (musical) | The regularly recurring unit that defines tempo. In 4/4 time, the pulse is the quarter note. | "The pulse is 120 BPM" = 120 quarter notes per minute |
| **Onset** | An acoustic transient — the start of a percussive sound. Concrete, irregular, may or may not align with a beat. | A kick drum hit, a snare crack, a hi-hat tick |
| **Accent** | A stressed or emphasized onset. Subjective — depends on loudness, timbre, and context. | A snare on beat 2 is typically accented in rock/pop |
| **Syncopation** | Onsets that fall off the expected beat grid. Deliberate rhythmic tension. | Off-beat hi-hats, anticipated kicks |
| **Subdivision** | Divisions of the pulse. 8th notes divide the quarter-note pulse in half; 16th notes divide it into four. | At 120 BPM: quarter = 500ms, 8th = 250ms, 16th = 125ms |
| **Downbeat** | Beat 1 of a bar. The strongest metrical position. | The first beat of every 4-bar phrase |
| **Time signature** | Meter definition. 4/4 = four quarter-note pulses per bar. 6/8 = six eighth-note pulses per bar. | 4/4 and 8/8 have the same BPM but different pulse groupings |
| **Phase** | Position within one pulse cycle, expressed as 0.0 to 1.0. | Phase 0.0 = on the beat, 0.5 = halfway between beats |

## Firmware Terms

| Term | Definition | Firmware Location |
|------|-----------|-------------------|
| **ODF** | Onset Detection Function — the raw NN activation output (0-1). Higher = more likely a transient just occurred. | `AudioTracker::odf` variable |
| **Visual trigger** | A discrete visual event fired when ODF exceeds the baseline threshold. What the firmware currently calls `pulse` in `AudioControl.pulse`. | `AudioControl::pulse`, `lastPulseStrength_` |
| **Energy** | Continuous audio level (0-1) blending mic level, bass mel energy, and ODF peak-hold. Drives background animation intensity. | `AudioControl::energy` |
| **rhythmStrength** | Confidence that the audio has a periodic beat structure (0-1). Blend of ACF periodicity and comb bank confidence. Gates the confidence modulator. | `AudioControl::rhythmStrength` |
| **Phase ramp** | Free-running sawtooth (0-1) at the estimated BPM. Phase 0.0 = predicted beat position. | `AudioTracker::pllPhase_` |
| **Confidence modulator** | Scales visual trigger strength based on where the onset falls on the beat grid. On-grid onsets get boosted; off-grid get attenuated (but not zeroed — `confFloor` = 0.4). | `synthesizeOutputs()` cosine proximity curve |
| **ODF gate** | Threshold below which NN activations are suppressed to prevent noise-driven false triggers. | `odfGateThreshold` (default 0.20) |
| **Baseline tracking** | Floor-tracking algorithm that adapts to the ODF noise floor. Visual triggers fire when ODF exceeds baseline × threshold multiplier. | `odfBaseline_`, `pulseThresholdMult` |

## Model Terms

| Term | Definition | Notes |
|------|-----------|-------|
| **FrameOnsetNN** | The Conv1D W16 TFLite model that detects acoustic onsets from mel spectrograms. | 13.4 KB INT8, 6.8ms inference |
| **Onset activation** | The model's single output (0-1) per frame. Does NOT distinguish onset type (kick vs hi-hat). | `getLastOnset()` |
| **Major event** | A kick drum or strong snare — the type of onset that should drive visual effects. | Low-frequency transients (<200 Hz for kicks) |
| **Minor event** | A hi-hat, cymbal tick, ghost note — transients that should NOT drive visual effects at the current density target (2-4 major events per bar). | High-frequency transients (>4 kHz for hi-hats) |

## Metric Terms

| Term | Definition | Notes |
|------|-----------|-------|
| **Onset F1** | F-measure of detected onset peaks vs librosa onset labels (±70ms tolerance). Our primary quality metric. Reported per band: Kick (<200 Hz), Snare (200-4k Hz), HiHat (>4k Hz). | v1: 0.681 all, v3: 0.787 all |
| ~~**Beat F1**~~ | ~~F-measure of onset peaks vs metrical beat positions.~~ **DROPPED** — depends on music structure, not model quality. | Do not use |
| **Phase alignment %** | Fraction of visual triggers landing within tolerance of a beat grid subdivision. Measures how well the DSP+NN system tracks musical rhythm. | On-device sweep metric |
| **Onset density** | Events per second. Target for visual quality: 1-2 major events/sec (2-4 per bar at 120 BPM). | Measured via `onsetDensity` in AudioControl |

## Common Confusions

| Confusion | Resolution |
|-----------|-----------|
| "The model detects beats" | No — the model detects **onsets** (acoustic transients). It has zero metrical awareness. |
| "Pulse detection fired" | This means a **visual trigger** fired, not that a musical pulse was detected. |
| "Octave error" | BPM estimate is off by 2x (half-time or double-time). **Not a visual problem** — events still align with grid subdivisions. |
| "Hi-hat onset = false positive" | It's a **true positive** (a real onset was detected) but an **unwanted event** for visual purposes. The model correctly detected it; the filtering chain should suppress it. |
| "Beat-aligned onset" | Ambiguous. Means "an onset that happens to coincide with a metrical beat position." This depends on the music, not the model. |
