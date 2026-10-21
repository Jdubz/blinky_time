# Blinky Time - Improvement Plan

*Last updated: 2026-10-21 (stable release b215, SETTINGS_VERSION 98)*

This is the status and roadmap doc. **GitHub issues are the system of record**; `docs/OPEN_ISSUES_2026_10_15.md` is the index of open issues by area. This page says where the system stands and what is left to do. The model-training roadmap lives in `docs/ML_IMPROVEMENT_PLAN.md`.

The history of how we got here (model versions v3–v36, CBSS, Bayesian tempo fusion, the PLL, sweeps) is in git history for this file. The short list of things that were tried and failed is kept below under "Don't retry", so nobody repeats them.

## Current Status (verified against code, 2026-10-21)

**Fleet:** nRF52840 devices only (ESP32-S3 support was cut March 2026; next-gen fixtures: `docs/NEXTGEN_ESP32S3_PLAN.md`). Latest stable release is **b215** (2026-10-20). Fleet and new devices get stable releases only (`docs/RELEASE_PROCESS.md`). Bootloader 0.8.0-10 (see CLAUDE.md).

**Audio (`blinky-things/audio/AudioTracker.*`, details in `docs/AUDIO_ARCHITECTURE.md`):**
- **Onsets come from spectral flux. The onset NN is off on devices** (`BLINKY_ONSET_NN_ENABLED=0` in `AudioTracker.h`). On edm/ with the host harness CPU cost model (`tests/tracker/`), flux scores F1 0.546 at 178 fps; the v33 NN scores 0.480 and holds the loop at 48 fps (~12 ms per inference). The NN goes back on only when a model beats flux under that same measurement.
- Flux onset detector: adaptive baseline × `pulsethreshmult` (2.0), floor `fluxonsetfloor` (0.01), rising edge, tempo-adaptive cooldown (40–150 ms).
- **No noise gate and no silence detection** (removed; SETTINGS_VERSION 98 drops `noiseGate`). The rhythm outputs (`rhythmStrength`, `plpPulse`) fade out 2–4 s after the last onset instead.
- **Rhythm:** ACF on three sources (bass flux, bass mel energy, mic level; the NN is not a rhythm source, #155), beat lags 20–80 frames, bar candidates at 1–4× plus syncopation-derived candidates (#166), tempo prior. ACF + PLP epoch-fold every 100 ms (`acfPeriodMs`). Fractional period. Pattern slot cache (4 slots).
- **Frame clock:** every rhythm step runs once per 62.5 Hz spectral frame.
- **Absolute time (PR #268, closes #259, #260, #261):** the PLP phase advances on the mic's sample clock, so dropped audio doesn't put the beat behind (`audio/PhaseClock.h`). `phase` and `plpPulse` are extrapolated to the moment of drawing. Animations use real frame time (`types/FrameClock.h`); physics runs in substeps of at most 50 ms over at most the last 1 s.

**Visuals (`blinky-things/generators/`, details in `docs/GENERATOR_EFFECT_ARCHITECTURE.md`):** Fire (per-column heightmap flame plus particle bursts on onsets), Water (rain particles, ripple simulation, bioluminescence, depth gradient), PlasmaGlobe (replaced Lightning in v94; selected with `gen lightning` or `gen plasma`), Audio (diagnostic visualizer). One effect: hue rotation. `scene` sets generator, effect, speed and hue in one command.

**Updates:** signed QSPI-staged OTA (`blinky-things/ota/`, Ed25519) and the browser firmware page (USB) are shipped; see `docs/WEB_FLASHER.md`. Fleet flashing goes through `./scripts/deploy.sh` only.

**Test tooling:** `tests/tracker/` runs the real `AudioTracker` on edm/ audio at simulated loop rates, with a CPU cost model (in CI, #202). blinky-server owns all device connections and runs validation (`docs/TESTING_METHODOLOGY.md`).

## Outstanding Work

Grouped by area. Status words follow the open-issues index: OPEN (not done), PARTIAL (part done), VERIFY (done in code, needs a device check).

### Device verification (do first)
- **#161 (VERIFY):** run the PR #152 checklist on the hat (§7 of the open-issues index). Observe the 60-second rule.
- **#160 (OPEN):** read the hat's link type and bootloader version live before flashing.
- **#164 (VERIFY):** frame-clocked rhythm tracking on a device.
- **#172 (VERIFY):** breakdowns no longer reset tempo.
- **#177, #184 (VERIFY):** AdaptiveMic valley/drop fixes; mic-overrun discontinuity handling.
- **#185 (VERIFY):** `BANDWIDTH_MAX` SoftDevice config boots with BLE on the hat.
- **#192 (VERIFY):** generator fixes (onset/beatCount events, pool exhaustion, time scaling) on the hat's linear layout.

### Rhythm tracking
- **#166 (PARTIAL):** wrong tempo multiple on syncopated content. Remaining: reggaeton (3+3+2), dnb-energetic, breakbeat-background.
- **#167 (OPEN):** phase lock near the noise floor on dubstep / garage / breakbeat / trap.
- **#168 (OPEN):** 174 BPM tracked as a 2-beat cycle. Fine for visuals; revisit with #166.

### Onset detection and the NN
- **#174 (OPEN, ML):** the NN computes 16 output frames to use 1. A W=9 or streaming export halves inference cost. This is the prerequisite for re-enabling the NN.
- **#165 (PARTIAL):** with the NN on and the loop below 62.5 fps, inference runs only on the newest frame of a catch-up burst.
- Training roadmap (label coverage, capacity, eval ceiling): `docs/ML_IMPROVEMENT_PLAN.md`. Labels must be onset labels, scored on edm/ only (CLAUDE.md).

### Spectral / mic
- **#175 (PARTIAL):** raw mel saturates at high SPL. A fix must be mirrored in training.
- **#178 (OPEN, ML):** non-overlapping frames make onset strength wobble with hit position; overlap needs a retrain.
- **#180 (OPEN, low):** mel whitening gives little contrast.
- **#181 (OPEN):** noise estimator creeps upward in steady music (off by default).

### Frame rate and loop stalls
- **#258 (PARTIAL):** release gate fps floor is now 60; still to do: gate on timing (longest frame, phase vs wall clock, overruns).
- **#171 (OPEN):** JSON stream formatting stalls the loop 10–50 ms per emit.
- **#191 (PARTIAL):** settings save still halts the CPU long enough to drop audio.
- **#193 (OPEN):** LED `show()` DMA wait caps fps on large strips.

### BLE and device safety
- **#173 (OPEN):** NUS commands run in the BLE callback task, concurrently with loop-task output. Queue them to the loop task.
- **#189 (OPEN):** NUS output can block the loop up to 100 ms per chunk during record sessions.
- **#190 (OPEN):** measure fleet-scanner reception while a central is connected.
- **#251 (PARTIAL):** server auto-recovery must use an immutable, stable-only firmware image.

### Phone record mode (`docs/PHONE_RECORD_MODE_PLAN.md`)
- **#243 (OPEN):** re-scope as live calibration-data capture, not audio recording.
- **#195, #196 (OPEN):** end-to-end test on Android Chrome; screen-lock / background behaviour.
- **#198 (VERIFY):** Copilot-review fixes on a phone.

### Fleet operations (carried over from 2026-05-20)
- **#209:** bake the BCM 18 reset-line hold into the swd-flash Pi's `recover.sh`.
- **#210:** finish 06ACEB recovery.
- **#211:** `deploy-bootloader.sh` false-negative exit 4 on the headless already-in-DFU path.
- **#212:** blinky-server re-acquires the serial port after a BL flash.
- **#215:** one clean all-BLE `deploy.sh` run to exit 0.
- **#216:** BLE-DFU negotiates MTU 23 (~6.5 min per device).
- **#217:** lemon-cart `install.sh` deploy.
- **#219:** confirm server auto-recovery on an enrolled fleet device.
- **#220:** bootloader self-update over BLE.
- **#221:** measure gossip-ACK command delivery across the fleet at range.

## Don't Retry

Each of these was built and measured, and lost or broke something. Don't re-run them without a new reason.

**Onset detection / NN**
- Mel-spectrogram Conv2D CNNs (v4–v9): 79–98 ms per inference on the nRF52840. Far over the frame budget.
- Running the NN every other frame: alternating frame latency makes LED animation jerk.
- PCEN input (v18): onset AUC 0.506, i.e. chance. It normalizes away onset contrast.
- Delta features (v15) and band-flux input (v17): zero on-device benefit, extra cost.
- SpecMix / CutMix, SWA, OWBCE proximity loss, global mixup (v12–v14): no gain, or unsound for frame-level labels.
- Beat-tracker labels (`consensus_v*`, beat_this, allin1, ...) and beat-tracker distillation: they teach the model to skip off-beat onsets. The task is onset detection (CLAUDE.md).
- The NN as a gate on spectral flux: the NN as the primary signal was better when the NN was on.
- PLP beat-grid AND-gate on onsets (`beatgridmin`, b142): suppressed 65–84% of true onsets. Default is 0 (off).
- An absolute noise gate, or level-based silence detection: on this weak mic they swallowed quiet music or reset tempo in breakdowns (removed twice; v98).
- "Wider models don't help": only tested below the published parameter range. See `docs/ML_IMPROVEMENT_PLAN.md` before treating this as settled.

**Rhythm tracking**
- PLL phase tracking (phase consistency ~0.04, i.e. random), CBSS (removed v75), Bayesian tempo fusion, comb filter bank, Percival harmonic enhancement, Rayleigh prior (removed v80).
- Forward filter, particle filter, HMM phase tracker, multi-agent, template/sub-beat checks (removed v64).
- The NN activation as an ACF source (#155): locked straight 4/4 to 2/3 tempo and broke DnB.
- Band-specific (mid/high flux) epoch-fold: regressed 8–10 of 18 tracks.
- Duple-support scoring for syncopated tempo (#166): did not help.

## Visualizer Roadmap

Ideas not yet built, by generator. Design rules are in `docs/VISUALIZER_GOALS.md`; photosensitivity rules in `docs/VISUALIZATION_SAFETY_TESTING.md`. Generators can use only the signals in `AudioControl`: `energy`, `level`, `pulse`, `onset`, `phase`, `beatCount`, `plpPulse`, `rhythmStrength`, `onsetDensity`. (There are no `downbeat` or `beatInMeasure` signals any more; use `beatCount` for beat-synced events.) All scaling must come from `width_`, `height_`, `numLeds_`, `traversalDim_`, `crossDim_`; no device-specific constants.

### Fire

Fire is now a per-column heightmap flame (b202) with particle bursts on onsets, curl-noise wind and a heat grid. Done earlier: dynamic particle pool, domain-warped noise background, two-palette blend, audio-driven gamma, curl-noise wind, energy-driven flame height.

Not built (the particle ideas need re-checking against the heightmap design):
1. **Spawn-on-death embers:** when a bright spark dies, spawn 1–2 dim, slow, longer-lived embers. Cap child spawns per frame so bursts are never starved.
2. **Ember pulsing:** slow embers pulse in brightness instead of fading linearly (`0.7 + 0.3 * sin(age * freq + seed)`, seed from position).
3. **Reaction-diffusion flame base (FitzHugh-Nagumo):** a 1D system along the base row shapes the flame tongues; `energy` sets the reaction rate, `onset` injects activation.
4. **Vortex filaments:** two counter-rotating vortices at the base give S-curve flames.

### Plasma Globe — DONE (v94)

`PlasmaGlobe` replaced the Lightning generator: 2–4 violet orbs drifting by noise over a very dim background, breathing with the rhythm and flashing on pulses (`p_*` settings). No open follow-ups.

### Water

Done: ripple simulation (two-buffer), depth gradient (matrix), bioluminescence on onsets, phase-driven flow speed.

Not built:
1. **Gerstner wave surface (matrix):** 3 summed trochoidal waves give a rolling surface with sharp crests. `energy` → amplitude.
2. **Foam at crests:** blend toward white where the surface is steep; slow decay.
3. **Onset density → rain character:** sparse music, gentle rain; dense music, heavy rain and more ripples.
4. **Caustic overlay (matrix):** animated Voronoi or `abs(n1 + n2)^0.5` light pattern.
5. **Multi-palette colour:** deep ocean / tropical / storm palettes blended by `energy × rhythmStrength` (Inigo Quilez cosine palettes).
6. **Drop trails:** draw falling drops 2–3 LEDs long, fading along the velocity.
7. **Swell accumulator:** each beat adds to a decaying swell that raises wave height and brightness.

Key principle: water responds through amplitude and speed (bigger waves, faster flow), not flashes.

### References
- [Bridson SIGGRAPH 2007: Curl-Noise for Procedural Fluid Flow](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf)
- [Stefan Petrick: Self-Modulating Noise Fire Effect](https://gist.github.com/StefanPetrick/819e873492f344ebebac5bcd2fdd8aa8)
- [Inigo Quilez: Domain Warping](https://iquilezles.org/articles/warp/) and [Cosine Palettes](https://iquilezles.org/articles/palettes/)
- [Demoscene 2D Water Effect (Hugo Elias)](https://web.archive.org/web/20160418004149/http://freespace.virgin.net/hugo.elias/graphics/x_water.htm)
- [Catlike Coding: Waves](https://catlikecoding.com/unity/tutorials/flow/waves/) (Gerstner)
- [The Book of Shaders: Voronoi](https://thebookofshaders.com/12/) (caustics)

## Design Philosophy

See **[VISUALIZER_GOALS.md](VISUALIZER_GOALS.md)**. Visual quality comes before metrics. False positives are the worst visual problem. Low F1 on ambient content can be correct behaviour (organic mode).

## Related Documents

| Document | Purpose |
|----------|---------|
| `docs/OPEN_ISSUES_2026_10_15.md` | Index of open GitHub issues by area |
| `docs/ML_IMPROVEMENT_PLAN.md` | NN training roadmap |
| `docs/AUDIO_ARCHITECTURE.md` | AudioTracker: spectral flux onsets, ACF + PLP, frame clock |
| `docs/AUDIO-TUNING-GUIDE.md` | Parameter reference and test procedures |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator / Effect / Renderer pattern |
| `docs/VISUALIZER_GOALS.md` | Design philosophy |
| `docs/RELEASE_PROCESS.md` | Stable / candidate / dev releases |
| `docs/WEB_FLASHER.md` | Signed QSPI OTA and the browser firmware page |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Calibration history |
