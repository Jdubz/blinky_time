# Test box camera harness: plan

**Status:** rollout step 1 under way (2026-11-24). Built: the capture helper, region discovery and visual onsets (`scripts/testbox/camera/`), with bench numbers below. Not built: running capture alongside validation runs, matching against serial detections, scoring, gates. Camera facts were measured on the box's Pi 5 (`blinkytest`) on 2026-10-27 and 2026-11-24. Part of the test box (`TEST_BOX.md`).

The box has one test chip driving an 8x8 WS2812 grid, and the camera sees the grid (hardware: `TEST_BOX.md`, "Hardware as built"). This plan adds the camera to the validation harness, so a run judges what a person at a show sees, not only what the firmware reports over serial.

## What the camera answers

The serial stream already says when the firmware *detected* a hit (`TRANSIENT` lines, scored by `blinky_scoring` in the `corpus/` submodule, blinky-corpus). The camera adds three questions that serial can't answer:

1. **Does every detection light up?** (render fidelity) A detection the renderer drops, or a flash with no detection behind it, is invisible to serial scoring.
2. **How late is the light, and how steady is that delay?** (render latency and jitter) From detection to photons, and from the sound to photons.
3. **Does the light follow the music?** (audio-follow) Judged on the visuals alone, against the corpus's drum labels: kicks and snares (CLAUDE.md, "Task is Drum-Hit Detection"), not beats.

It also gives an independent check for visible freezes, which the staging suite today infers only from `maxFrameMs` (the longest loop frame in `json info`).

## Principles

- **Distributions, not single events.** Every stage of the chain jitters: USB audio buffering, the device's frame loop, the LED write, the camera's frame clock. One event's latency means little. Every timing result is a distribution (median, IQR, p90) over hundreds of events, and every gate is on those statistics. A median over N events is far more precise than one event: with the camera's ±7.4 ms frame quantization (sd ≈ 4.3 ms), 300 events pin the median to about ±0.5 ms.
- **Tolerances that match the measurement.** Matching windows are wide enough for frame quantization plus the system's natural spread (set from baseline runs, below). Nothing is judged to the millisecond.
- **Design-agnostic.** The harness doesn't depend on LED counts, layouts or generator internals. It finds the lit regions itself, reads brightness from them, and detects visual onsets the way the firmware detects audio onsets: rises in a signal, adaptive threshold, peak picking. Whatever must differ per generator lives in a small profile file, not in code.
- **Separate the stages.** Detection→light (renderer) and sound→detection (audio pipeline) are measured separately, then checked against the end-to-end sound→light number. A regression then points at its stage.
- **Measurement problems are loud and never scored** (CLAUDE.md, "No Silent Fallbacks"). Dropped frames, unlocked exposure, saturated LEDs, a stale region map: the run is marked INVALID with the reason, never quietly scored on bad data. INVALID (the measurement failed) is a different verdict from FAIL (the device failed).

## The camera

| | |
|---|---|
| Module | Raspberry Pi Camera Module v1 class: OmniVision **OV5647**, 5 MP, **rolling shutter**, fixed focus. On CSI (`rpicam-hello --list-cameras`). |
| Mode used | 640×480, 2×2 binned from the full 2560×1920 sensor (full field of view). The fastest mode libcamera lists for this sensor. |
| Measured frame rate | Asking for 60 fps gives 64.9 fps (15.40 ms); asking for 90 gives the cap, **67.5 fps (14.82 ms)**. Frame spacing sd ≈ 1 µs, so the camera's own clock is not a jitter source. |
| Field of view | About 54° × 41°. Check that the whole grid is in frame at the camera's mounting distance. |
| Focus | Fixed. Soft focus is harmless: the harness sums brightness over regions. |
| Software | `python3-picamera2` (apt, installed by `setup.sh`). Per-frame metadata gives the sensor timestamp, exposure and gain. |
| Line time | About **15.87 µs**: requested exposures land on whole multiples of it (126, 285, 984, 2984 µs). Rows are read about 7.6 ms apart top to bottom, about 2.2 ms across the grid. Inferred from the exposure steps; to be confirmed by a timing reference (Calibration). |
| Clock | `SensorTimestamp` is on CLOCK_MONOTONIC (it matches monotonic and boottime alike, and the box never suspends). Whether it marks the start of exposure or of readout is still open. |
| Levels (8x8 grid, brightness 40) | At gain 1.0 and 1 ms exposure, every LED resolves with no saturated pixel; at the auto-exposure setting (gain 8, ~16 ms) the grid saturates. The ISP leaves a pedestal of about 30 per channel in dark areas. At 100 µs some LEDs drop out of a frame: the LEDs' PWM showing through (Calibration, step 3). |
| Capture cost | `record` with 64 regions: about 14% of one core, zero dropped frames in 30 s (2025 frames, spacing sd 1 µs). |

**Rolling shutter.** Each image row starts its exposure a fixed line time after the previous one, so the top and bottom of a frame are exposed several ms apart. A region's timestamp is the frame timestamp plus (its row centroid × the line time). The line time is measured, not taken from a datasheet (Calibration, step 2).

A global-shutter camera would remove the row correction and the banding concerns below, but nothing in this plan requires it. Revisit if calibration shows the OV5647 can't meet the timing gates.

## Timing model

One run has four clocks. All timing is converted to the host's monotonic clock:

| Clock | Source | Mapped to host monotonic by |
|---|---|---|
| Host | `time.monotonic_ns()` and `time.time()` | (time.time and monotonic) pairs recorded at run start and end |
| Audio | when sound actually leaves the speaker | the existing per-run sync clicks (`tools/device_eval.py`): the device's detections of three irregular clicks before the music give the run's true audio lag |
| Device | firmware `millis()` in `TRANSIENT.timestampMs` | the existing clock sync (`test_runner._sync_clock` → `TestSession.set_clock_offset`), its error recorded per run |
| Camera | picamera2 sensor timestamp, per frame, plus the row offset | a timing reference (Calibration, step 1), which also confirms which kernel clock the timestamp is on |

The chain and the sources of spread, to be confirmed by measurement:

```
 sound → mic → detection (fw millis) → next render frame → LED write → light → camera frame
         |---- audio latency ------|  |------ render latency ------------|   |- capture -|
         scored by serial today        5-10 ms frame loop + ~2 ms for       ±7.4 ms frame
         (+ USB audio buffering)       64 LEDs (WS2812 at 30 µs/LED)         quantization
```

Expected detection→light: roughly 3–15 ms of render plus the camera's quantization. The baseline runs replace these estimates with measured distributions.

**Sub-frame timing (optional).** With exposure close to the frame period, a light that comes on part-way through a frame shows at partial brightness. The fraction gives an onset time inside the frame. That only works for pulses that hold their brightness for a frame or two, and the fleet's hit flashes decay fast. Treat it as an experiment, validated against the timing reference before any gate uses it. The baseline method is frame-quantized timing with statistics.

## How it works

### 1. Region map (adapts to layout changes)

The harness finds the LEDs instead of being told where they are:

1. A survey (`camera_capture.py survey`) runs while the LEDs animate. It records each pixel's brightness mean, variance and max over time. LEDs vary; the background should not (keep ambient light constant and low).
2. Find each LED as a local peak of the standard-deviation map, judged against its own neighbourhood (`camera_analysis.find_regions`). A single global threshold doesn't work: generators light some LEDs far more often than others (fire rarely reaches the top rows), and light reflected off the foam varies too. Each LED's region is the pixels near its peak above a fraction of its own level. On the box's grid this finds all 64 LEDs, 69-81 px each, and nothing else.
3. Check the region follows the chip: correlate its brightness series with the chip's serial `TRANSIENT` train. That needs no device commands and no resets (60-second rule).
4. Segments come for free: a region per LED, so the whole grid, a row or one LED are sums of regions. Generators with spatial effects (e.g. hit height) can use them.
5. Save the region map (`.npz`): the label image, each region's area, centroid and bounding box, and the camera settings it was made with. `record` refuses a map made with other settings.

**Every run checks the map still fits:** light outside the region, a region that stays dark while the chip reports detections, or different camera settings make the run INVALID with "layout changed: re-run the region calibration". So a moved grid or a bumped camera can't produce wrong numbers, only a clear request to recalibrate.

### 2. Capture

A helper process, `scripts/testbox/camera/camera_capture.py`, runs on the system Python, which has `python3-picamera2`. blinky-server's venv doesn't have it, and the process boundary keeps camera work out of the server's event loop. blinky-server starts and stops it for each run, next to `ffplay`.

- Pinned to a core away from `ffplay` and the server (`taskset`), so capture load doesn't move the audio timing (host-timing gate).
- Fixed controls: frame duration, exposure, analog gain, AWB gains. AE, AWB and any denoise that blends frames are off. Each frame's metadata is checked against the locked values; a mismatch makes the run INVALID.
- Per frame it records the sensor timestamp, exposure and gain, and per region the summed R, G and B, the saturated-pixel count and the peak value. Dropped frames (a gap over 1.5 periods) and frames whose controls drifted are counted in its summary. It does **not** store raw video (4 minutes of 640×480 YUV is about 6.6 GB). For human review it can save a low-rate JPEG preview; a short clip around any event the scorer flags is still to do.
- Output goes next to the run's other test data (one `.npz` per run).

### 3. Visual onsets

For the grid's region, the same recipe as audio onset detection (a published approach; see blinky-firmware's `docs/ONSET_DETECTION_LITERATURE_2026_05_02.md`):

- Signal: summed luminance, background-subtracted (the region's level with the LEDs dark, taken at run start). A generator profile may name a color channel or segment instead.
- Visual flux: the half-wave rectified frame-to-frame rise, normalized by the region's range (p99 - p1). A region whose range isn't well above its frame-to-frame noise has no onsets (sensor noise, normalized, would otherwise look like flashes; a steadily lit LED is the same case).
- Peak picking: adaptive threshold (median + k·MAD over a ~1 s window) and a minimum gap between onsets of 50 ms. The onset's time is the first frame of the rise, with the row offset applied.

### 4. Measurements

Per run:

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
- **production** (the fleet's default generator, e.g. Fire's per-hit glow, #280): statistical checks only: follow strength, visual drum accuracy, freezes. Its background flicker would make per-event orphan counts meaningless.

A profile sets the signal (luminance, a channel or segments), the peak-picking constants, whether the generator animates continuously (for the freeze check), and which measurements are gated. A new generator or LED design needs a new profile and a baseline, not new harness code.

The test run selects the chip's generator through the validation job and never saves it, like the run's existing reset to defaults. No ad-hoc config changes (CLAUDE.md, "Device Workflow Discipline").

## Calibration

Repeat whenever the camera or the grid moves.

1. **Timing reference.** The sensor timestamp is already on host monotonic time (The camera, Clock). What a reference still has to give is where in the exposure the timestamp falls, and the floor the camera adds to every timing number. A Pi GPIO LED is out: the Pi can't be reached for soldering (owner, 2026-11-23). The remaining option is the chip itself showing flashes at times it reports over serial, with the serial clock offset as the link. That needs a firmware hook (a test pattern with timestamps), to be designed. Several hundred flashes give:
   - the camera timestamp's offset from host monotonic, and which clock it is on;
   - the timing error distribution the camera alone contributes (the floor under every other number);
   - from LEDs at different heights (the grid's rows), the rolling-shutter line time, checking the 15.87 µs inferred from exposure steps.
   A few toggles at the start of every run re-check the offset. If it drifted beyond tolerance, the run is INVALID.
2. **Rolling-shutter line time**, from step 1, stored with the camera settings (`camera_analysis.LINE_NS` until then).
3. **LED PWM.** WS2812-class LEDs dim with PWM at a few hundred Hz to a few kHz. A short exposure makes that PWM show as brightness ripple, and with a rolling shutter as bands. Measure the PWM period once (with a short exposure, the band spacing in rows × line time gives it). Then set the exposure to the largest whole number of PWM periods that fits in the frame period, which averages the PWM out.
4. **Levels.** Set LED brightness for test runs and the analog gain so the brightest flash stays below ~80% of full scale. Saturated pixels hide the rise and the brightness. A region with saturated pixels makes the run INVALID.
5. **Region map** (How it works, step 1).
6. **Baseline.** Current stable firmware, both profiles, 5 runs on the `edm/` corpus. The thresholds below come from these runs.

## Gates (provisional)

Thresholds are set from the baseline, then frozen in the profile files. The shapes:

| Gate | Pass condition (provisional) |
|---|---|
| Camera timing | Timing reference: camera timing error IQR ≤ one frame period; offset stable within ±2 ms across the run. |
| Render fidelity (diagnostic) | ≥ 95% of serial detections light up; ≤ 5% of visual onsets lack a detection. |
| Render latency (diagnostic) | median within baseline ± 5 ms; p90 ≤ 50 ms. |
| Follow (both profiles) | the follow peak beats every other-track null on ≥ 16 of 18 tracks; peak lag consistent with audio lag + render latency. |
| Visual drum accuracy (both) | kick/snare recall within 0.05 of the same run's serial recall. |
| Freezes (production) | none longer than 100 ms while music plays (matching the staging suite's `maxFrameMs` limit). |
| Repeatability | 5 runs, same firmware: medians vary ≤ 5 ms; follow margin and visual recall vary ≤ 0.03. |

Verdicts per run: **PASS**, **MARGINAL** (inside the gate but outside the baseline's spread: a person looks at the flagged clips), **FAIL**, **INVALID** (measurement problem, with the reason).

## Code shape

The camera code lives in `scripts/testbox/camera/` and runs on the box's system Python (numpy and picamera2 from apt). blinky-server's venv has neither, and the box stays capture and routing only.

- `camera_capture.py` (built): `still`, `survey`, `record`, described above.
- `camera_analysis.py` (built in part): pure functions on arrays: region discovery, frame timing, visual onsets, row offsets. CI tests it on synthetic data (`test_camera_analysis.py`, job "Test box camera analysis"). Matching against serial detections and the follow test come next.
- blinky-server starts and stops `camera_capture.py record` around a run as a subprocess, records the clock pairs, and keeps its `.npz` with the run's data (not built).
- Calibration: camera timing, PWM, levels, region map, baseline (not built beyond the region survey).
- `DeviceRunScore` gets an optional `visual` block, and the console shows it next to the serial scores.
- `setup.sh` installs `python3-picamera2` and adds the user to the `video` group (done).

## Rollout

1. **On the box.** Done: picamera2, the capture helper, region discovery, frame timing (no drops at 67.5 fps) and levels. Still to do: the timing reference (a firmware flash pattern, Calibration step 1), then the line time and PWM measured against it. Do the numbers support the plan, or is a global-shutter camera needed?
2. **Offline first.** Capture alongside normal validation runs, score afterwards, compare with serial. Nothing gates yet.
3. **Baseline** on the current stable release; set and freeze the thresholds.
4. **Into the validation job:** the visual block in every box validation run.
5. **Into the staging suite** (`blinky_server/testing/staging.py`): visual fidelity, follow and freeze checks join its health checks.

## Open questions

- Can the OV5647 run faster in a cropped mode, or a newer libcamera tuning? Faster frames shrink quantization directly. 67.5 fps is the measured ceiling at 640×480 today.
- Is the timestamp from picamera2 exposure start or readout? The timing reference answers this; the plan assumes nothing.
- The timing reference's firmware hook: which pattern, and how its flash times reach the host (TRANSIENT-style lines with `millis()`?).
- Should the PLP phase visuals (the `Audio` generator's moving phase row) be checked against the wall clock here, next to the staging suite's beat-pace check?
