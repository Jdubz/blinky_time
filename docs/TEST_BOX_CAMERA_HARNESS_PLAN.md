# Test box camera harness: plan

**Status:** planned (2026-10-27). Nothing is built yet. Camera facts below were measured on the box's Pi 5 (`blinkytest`) on 2026-10-27. Part of the test box (`docs/TEST_BOX_BUILD_PLAN.md`).

In the final box, every test chip drives a string of LEDs, and the camera sees all of them. This plan adds the camera to the validation harness, so a run judges what a person at a show sees, not only what the firmware reports over serial.

## What the camera answers

The serial stream already says when the firmware *detected* a hit (`TRANSIENT` lines, scored by `blinky_server/testing/scoring.py`). The camera adds three questions that serial can't answer:

1. **Does every detection light up?** (render fidelity) A detection the renderer drops, or a flash with no detection behind it, is invisible to serial scoring.
2. **How late is the light, and how steady is that delay?** (render latency and jitter) From detection to photons, and from the sound to photons.
3. **Does the light follow the music?** (audio-follow) Judged on the visuals alone, against the corpus's drum labels: kicks and snares (CLAUDE.md, "Task is Drum-Hit Detection"), not beats.

It also gives an independent check for visible freezes, which the release gate today infers only from `maxFrameMs` (#258).

## Principles

- **Distributions, not single events.** Every stage of the chain jitters: USB audio buffering, the device's frame loop, the LED write, the camera's frame clock. One event's latency means little. Every timing result is a distribution (median, IQR, p90) over hundreds of events, and every gate is on those statistics. A median over N events is far more precise than one event: with the camera's ±7.4 ms frame quantization (sd ≈ 4.3 ms), 300 events pin the median to about ±0.5 ms.
- **Tolerances that match the measurement.** Matching windows are wide enough for frame quantization plus the system's natural spread (set from baseline runs, below). Nothing is judged to the millisecond.
- **Design-agnostic.** The harness doesn't know LED counts, string shapes or generator internals. It finds the lit regions itself, reads brightness from them, and detects visual onsets the way the firmware detects audio onsets: rises in a signal, adaptive threshold, peak picking. Whatever must differ per generator lives in a small profile file, not in code.
- **Separate the stages.** Detection→light (renderer) and sound→detection (audio pipeline) are measured separately, then checked against the end-to-end sound→light number. A regression then points at its stage.
- **Measurement problems are loud and never scored** (CLAUDE.md, "No Silent Fallbacks"). Dropped frames, unlocked exposure, saturated LEDs, a stale region map: the run is marked INVALID with the reason, never quietly scored on bad data. INVALID (the measurement failed) is a different verdict from FAIL (the device failed).

## The camera

| | |
|---|---|
| Module | Raspberry Pi Camera Module v1 class: OmniVision **OV5647**, 5 MP, **rolling shutter**, fixed focus. On CSI (`rpicam-hello --list-cameras`). |
| Mode used | 640×480, 2×2 binned from the full 2560×1920 sensor (full field of view). The fastest mode libcamera lists for this sensor. |
| Measured frame rate | Asking for 60 fps gives 64.9 fps (15.40 ms); asking for 90 gives the cap, **67.5 fps (14.82 ms)**. Frame spacing sd ≈ 1 µs, so the camera's own clock is not a jitter source. |
| Field of view | About 54° × 41°. Across the box's 49 cm interior width it needs about 48 cm of distance, which fits the 66 cm long axis; check framing at build time. A wide-angle OV5647 module is the fallback. |
| Focus | Fixed. Soft focus is harmless: the harness sums brightness over regions; strings just must not overlap in the image. |
| Software | `python3-picamera2` (apt; not installed yet). Per-frame metadata gives the sensor timestamp, exposure, gain and frame sequence number. |

**Rolling shutter.** Each image row starts its exposure a fixed line time after the previous one, so the top and bottom of a frame are exposed several ms apart. A region's timestamp is the frame timestamp plus (its row centroid × the line time). The line time is measured, not taken from a datasheet (Calibration, step 2).

A global-shutter camera (Raspberry Pi Global Shutter Camera) would remove the row correction and the banding concerns below, but nothing in this plan requires it. Revisit if the reference-LED calibration shows the OV5647 can't meet the timing gates.

## Timing model

One run has four clocks. All timing is converted to the host's monotonic clock:

| Clock | Source | Mapped to host monotonic by |
|---|---|---|
| Host | `time.monotonic_ns()` and `time.time()` | (time.time and monotonic) pairs recorded at run start and end |
| Audio | when sound actually leaves the speaker | the existing per-run sync clicks (`tools/device_eval.py`): the device's detections of three irregular clicks before the music give the run's true audio lag |
| Device | firmware `millis()` in `TRANSIENT.timestampMs` | the existing clock sync (`test_runner._sync_clock` → `TestSession.set_clock_offset`), its error recorded per run |
| Camera | picamera2 sensor timestamp, per frame, plus the row offset | the reference LED (Calibration, step 1), which also confirms which kernel clock the timestamp is on |

The chain and the sources of spread, to be confirmed by measurement:

```
 sound → mic → detection (fw millis) → next render frame → LED write → light → camera frame
         |---- audio latency ------|  |------ render latency ------------|   |- capture -|
         scored by serial today        5-10 ms frame loop + ~3 ms per        ±7.4 ms frame
         (+ USB audio buffering)       100 LEDs (WS2812 at 30 µs/LED)        quantization
```

Expected detection→light: roughly 3–15 ms of render plus the camera's quantization. The baseline runs replace these estimates with measured distributions.

**Sub-frame timing (optional).** With exposure close to the frame period, a light that comes on part-way through a frame shows at partial brightness. The fraction gives an onset time inside the frame. That only works for pulses that hold their brightness for a frame or two, and the fleet's hit flashes decay fast. Treat it as an experiment, validated against the reference LED before any gate uses it. The baseline method is frame-quantized timing with statistics.

## How it works

### 1. Region map (adapts to layout changes)

The harness finds the LEDs instead of being told where they are:

1. A calibration run plays a corpus track with the lid closed. The harness computes each pixel's brightness variance over time. LEDs vary and everything else is dark (matte black interior, no ambient light).
2. Threshold the variance map and take connected components: one or more blobs per LED string.
3. **Assign blobs to chips by behavior, not position:** correlate each blob's brightness series with each chip's serial `TRANSIENT` train. A blob belongs to the chip whose detections it follows. That needs no device commands and no resets (60-second rule), and it keeps working when chips move or strings change.
4. Optional segments: split each chip's region along the string's main axis into K bins (default K=1, the whole string). Generators with spatial effects (e.g. hit height) can use them.
5. Save `roi_map.json`: chip ID → pixel masks, row centroids, segments, plus the camera settings it was made with.

**Every run checks the map still fits:** light outside every region, a region that stays dark while its chip reports detections, or different camera settings make the run INVALID with "layout changed: re-run the region calibration". So a changed string or a bumped camera can't produce wrong numbers, only a clear request to recalibrate.

### 2. Capture

A helper process, `tools/camera_capture.py`, runs on the system Python, which has `python3-picamera2`. blinky-server's venv doesn't have it, and the process boundary keeps camera work out of the server's event loop. blinky-server starts and stops it for each run, next to `ffplay`.

- Pinned to a core away from `ffplay` and the server (`taskset`), so capture load doesn't move the audio timing (host-timing gate).
- Fixed controls: frame duration, exposure, analog gain, AWB gains. AE, AWB and any denoise that blends frames are off. Each frame's metadata is checked against the locked values; a mismatch makes the run INVALID.
- Per frame it records the sequence number, sensor timestamp, and per region: summed luminance, mean R/G/B, saturated-pixel count, and per-segment luminance. It does **not** store raw video (4 minutes of 640×480 YUV is about 6.6 GB). For human review it can save a low-rate MJPEG preview and a short clip around any event the scorer flags.
- Output goes next to the run's other test data (one `.npz` per run).

### 3. Visual onsets

For each chip's region, the same recipe as audio onset detection (a published approach; see `docs/ONSET_DETECTION_LITERATURE_2026_05_02.md`):

- Signal: summed luminance, background-subtracted (the region's level with the LEDs dark, taken at run start). A generator profile may name a color channel or segment instead.
- Visual flux: the half-wave rectified frame-to-frame rise, normalized by the region's running range.
- Peak picking: adaptive threshold (median + k·MAD over a ~1 s window) and a minimum gap between onsets of 50 ms. The onset's time is the first frame of the rise, with the row offset applied.

### 4. Measurements

Per chip, per run:

| Measurement | How | Reported |
|---|---|---|
| Render fidelity | Match serial detections to visual onsets in a window from −15 ms (quantization) to +60 ms | share of detections that lit up; share of visual onsets with no detection |
| Render latency | visual onset − detection, for matched pairs | median, IQR, p90, histogram |
| Sound→light latency | visual onset − labeled drum onset (corpus `kick_weighted` / `onsets_consensus`), using the run's click-measured audio lag | median, IQR, p90 |
| Visual drum accuracy | visual onsets scored against kick/snare labels after removing the run's median sound→light lag, at ±50 and ±70 ms | kick/snare recall; share of visual onsets on a kick or snare |
| Follow strength | cross-correlate the region's flux with the labeled drum-onset train (Gaussian σ = 30 ms) over lags 0–300 ms; compare the peak with the same correlation against **each other corpus track's** labels (the null) | peak lag; peak height; margin over the highest null |
| Consistency | the follow peak lag must equal the audio lag plus the render latency, within the IQRs | pass/fail plus the difference |
| Freezes | runs of frames where the region doesn't change while the generator profile says it animates | count and longest, in ms |

**Why the other-track null.** Music is periodic, so the correlation with a time-shifted copy of the same track's labels is high at every beat multiple. Correlating against the other 17 tracks' labels gives correlations that are "musical but unrelated". Clearing all of them shows the light follows *this* track's drums rather than just pulsing at some tempo. The test makes no assumption about how the generator draws a hit.

**Visual vs serial.** The device's serial F1 is already known for the same run. Visual drum recall should be within a small margin of serial recall: the gap is what the renderer loses. A visual result is always reported next to the serial one, never alone.

### 5. Generator profiles

A small file per generator, `tests/camera/profiles/<generator>.json`. Nothing about drawing is in code.

- **diagnostic** (the `Audio` generator: green transient rows, decays over ~6 frames): strict per-event checks: render fidelity, render latency, orphan flashes.
- **production** (the fleet's default generator on linear strings, e.g. Fire's per-hit glow, #280): statistical checks only: follow strength, visual drum accuracy, freezes. Its background flicker would make per-event orphan counts meaningless.

A profile sets the signal (luminance, a channel or segments), the peak-picking constants, whether the generator animates continuously (for the freeze check), and which measurements are gated. A new generator or LED design needs a new profile and a baseline, not new harness code.

The test run selects a chip's generator through the validation job and never saves it, like the run's existing reset to defaults. No ad-hoc config changes (CLAUDE.md, "Device Workflow Discipline").

## Calibration

Repeat whenever the camera, the strings or the chip positions change. It is part of the box calibration (`TEST_BOX_BUILD_PLAN.md`, Calibration).

1. **Reference LED.** One LED driven by a Pi 5 GPIO (through a resistor), mounted in the camera's view, ideally two: one near the top of the frame and one near the bottom. The harness toggles it at known host-monotonic times. Several hundred toggles give:
   - the camera timestamp's offset from host monotonic, and which clock it is on;
   - the timing error distribution the camera alone contributes (the floor under every other number);
   - with LEDs at two heights, the rolling-shutter line time.
   A few toggles at the start of every run re-check the offset. If it drifted beyond tolerance, the run is INVALID.
2. **Rolling-shutter line time**, from step 1, stored with the camera settings.
3. **LED PWM.** WS2812-class LEDs dim with PWM at a few hundred Hz to a few kHz. A short exposure makes that PWM show as brightness ripple, and with a rolling shutter as bands. Measure the PWM period once (with a short exposure, the band spacing in rows × line time gives it). Then set the exposure to the largest whole number of PWM periods that fits in the frame period, which averages the PWM out.
4. **Levels.** Set LED brightness for test runs and the analog gain so the brightest flash stays below ~80% of full scale. Saturated pixels hide the rise and the brightness. A region with saturated pixels makes the run INVALID.
5. **Region map** (How it works, step 1).
6. **Baseline.** Current stable firmware, every chip, both profiles, 5 runs on the `edm/` corpus. The thresholds below come from these runs.

## Gates (provisional)

Thresholds are set from the baseline, then frozen in the profile files. The shapes:

| Gate | Pass condition (provisional) |
|---|---|
| Camera timing | Reference LED: camera timing error IQR ≤ one frame period; offset stable within ±2 ms across the run. |
| Render fidelity (diagnostic) | ≥ 95% of serial detections light up; ≤ 5% of visual onsets lack a detection. |
| Render latency (diagnostic) | median within baseline ± 5 ms; p90 ≤ 50 ms. |
| Follow (both profiles) | the follow peak beats every other-track null on ≥ 16 of 18 tracks; peak lag consistent with audio lag + render latency. |
| Visual drum accuracy (both) | kick/snare recall within 0.05 of the same run's serial recall. |
| Freezes (production) | none longer than 100 ms while music plays (matching the release gate's `maxFrameMs` limit). |
| Repeatability | 5 runs, same firmware: medians vary ≤ 5 ms; follow margin and visual recall vary ≤ 0.03. |

Verdicts per chip per run: **PASS**, **MARGINAL** (inside the gate but outside the baseline's spread: a person looks at the flagged clips), **FAIL**, **INVALID** (measurement problem, with the reason).

## Code shape

- `tools/camera_capture.py`: capture helper (system Python, picamera2), described above.
- `blinky_server/testing/camera_capture.py`: starts and stops the helper around a run, records the clock pairs, loads its output.
- `blinky_server/testing/camera_scoring.py`: pure functions on arrays (region signals, visual onsets, matching, follow test). Unit-tested in CI with synthetic frame series, so `blinky-server-tests` covers it with no camera.
- `tools/camera_calibrate.py`: reference-LED timing, PWM, levels, region map, baseline.
- `DeviceRunScore` gets an optional `visual` block, and the console shows it next to the serial scores.
- `setup.sh` installs `python3-picamera2`, and adds the user to the `video` group (already done) and to GPIO access for the reference LED.

## Rollout

1. **Bench.** Camera, one chip with a LED string, the reference LED, on the open bench. Install picamera2. Measure the camera timing, line time and PWM. Do the numbers support the plan, or is a global-shutter camera needed?
2. **Offline first.** Capture alongside normal validation runs, score afterwards, compare with serial. Nothing gates yet.
3. **Baseline** on the current stable release; set and freeze the thresholds.
4. **Into the validation job:** the visual block in every box validation run.
5. **Into the release gate** (with the gate's move to the box, `TEST_BOX_BUILD_PLAN.md`, Rollout step 4): visual fidelity, follow and freeze checks join the gate's existing ones. That closes the camera part of #258.

## Open questions

- Can the OV5647 run faster in a cropped mode, or a newer libcamera tuning? Faster frames shrink quantization directly. 67.5 fps is the measured ceiling at 640×480 today.
- Is the timestamp from picamera2 exposure start or readout? The reference LED answers this; the plan assumes nothing.
- One camera for all chips, or one per shelf if strings don't fit one frame without overlapping?
- Should the PLP phase visuals (the `Audio` generator's moving phase row) be checked against the wall clock here, for #258's remaining "phase keeps pace" item?
