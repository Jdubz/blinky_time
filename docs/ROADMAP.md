# Roadmap

Where the project is going, in priority order, across all three repos:
- blinky_time: tooling
- [blinky-firmware](https://github.com/Jdubz/blinky-firmware): firmware
- [blinky-corpus](https://github.com/Jdubz/blinky-corpus): validation corpus and scorers

**GitHub issues are the system of record.** This page sets the order and links to the issues; it doesn't copy their status. Every open issue carries one of these labels:

| Label | Priority | Open issues |
|---|---|---|
| `P0-tooling` | Test-box hardware testing, Spark and devtop workflow | [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3AP0-tooling) · [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP0-tooling) |
| `P1-audio` | Consistent drum-hit reactivity | [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP1-audio) · [blinky-corpus](https://github.com/Jdubz/blinky-corpus/issues?q=is%3Aopen+label%3AP1-audio) |
| `P2-smoothness` | LED smoothness, no dropped frames | [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP2-smoothness) |
| `P3-esp32` | ESP32-S3: dual cores, NN feasibility | [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP3-esp32) · [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3AP3-esp32) |
| `parked` | Not scheduled (record mode: after P1–P3) | [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3Aparked) · [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3Aparked) |

A new issue gets a label when it's filed. When priorities change, change this page and the labels together.

## Order (owner, 2026-11-23)

> Box tooling will support all other endeavors. We need to solidify the hardware testing in the test box and leverage the Spark and devtop services to their full extent. Once we have an elegant workflow in place then we can focus on real improvements.

1. **P0, tooling.** Solid, hands-off hardware testing on the test box, and full use of the DGX Spark and devtop.
2. **P1, audio.** Consistent reactivity to drum hits.
3. **P2, smoothness.** No dropped frames, ever.
4. **P3, next generation.** The ESP32-S3 again: dual cores, and whether NN analysis is feasible there.

## Quality bars (they hold at every priority)

- **LED smoothness:** dropped frames are never acceptable. "Performance" means accuracy; speed only has to fit the frame budget.
- **The task is drum hits** (kicks and snares), not beats. Telling kicks from snares doesn't matter; consistent reactivity that follows the music as a person hears it does.
- **On the nRF52840, SuperFlux is the detector.** The onset NN is shelved there, because a tie goes to flux.
- **Audio changes** fix a measured root cause with a published method (CLAUDE.md). Research first; don't build, measure and toss.
- **The test box** is a test box, not strict CI. It routes data and captures tests; heavy compute goes to CI or the Spark.

## P0: an elegant test-box workflow

Umbrella: [#386](https://github.com/Jdubz/blinky_time/issues/386). The loop:

> change → CI → staging build → box hardware test (device + camera) → report → promotion

Heavy work runs on the Spark or devtop, never on the box.

**Next: the dev loop** (owner, 2026-11-25; design in [`DEV_LOOP.md`](DEV_LOOP.md)). Build a commit on a devtop build service, flash it to the box's test chip, run only the checks a change needs, then send it through CI. The box gets `dev` and `queue` modes (no expiry; work waiting behind dev mode is visible). Drafts run no heavy CI. Smoke tests block on health, never on scores. Top-scoring builds are proposed for stable. Nothing from a fork reaches the owner's machines.

**Done when** (owner, 2026-11-24): a full dev loop is iterable with adequate observability. Measured 2026-11-24: a firmware PR to promoted master takes about an hour (CI and review ~15-35 min, the box's hardware test ~30 min, promotion ~6-8 min).

**Where it stands (2026-11-24).** The box has its permanent test chip, a XIAO nRF52840 Sense Plus driving an 8x8 grid, and since 2026-11-24 a second chip, a XIAO ESP32-S3 (unconfigured, old firmware; `TEST_BOX.md`). Every chip on the box is a test chip (#382). The staging suite passes end to end, with the flash race fixed (#385, blinky-firmware#47), the health settle (#384) and review verdicts in the report (#383). Built since:
- **Observability:** Alloy ships the box's logs, metrics and traces to the LGTM stack (#390), blinky-server exports Prometheus metrics and OTLP traces (#391; its metrics reached Prometheus only from 2026-11-24, when #407 fixed the deployer's install of the Alloy config), staging reports go to the Spark artifact store (#392), and the box has a Grafana dashboard (observability#162).
- **Camera harness** ([#387](https://github.com/Jdubz/blinky_time/issues/387)): capture, region discovery, calibration with the firmware's `testpattern` (blinky-firmware#53: pixel walk, flash timing reference, PWM) and per-device detection-to-light reviews (#393, #396, #399). It is a secondary review, never a gate (`TEST_BOX_CAMERA_HARNESS_PLAN.md`).
- **Timing you can trust:** the clock sync times the reply line (#395, RTT 3-4 ms), runs record the audio start, clock and the chip's loop timing (#394, #399).
- **CI only runs what a change touches** (#397, blinky-firmware#55).
- **Serial:** a quiet device is no longer dropped as dead (#401).
- **The box can SSH to devtop** (owner-authorized key, 2026-11-24) and fixed the org's CI runners there (homelab-stacks#10).
- **blinky_time and blinky-firmware are decoupled** (owner, 2026-11-24: "they need to be completely decoupled"): no submodule, no source dependency, and neither repo reads the other's tree (#411, #413, #414 and the submodule's removal). They share three things: blinky-firmware's published artifacts (releases `bN` and staging builds with their manifest), which blinky-server and the console mirror download through blinky-server's own host tools (`blinky-server/hosttools/`); the mel contract files (`ml-training/spec/mel_frontend_v1.json` and `mel_golden_v1.npz`), carried identically by both and checked on each side in CI; and service APIs (the box's `Hardware test (staging)` status and promotion PRs, firmware's `release_promote.py` dispatching the console deploy, its `uf2_upload.py` calling blinky-server's HTTP API). A model reaches the firmware by a blinky-firmware PR that copies ml-training's exported header in.

**Next:**
- Registry-driven config for box chips: the server uploads a chip's registry entry itself. The grid's entry (`devices/registry/testbox_grid_v1.json`, orientation 5, brightness 0) exists since #416 but was uploaded by hand.
- The ESP32-S3 ([blinky-firmware#50](https://github.com/Jdubz/blinky-firmware/issues/50)): its wiring and device config from the owner, a flash path on the box (P3 overlaps).
- Per-server device ownership (`TEST_BOX.md`, "Needed: per-server device ownership"): each device belongs to exactly one server; BLE testing on the box waits on it.
- Camera: the follow test against drum labels, sound-to-light, and a review attached to each staging report.
- Run the release gate on the box through the server API.
- Recovery: USB auto-recovery on the box's enrolled chip ([#219](https://github.com/Jdubz/blinky_time/issues/219)), server-triggered SWD, and the bootloader gate runner.
- Spark and devtop: put the surveyed services to work (#386): Grafana observability for the box, the artifact store for reports and captures, Spark `audio` / `gpu_task` jobs for heavy work.

## P1: consistent drum-hit reactivity

Umbrella: [blinky-firmware#8](https://github.com/Jdubz/blinky-firmware/issues/8). The primary metric is repS, the label-free repetition-consistency score; drum metrics come second.

- **The rest of #8:**
  - tune and improve the rhythm-pattern bias (`onsetPatternBias`; it stays);
  - the context-dependent threshold and the threshold cliff;
  - the device vs host gap;
  - the flux δ re-tune on the overlapped STFT, and the remaining ~9.6% of frame-phase-sensitive hits (both carried over from #20, closed 2026-11-23).
- **[blinky-firmware#26](https://github.com/Jdubz/blinky-firmware/issues/26):** tempo multiple and phase on syncopated content. It feeds the pattern tracker the bias depends on.
- **[blinky-firmware#9](https://github.com/Jdubz/blinky-firmware/issues/9):** simulator visual-impact metrics, the "ships when" check for #8.
- **[blinky-corpus#4](https://github.com/Jdubz/blinky-corpus/issues/4):** hand-curated ground truth for the 18 edm/ tracks (owner time). It anchors absolute recall; repS doesn't need it.

## P2: LED smoothness

- [blinky-firmware#49](https://github.com/Jdubz/blinky-firmware/issues/49): a settings save stalls the loop ~85–115 ms (flash page erase).
- [blinky-firmware#23](https://github.com/Jdubz/blinky-firmware/issues/23): JSON stream formatting runs on the render loop. The box streams during tests.
- [blinky-firmware#14](https://github.com/Jdubz/blinky-firmware/issues/14): the LED DMA wait caps fps on large strips. Needed before the 1024-LED matrices.
- ~~blinky-firmware#59, the ~30 ms detection-to-light tail~~: fixed 2026-11-25 by ACF phase 1 ([blinky-firmware#61](https://github.com/Jdubz/blinky-firmware/pull/61), exact incremental lag tables, each decision spread over its cycle). On the box: `maxFrameMs` 31–34 → 15 ms (streaming off), `acfUs` 17–19.5 → 6.4–7.2 ms, and a camera p90 of 29–31 → 19–21 ms; tracker harness unchanged. ACF phase 2 (narrower range or comb filterbank for pass 2, 32-bit table updates) is optional CPU headroom, not a smoothness blocker.
- Then tighten the staging frame-time gate. It allows 100 ms today, which the release docs call a visible freeze. The ACF stall is gone (frames now peak at 15 ms with streaming off and 16–18 ms during validation), so what's left is #49 (settings-save stall) and #23. The threshold is the owner's call, and it changes in two places together: the server's staging suite and blinky-firmware's `release_gate.py`. The camera harness (P0) can confirm frames from outside.

## P3: ESP32-S3

Umbrella: [blinky-firmware#50](https://github.com/Jdubz/blinky-firmware/issues/50). Design: blinky-firmware `docs/NEXTGEN_ESP32S3_PLAN.md`. A XIAO ESP32-S3 is on the test box since 2026-11-24 (`TEST_BOX.md`): connected, unconfigured, old firmware; no flash path on the box yet.
- Bring the build back.
- Split the work across the two cores: the renderer never waits on audio.
- Answer whether a frame-level onset NN fits the audio core *and beats SuperFlux on accuracy*.

Training as a Spark gpu_task (#305) and the mel front end (blinky-firmware#21) belong here. Don't change deployed-fleet code for it.

## Later: after P1–P3

- **Record mode (#243):** wanted, but only after a pass at audio, smoothness and ESP32-S3 support (owner, 2026-11-23). Start with the #243 re-scope; its phone checks are listed there.

## Parked

- On-device NN training for the nRF52840 (see `ML_IMPROVEMENT_PLAN.md`).
- Bootloader self-update over BLE (blinky-firmware#10).
- BLE-DFU MTU (blinky-firmware#11).
- Fleet BLE reach (#221, blinky-firmware#16).
- Fleet deploy and ops checks that need the fleet in range (#215, #217).
- Low-value audio items off the flux path (blinky-firmware#18, #19).
- Drum labels on GPU models (#324): becomes active again with a P0 Spark workload or the ESP32-S3 NN.

## Open owner decisions

Decided 2026-11-23: the `swd-flash.local` Pi is retired; SWD recovery is the test box's XIAO Debug Mate only. Record mode waits until after P1–P3.

Decided 2026-11-24: P0 is done when a full dev loop is iterable with adequate observability. What P0 uses from the platform (surveyed in [#386](https://github.com/Jdubz/blinky_time/issues/386#issuecomment-7123790919)): box logs, metrics and traces go to the existing Grafana stack on bignasty, reports and captures go to the artifact store, and heavy analysis runs as Spark jobs.

Still open:

- **The frame-time gate:** the target threshold once the remaining P2 stalls are fixed (blinky-firmware#49, #23; the ACF one, #59, is fixed).
