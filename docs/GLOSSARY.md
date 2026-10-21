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

Locations are in `blinky-things/audio/` (`AudioTracker.{h,cpp}`, `AudioControl.h`). For the full pipeline, see `docs/AUDIO_ARCHITECTURE.md`.

| Term | Definition | Firmware Location |
|------|-----------|-------------------|
| **ODF** | Onset Detection Function: the per-frame signal the onset detector peak-picks. On devices it is **spectral flux** (the onset NN is compiled out, `BLINKY_ONSET_NN_ENABLED=0`). NN builds peak-pick the NN activation (0-1) instead. | `updatePulseDetection(odf, ...)`, called with `spectral_.getSpectralFlux()` |
| **Onset** (output) | A discrete event: strength (0-1) of the onset fired during this update, 0 when none fired. One non-zero value per detected percussive event. | `AudioControl::onset` |
| **Pulse** (output) | A continuous envelope (0-1) for sparks and flashes. Rises when the normalized flux (or NN peak) passes the pulse signal threshold, then decays (240 ms time constant). Not a musical pulse. | `AudioControl::pulse` |
| **Visual trigger** | Informal name for an onset event reaching the generators (`onset > 0`, or a rise in `pulse`). How big it looks is decided in each generator. | `AudioControl::onset`, `AudioControl::pulse` |
| **Energy** | Continuous audio level (0-1) blending mic level, bass mel energy, and flux peak-hold. Drives background animation intensity. | `AudioControl::energy` |
| **Level** | Mic loudness auto-ranged to 0-1 by AdaptiveMic's window/range normalizer, with no gate. | `AudioControl::level` |
| **rhythmStrength** | Confidence that the audio has a periodic structure (0-1): ACF periodicity strength, faded to 0 a few seconds after the last onset. | `AudioControl::rhythmStrength` |
| **Phase** (rhythm) | Rhythm-cycle phase 0→1, 0 = accent. Advances at the detected period in absolute time and is pulled toward the measured accent. | `AudioControl::phase`, `AudioTracker::plpPhase_` |
| **beatCount** | Increments once per rhythm cycle (phase wrap), never decrements. Use it for "a beat happened", not phase thresholds. | `AudioControl::beatCount` |
| **plpPulse** | Repeating pattern shape at the detected period, from epoch folding (cosine fallback when no rhythm is detected). What generators consume as the "breathing curve". | `AudioControl::plpPulse`, `plpPulseValue_` |
| **Pulse signal threshold** | Hardcoded `0.4f` in `synthesizeOutputs()`: the normalized flux (or NN peak) must exceed it to raise the `pulse` envelope. | `synthesizeOutputs()` `pulseSignal > 0.4f` |
| **Baseline tracking** | Floor-tracking of the ODF on the spectral-flux path. An onset fires when flux crosses baseline × `pulseThresholdMult`, never below `fluxOnsetFloor`. | `odfBaseline_`, `pulseThresholdMult`, `fluxOnsetFloor` |

## Model Terms

| Term | Definition | Notes |
|------|-----------|-------|
| **FrameOnsetNN** | The Conv1D W16 TFLite model that detects acoustic onsets from mel spectrograms. **Off on devices** until a model beats spectral flux on edm/. | Embedded: v33, 19.6 KB INT8, ~12 ms per inference on the nRF52840 (`audio/frame_onset_model_data.h`) |
| **Onset activation** | The model's single output (0-1) per frame. Does NOT distinguish onset type (kick vs hi-hat). | `FrameOnsetNN::getLastOnset()` |
| **Major event** | A kick drum or strong snare. Usually drives the biggest visual response. | Low-frequency transients (<200 Hz for kicks) |
| **Minor event** | A hi-hat, cymbal tick, ghost note. Still an onset the detector should fire on; how much it shows is a visual decision in the generators (`docs/VISUALIZER_GOALS.md`). | High-frequency transients (>4 kHz for hi-hats) |

## Metric Terms

| Term | Definition | Notes |
|------|-----------|-------|
| **Onset F1** | F-measure of detected onsets vs the `.onsets_consensus.json` ground truth on `blinky-test-player/music/edm/`. Our primary quality metric. blinky-server reports it at several tolerances (±70 ms is the usual one). | Current device baseline (spectral flux): 0.542 on edm/ (`docs/AUDIO_ARCHITECTURE.md`) |
| ~~**Beat F1**~~ | ~~F-measure of onset peaks vs metrical beat positions.~~ **DROPPED** — depends on music structure, not detector quality. | Do not use |
| **Phase alignment %** | Fraction of visual triggers landing within tolerance of a beat grid subdivision. Measures how well the rhythm tracking follows the music. | On-device sweep metric |
| **Onset density** | Onsets per second, smoothed. Dance music is typically 2-6/s. | `AudioControl::onsetDensity` |

## Common Confusions

| Confusion | Resolution |
|-----------|-----------|
| "The model detects beats" | No — the detector (flux or NN) detects **onsets** (acoustic transients). It has zero metrical awareness. |
| "Pulse detection fired" | This means the `pulse` envelope rose or an `onset` event fired, not that a musical pulse was detected. |
| "Octave error" | BPM estimate is off by 2x (half-time or double-time). **Not a visual problem** — events still align with grid subdivisions. |
| "Hi-hat onset = false positive" | It's a **true positive**: a real onset was detected, and the detector should fire on it (CLAUDE.md: onset detection, not beat detection). Whether it drives a big visual is up to the generators. |
| "Beat-aligned onset" | Ambiguous. Means "an onset that happens to coincide with a metrical beat position." This depends on the music, not the detector. |
