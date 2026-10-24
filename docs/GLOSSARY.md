# Blinky Time Glossary

Terminology is a recurring source of design confusion in this project. This glossary defines terms precisely to prevent circular reasoning from overloaded words.

## Musical Terms

| Term | Definition | Example |
|------|-----------|---------|
| **Beat** | A metrical grid position. Abstract, periodic, exists even during silence. | Beat 1 of a 4/4 bar, whether or not there's a kick |
| **Pulse** (musical) | The regularly recurring unit that defines tempo. In 4/4 time, the pulse is the quarter note. | "The pulse is 120 BPM" = 120 quarter notes per minute |
| **Onset** | An acoustic transient — the start of a percussive sound. Concrete, irregular, may or may not align with a beat. | A kick drum hit, a snare crack, a hi-hat tick |
| **Drum hit** | A kick or snare onset: **the detector's target** (CLAUDE.md). Hi-hats, cymbals and non-percussive changes are not targets. Metrical position doesn't decide it: an off-beat kick counts. | A syncopated kick, a snare on 2 |
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
| **ODF** | Onset Detection Function: the per-frame signal the onset detector peak-picks. On devices it is **SuperFlux** (Böck & Widmer 2013; the onset NN is compiled out, `BLINKY_ONSET_NN_ENABLED=0`). NN builds peak-pick the NN activation (0-1) instead. | `OnsetDetector::process()` → `OnsetEvidence.odf`; flux: `spectral_.getSuperFlux()` |
| **Online peak picker** | Böck, Krebs & Schedl (ISMIR 2012), shared by every detector: a frame is an onset if it is the max of the last W1 frames, reaches their mean + δ, and more than W5 frames passed since the last onset. No look-ahead, no other gates. | `AudioTracker::pickOnset`; `onsetmaxframes`, `fluxavgframes`, `onsetmingap` |
| **δ (delta)** | The picker's threshold above the moving mean: `fluxdelta` (7) for flux, `pulseonsetfloor` (0.55, fixed threshold, no mean) for the NN. | `OnsetDetector::delta()` |
| **Onset** (output) | A discrete event: strength (0-1) of the onset fired during this update, 0 when none fired. One non-zero value per detected percussive event. | `AudioControl::onset` |
| **Pulse** (output) | A continuous envelope (0-1) for sparks and flashes. Each picked onset raises it to that onset's strength; it then decays with a 150 ms time constant (`pulsedecay`). Not a musical pulse. | `AudioControl::pulse`, `pulseDecayMs` |
| **Visual trigger** | Informal name for an onset event reaching the generators (`onset > 0`, or a rise in `pulse`). How big it looks is decided in each generator. | `AudioControl::onset`, `AudioControl::pulse` |
| **Energy** | Continuous audio level (0-1) blending mic level, bass mel energy, and flux peak-hold. Drives background animation intensity. | `AudioControl::energy` |
| **Level** | Mic loudness auto-ranged to 0-1 by AdaptiveMic's window/range normalizer, with no gate. | `AudioControl::level` |
| **rhythmStrength** | Confidence that the audio has a periodic structure (0-1): ACF periodicity strength, faded to 0 a few seconds after the last onset. | `AudioControl::rhythmStrength` |
| **Phase** (rhythm) | Rhythm-cycle phase 0→1, 0 = accent. Advances at the detected period in absolute time and is pulled toward the measured accent. | `AudioControl::phase`, `AudioTracker::plpPhase_` |
| **beatCount** | Increments once per rhythm cycle (phase wrap), never decrements. Use it for "a beat happened", not phase thresholds. | `AudioControl::beatCount` |
| **plpPulse** | Repeating pattern shape at the detected period, from epoch folding (cosine fallback when no rhythm is detected). What generators consume as the "breathing curve". | `AudioControl::plpPulse`, `plpPulseValue_` |
| ~~**Baseline tracking**~~ | ~~Floor-tracking of the flux ODF × `pulseThresholdMult`.~~ **Deleted in #292**: it settled at the noise floor and fired on steady noise. The picker's moving mean + δ replaced it. | — |

## Model Terms

| Term | Definition | Notes |
|------|-----------|-------|
| **FrameOnsetNN** | The Conv1D W16 TFLite model that detects acoustic onsets from mel spectrograms. **Off on devices** until a model beats spectral flux on edm/. | Embedded: v33, 19.6 KB INT8, ~12 ms per inference on the nRF52840 (`audio/frame_onset_model_data.h`) |
| **Onset activation** | The model's single output (0-1) per frame. Does NOT distinguish onset type (kick vs hi-hat). | `FrameOnsetNN::getLastOnset()` |
| **Major event** | A kick drum or strong snare. Usually drives the biggest visual response. | Low-frequency transients (<200 Hz for kicks) |
| **Minor event** | A hi-hat, cymbal tick, ghost note. **Not a target:** a detection on a hat is a false positive (CLAUDE.md, 2026-10-22). | High-frequency transients (>4 kHz for hi-hats) |

## Metric Terms

| Term | Definition | Notes |
|------|-----------|-------|
| **Drum metrics** | **The primary quality metrics.** Kick and snare recall at ±30/±50 ms, the share of detections that land on a kick or snare (on-drum), and drum F, against cleaned `kick_weighted` labels, with edm/ played at the mic's real level (−50 dB). | SuperFlux ≤ 5 kHz: drum F 0.657, kick R 79%, on-drum 56% (`docs/AUDIO_ARCHITECTURE.md`) |
| **Onset F1** | F-measure of all detected onsets vs `.onsets_consensus.json` on edm/. **Secondary** since the drum-hit framing: it scored 0.68 while the hat looked almost random (#280). blinky-server still reports it. | Old picker: 0.546 on edm/ |
| ~~**Beat F1**~~ | ~~F-measure of onset peaks vs metrical beat positions.~~ **DROPPED** — depends on music structure, not detector quality. | Do not use |
| **Phase alignment %** | Fraction of visual triggers landing within tolerance of a beat grid subdivision. Measures how well the rhythm tracking follows the music. | On-device sweep metric |
| **Onset density** | Onsets per second, smoothed. Dance music is typically 2-6/s. | `AudioControl::onsetDensity` |

## Common Confusions

| Confusion | Resolution |
|-----------|-----------|
| "The model detects beats" | No — the detector (flux or NN) detects **drum hits** (kick and snare onsets). It has zero metrical awareness, apart from the optional rhythm-pattern bias on δ (`onsetpatternbias`). |
| "Pulse detection fired" | This means the `pulse` envelope rose or an `onset` event fired, not that a musical pulse was detected. |
| "Octave error" | BPM estimate is off by 2x (half-time or double-time). **Not a visual problem** — events still align with grid subdivisions. |
| "Hi-hat onset = false positive" | **Yes, since 2026-10-22.** The detector targets kicks and snares (CLAUDE.md: drum-hit detection, not beat detection), so a detection on a lone hat is a false positive. Off-beat *kicks and snares* are still targets: metrical position never decides it. |
| "Beat-aligned onset" | Ambiguous. Means "an onset that happens to coincide with a metrical beat position." This depends on the music, not the detector. |
