# Test box camera harness: plan

**Status:** rollout steps 1-2 under way (2026-11-24). Built (`scripts/testbox/camera/`): the capture helper, region discovery, visual onsets, matching against serial detections, the timing budget, and `camera_review.py`, which records a validation run and reports on it. Not built: the follow test against drum labels, sound-to-light timing, a review attached to staging reports. Camera facts were measured on the box's Pi 5 (`blinkytest`) on 2026-10-27 and 2026-11-24. Part of the test box (`TEST_BOX.md`).

The box has one test chip driving an 8x8 WS2812 grid, and the camera sees the grid (hardware: `TEST_BOX.md`, "Hardware as built"). This plan adds the camera to the validation harness, so a run judges what a person at a show sees, not only what the firmware reports over serial.

## What the camera answers

The serial stream already says when the firmware *detected* a hit (`TRANSIENT` lines, scored by `blinky_scoring` in the `corpus/` submodule, blinky-corpus). The camera adds three questions that serial can't answer:

1. **Does every detection light up?** (render fidelity) A detection the renderer drops, or a flash with no detection behind it, is invisible to serial scoring.
2. **How late is the light, and how steady is that delay?** (render latency and jitter) From detection to photons, and from the sound to photons.
3. **Does the light follow the music?** (audio-follow) Judged on the visuals alone, against the corpus's drum labels: kicks and snares (CLAUDE.md, "Task is Drum-Hit Detection"), not beats.

It also gives an independent check for visible freezes, which the staging suite today infers only from `maxFrameMs` (the longest loop frame in `json info`).

## Principles

- **A secondary review, never a gate** (owner, 2026-11-24). The camera and the chips run unsynced: the camera at 67.5 fps, each chip at its own frame rate on its own clock. A review says whether what the chips report over serial shows on their LEDs and how late, with the uncertainty spelled out, so a person can judge the setup. It never passes or fails a build; serial scoring and the staging suite do that.
- **Distributions, not single events.** Every stage of the chain jitters: USB audio buffering, the device's frame loop, the LED write, the camera's frame clock. One event's latency means little. Every timing result is a distribution (median, IQR, p10/p90) over hundreds of events, reported with its timing budget (below). A median over N events is far more precise than one event: with the camera's ±7.4 ms frame quantization (sd ≈ 4.3 ms), 300 events pin the median to about ±0.5 ms.
- **Tolerances that match the measurement.** Matching windows are wide enough for frame quantization plus the system's natural spread (set from baseline runs, below). Nothing is judged to the millisecond.
- **Design-agnostic.** The harness doesn't depend on LED counts, layouts or generator internals. It finds the lit regions itself, reads brightness from them, and detects visual onsets the way the firmware detects audio onsets: rises in a signal, adaptive threshold, peak picking. Whatever must differ per generator lives in a small profile file, not in code.
- **Separate the stages.** Detection→light (renderer) and sound→detection (audio pipeline) are measured separately, then checked against the end-to-end sound→light number. A regression then points at its stage.
- **Measurement problems are loud and never scored** (CLAUDE.md, "No Silent Fallbacks"). Dropped frames, unlocked exposure, saturated LEDs, a stale region map: the run is marked INVALID with the reason, never quietly reported on bad data. INVALID means the measurement failed, not the device.

## The camera

| | |
|---|---|
| Module | Raspberry Pi Camera Module v1 class: OmniVision **OV5647**, 5 MP, **rolling shutter**, fixed focus. On CSI (`rpicam-hello --list-cameras`). |
| Mode used | 640×480, 2×2 binned from the full 2560×1920 sensor (full field of view). The fastest mode libcamera lists for this sensor. |
| Measured frame rate | Asking for 60 fps gives 64.9 fps (15.40 ms); asking for 90 gives the cap, **67.5 fps (14.82 ms)**. Frame spacing sd ≈ 1 µs, so the camera's own clock is not a jitter source. |
| Field of view | About 54° × 41°. Check that the whole grid is in frame at the camera's mounting distance. |
| Focus | Fixed. Soft focus is harmless: the harness sums brightness over regions. |
| Software | `python3-picamera2` (apt, installed by `setup.sh`). Per-frame metadata gives the sensor timestamp, exposure and gain. |
| Line time | **31.75 µs** per output row (measured with the flash timing reference, 2026-11-24): the sensor's exposure steps are 31.75 µs; 200 flashes give 25.4 ± 3.6 µs from how often the bottom LED row sees a flash first; and with 31.75 µs the timing reference has a zero offset. The readout takes about 15.2 ms top to bottom, about 3.6 ms across the grid. An exposure between steps is made up with digital gain. |
| Clock | `SensorTimestamp` is on CLOCK_MONOTONIC (it matches monotonic and boottime alike, and the box never suspends). Whether it marks the start of exposure or of readout is still open. |
| Levels (8x8 grid, brightness 40) | At gain 1.0 and 1 ms exposure, every LED resolves with no saturated pixel; at the auto-exposure setting (gain 8, ~16 ms) the grid saturates. The ISP leaves a pedestal of about 30 per channel in dark areas. At 100 µs some LEDs drop out of a frame: the LEDs' PWM showing through (Calibration, step 3). |
| Capture cost | `record` with 64 regions: about 14% of one core, zero dropped frames in 30 s (2025 frames, spacing sd 1 µs). |

**Rolling shutter.** Each image row starts its exposure a fixed line time after the previous one, so the top and bottom of a frame are exposed several ms apart. A region's timestamp is the frame timestamp plus (its row centroid × the line time). The line time is measured, not taken from a datasheet (Calibration, step 2).

A global-shutter camera would remove the row correction and the banding concerns below, but nothing in this plan requires it. Revisit if the timing budget turns out too wide to be useful.

## Timing model

One run has four clocks. All timing is converted to the host's monotonic clock:

| Clock | Source | Mapped to host monotonic by |
|---|---|---|
| Host | `time.monotonic_ns()` and `time.time()` | (time.time and monotonic) pairs recorded at run start and end |
| Audio | when sound actually leaves the speaker | the existing per-run sync clicks (`tools/device_eval.py`): the device's detections of three irregular clicks before the music give the run's true audio lag |
| Device | firmware `millis()` in `TRANSIENT.timestampMs` | the clock sync (`test_runner._sync_clock`): the shortest of 5 `json info` round trips before the music, again after it for the drift. The raw capture records offset, RTT and drift, and the audio start (#394). Error: up to RTT/2, a constant shift per run. |
| Camera | picamera2 sensor timestamp, per frame, plus the row offset | nothing to map: it is CLOCK_MONOTONIC already (measured), and the capture records (monotonic, wall clock) pairs. Unknown: where in the exposure it falls, a shift of up to the exposure time (1 ms). |

The chain and the sources of spread, to be confirmed by measurement:

```
 sound → mic → detection (fw millis) → next render frame → LED write → light → camera frame
         |---- audio latency ------|  |------ render latency ------------|   |- capture -|
         scored by serial today        5-10 ms frame loop + ~2 ms for       ±7.4 ms frame
         (+ USB audio buffering)       64 LEDs (WS2812 at 30 µs/LED)         quantization
```

Expected detection→light: roughly 3–15 ms of render plus the camera's quantization. The baseline runs replace these estimates with measured distributions.

**The timing budget.** Every review lists, per device, what spreads or shifts its lags (`camera_analysis.timing_budget`):

| Source | Effect | Size on the box |
|---|---|---|
| Camera frame | the light came on in the frame before the one showing it; onsets are placed mid-interval | ±period/2 uniform: 14.8 ms wide, sd 4.3 ms |
| Exposure | timestamp at exposure start or readout | shift ≤ 1 ms |
| Chip frame | a detection waits for the chip's next render frame | 0 to 1/fps uniform: about 10 ms at ~100 fps |
| Clock sync | firmware times on the host clock | constant shift ≤ RTT/2 per run |
| Clock drift | the chip's crystal over the run | measured per run, corrected linearly |

**Two devices.** Each chip has its own clock (its own offset, RTT and drift) and its own frame loop, unsynced with each other and with the camera. Their numbers are never pooled: each device has its own lag distribution and budget. A camera frame samples both chips at once, but at different rows (rolling shutter), and each region's row offset is applied. A region belongs to the chip whose detections it follows (recall and precision, below), so the harness doesn't need to be told which LEDs are whose.

**Detection to light, measured** (2026-11-24, `Audio` generator, three edm/ tracks, absolute timing: #394 and #395 for the sync, the calibrated camera model above): median 14.3-17.4 ms, IQR about 9 ms, p10 7-9 ms, p90 29-31 ms; 99-100% of detections lit on sparse tracks (87% on garage, where 24 of 25 misses came during the previous flash's fade). The camera adds no offset (calibrated +0.2 ms) and a uniform 14.8 ms frame spread. Where it comes from (2026-11-24): `TRANSIENT.timestampMs` is the `millis()` at the top of the chip's loop, and the onset is drawn later in the same iteration (the `Audio` generator follows the picked onsets with no ramp), so the lag is the rest of that loop plus the 1.92 ms latch. Validation streams `debug`+`fast` (a long line every 10 ms), which **halves the loop rate**: 118-142 fps (~8 ms) with streaming off, 57-62 fps (~17 ms) during a run. That accounts for the ~15 ms median; a production chip (no streaming) should be at ~7-9 ms. The **~30 ms tail** is the chip's longest loop, 31 ms even with streaming off. The suspect is the ACF tempo + PLP work that runs inline every 100 ms before the frame is drawn; blinky-firmware#57 reports its cost in `json info` (`acfUs`, `acfMaxUs`, `plpUs`, `plpMaxUs`) to confirm. Reviews now take the chip's loop rate from the run itself (the server's post-run clock sync records `after_loop`).

**Sub-frame timing (optional).** With exposure close to the frame period, a light that comes on part-way through a frame shows at partial brightness. The fraction gives an onset time inside the frame. That only works for pulses that hold their brightness for a frame or two, and the fleet's hit flashes decay fast. Treat it as an experiment, validated against the timing reference before a review reports it. The baseline method is frame-quantized timing with statistics.

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

- Pinned to a core away from `ffplay` and the server (`taskset`), so capture load doesn't move the audio timing.
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

A profile sets the signal (luminance, a channel or segments), the peak-picking constants, whether the generator animates continuously (for the freeze check), and which measurements apply (`camera_review.PROFILES`). A new generator or LED design needs a new profile and a baseline, not new harness code.

The test run selects the chip's generator through the validation job and never saves it, like the run's existing reset to defaults. No ad-hoc config changes (CLAUDE.md, "Device Workflow Discipline").

## Calibration

Repeat whenever the camera or the grid moves.

1. **Timing reference.** `camera_calibrate.py timing`: a flash train from `testpattern flash` (irregular intervals, edge times read back from the chip), synced to the host through the server's reply-line timing (#395, RTT 3-4 ms). Result (2026-11-24, 200 flashes): all seen, none missed; lag spread IQR 7.0 ms, p10-p90 11.4 ms, which is the camera frame's quantization alone (uniform 14.8 ms: 7.4 and 11.9); with the 31.75 µs line time the offset is +0.2 ms, i.e. the model (sensor timestamp + row × line time + exposure − period/2) needs no correction. A Pi GPIO LED is out: the Pi can't be reached for soldering (owner, 2026-11-23). The remaining option is the chip itself showing flashes at times it reports over serial, with the serial clock offset as the link. That needs a firmware hook (a test pattern with timestamps), to be designed. Several hundred flashes give:
   - the camera timestamp's offset from host monotonic, and which clock it is on;
   - the timing error distribution the camera alone contributes (the floor under every other number);
   - from LEDs at different heights (the grid's rows), the rolling-shutter line time, which settled it at 31.75 µs (The camera, Line time).
   A few toggles at the start of every run re-check the offset. If it drifted beyond tolerance, the run is INVALID.
2. **Rolling-shutter line time**, from step 1: 31.75 µs (`camera_analysis.LINE_NS`).
3. **LED PWM.** WS2812-class LEDs dim with PWM at a few hundred Hz to a few kHz. A short exposure makes that PWM show as brightness ripple, and with a rolling shutter as bands. Measure the PWM period once (with a short exposure, the band spacing in rows × line time gives it). Then set the exposure to the largest whole number of PWM periods that fits in the frame period, which averages the PWM out. Tried 2026-11-24 with an exposure sweep (0.2-4 ms) on fire's base row: fire's own flicker (frame-to-frame CV 0.2-0.4) swamps any PWM ripple, and no generator holds still. Done 2026-11-24 with `testpattern solid` (blinky-firmware#53, `camera_calibrate.py pwm`): ripple falls steadily with exposure (16% at 0.35 ms, 1.2% at 1 ms, 0.3% at 4 ms) with no periodic minima, so the LEDs' PWM is short enough to average out at the 1 ms used; no exposure change needed.
4. **Levels.** Set LED brightness for test runs and the analog gain so the brightest flash stays below ~80% of full scale. Saturated pixels hide the rise and the brightness. A region with saturated pixels makes the run INVALID.
5. **Region map** (How it works, step 1).
6. **Baseline.** Current stable firmware, both profiles, 5 runs on the `edm/` corpus: the usual spread a review compares against.

## What a review reports (advisory)

Per run and device (`camera_review.py`, `report.json` and `report.md`). Nothing here is a pass or fail; a person reads it next to the serial scores.

| Item | Meaning |
|---|---|
| Responsive LEDs | LEDs whose onsets follow this chip's detections: recall (share of detections they light for) and precision (share of their flashes that have a detection) both ≥ 0.5. LEDs with high recall but low precision are listed apart: they move with the music (the `Audio` generator's energy row), not with detections. |
| Lit share | detections followed by a flash within −30..+80 ms. |
| Flashes without a detection | with their median strength next to the matched ones' (weak ones are decay flicker; strong ones are worth a look). |
| Detection to light | median, IQR, p10/p90, with the timing budget. Absolute when the raw capture has the audio start and clock (#394); relative otherwise (only the spread means anything). |
| Freezes | for generators that never hold still (fire): stretches with no change beyond the camera's noise, ≥ 100 ms. |
| Capture health | frames, frame spacing, dropped frames, control drift, saturation. Dropped frames make a run INVALID: a measurement problem, not a device result. |

Still to add: the follow test and visual drum accuracy against the corpus labels, sound-to-light timing (needs the speaker's output latency, below), and repeatability across runs.

## Code shape

The camera code lives in `scripts/testbox/camera/` and runs on the box's system Python (numpy and picamera2 from apt). blinky-server's venv has neither, and the box stays capture and routing only.

- `camera_capture.py` (built): `still`, `survey`, `record`, described above.
- `camera_analysis.py` (built): pure functions on arrays: region discovery, frame timing, visual onsets, row offsets, matching, distributions, the timing budget, recall/precision per region, sensor noise and freezes. CI tests it on synthetic data (`test_camera_analysis.py`, job "Test box camera analysis"). The follow test comes next.
- `camera_review.py` (built): records a validation run through blinky-server's API (`gen <generator>`, `persist_raw`) and writes the review. `--analyze DIR` re-runs the analysis on a saved review.
- Later: the staging suite runs a review and links it from its report, as information.
- Calibration: PWM and baseline (not built beyond the region survey and levels).
- `setup.sh` installs `python3-picamera2` and adds the user to the `video` group (done).

## Rollout

1. **On the box.** Done: picamera2, the capture helper, region discovery, frame timing (no drops at 67.5 fps) and levels. Still to do: PWM. The timing reference (Calibration, step 1) is a refinement, not a prerequisite: the budget already bounds the exposure term.
2. **Reviews on demand** (under way): `camera_review.py` around a validation run, with absolute timing once #394 is deployed.
3. **Baseline** on the current stable release: what normal looks like (lag distribution, lit share, orphan rate) for each generator, so a review can say "outside the usual spread".
4. **Alongside the staging suite:** a review of the suite's validation run, linked from the staging report (artifact store), never part of its verdict.

## Open questions

- **A firmware test-pattern command** would serve several calibration steps at once: a solid colour (PWM, levels), one LED at a time (region map and orientation, dead or stuck LEDs), and flashes at reported `millis()` times (the timing reference, step 1). None exists today: every generator animates.

- Can the OV5647 run faster in a cropped mode, or a newer libcamera tuning? Faster frames shrink quantization directly. 67.5 fps is the measured ceiling at 640×480 today.
- Is the timestamp from picamera2 exposure start or readout? The timing reference answers this; the plan assumes nothing.
- The timing reference's firmware hook: which pattern, and how its flash times reach the host (TRANSIENT-style lines with `millis()`?).
- Should the PLP phase visuals (the `Audio` generator's moving phase row) be checked against the wall clock here, next to the staging suite's beat-pace check?
