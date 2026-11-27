# Roadmap

Where the project is going, in priority order, across all three repos:
- blinky_time: tooling
- [blinky-firmware](https://github.com/Jdubz/blinky-firmware): firmware
- [blinky-corpus](https://github.com/Jdubz/blinky-corpus): validation corpus and scorers

**GitHub issues are the system of record.** This page sets the order and links to the issues; it doesn't copy their status. Every open issue carries one of these labels:

| Label | Priority | Open issues |
|---|---|---|
| `P0-tooling` | Test-box hardware testing, Spark and build-host workflow | [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3AP0-tooling) · [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP0-tooling) |
| `P1-audio` | Consistent drum-hit reactivity | [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP1-audio) · [blinky-corpus](https://github.com/Jdubz/blinky-corpus/issues?q=is%3Aopen+label%3AP1-audio) |
| `P2-smoothness` | LED smoothness, no dropped frames | [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP2-smoothness) |
| `P3-esp32` | ESP32-S3: dual cores, NN feasibility | [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3AP3-esp32) · [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3AP3-esp32) |
| `parked` | Not scheduled (record mode: after P1–P3) | [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3Aparked) · [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3Aparked) |

A new issue gets a label when it's filed. When priorities change, change this page and the labels together.

## Order (owner, 2026-11-23)

> Box tooling will support all other endeavors. We need to solidify the hardware testing in the test box and leverage the Spark and build-host services to their full extent. Once we have an elegant workflow in place then we can focus on real improvements.

1. **P0, tooling.** Solid, hands-off hardware testing on the test box, and full use of the DGX Spark and the build host.
2. **P1, audio.** Consistent reactivity to drum hits.
3. **P2, smoothness.** No dropped frames, ever.
4. **P3, next generation.** The ESP32-S3 again: dual cores, and whether NN analysis is feasible there.

**Prioritised now, alongside P0 (owner, 2026-11-25): the hosted label reviewer.** Fixing the corpus labels ([blinky-corpus#4](https://github.com/Jdubz/blinky-corpus/issues/4)) is slow, laborious work and needs more hands, so the reviewer becomes a public page that the owner and recruited helpers use from anywhere. See "Product: two UIs and the hosted tools" below.

## Quality bars (they hold at every priority)

- **LED smoothness:** dropped frames are never acceptable. "Performance" means accuracy; speed only has to fit the frame budget.
- **The task is drum hits** (kicks and snares), not beats. Telling kicks from snares doesn't matter; consistent reactivity that follows the music as a person hears it does.
- **On the nRF52840, SuperFlux is the detector.** The onset NN is shelved there, because a tie goes to flux.
- **Audio changes** fix a measured root cause with a published method (CLAUDE.md). Research first; don't build, measure and toss.
- **The test box** is a test box, not strict CI. It routes data and captures tests; heavy compute goes to CI or the Spark.

## P0: an elegant test-box workflow

Umbrella: [#386](https://github.com/Jdubz/blinky_time/issues/386). The loop:

> change → CI → staging build → box hardware test (device + camera) → report → promotion

Heavy work runs on the Spark or the build host, never on the box.

**Next: the dev loop** (owner, 2026-11-25; design in [`DEV_LOOP.md`](DEV_LOOP.md)). Build a commit on the build host's build service, flash it to the box's test chip, run only the checks a change needs, then send it through CI. The box gets `dev` and `queue` modes (no expiry; work waiting behind dev mode is visible). Drafts run no heavy CI. Smoke tests block on health, never on scores. Top-scoring builds are proposed for stable. Nothing from a fork reaches the owner's machines.

**Done when** (owner, 2026-11-24): a full dev loop is iterable with adequate observability. Measured 2026-11-24: a firmware PR to promoted master takes about an hour (CI and review ~15-35 min, the box's hardware test ~30 min, promotion ~6-8 min).

**Where it stands (2026-11-24).** The box has its permanent test chip, a XIAO nRF52840 Sense Plus driving an 8x8 grid, and since 2026-11-24 a second chip, a XIAO ESP32-S3 (unconfigured, old firmware; `TEST_BOX.md`). Every chip on the box is a test chip (#382). The staging suite passes end to end, with the flash race fixed (#385, blinky-firmware#47), the health settle (#384) and review verdicts in the report (#383). Built since:
- **Observability:** Alloy ships the box's logs, metrics and traces to the LGTM stack (#390), blinky-server exports Prometheus metrics and OTLP traces (#391; its metrics reached Prometheus only from 2026-11-24, when #407 fixed the deployer's install of the Alloy config), staging reports go to the Spark artifact store (#392), and the box has a Grafana dashboard (observability#162).
- **Camera harness** ([#387](https://github.com/Jdubz/blinky_time/issues/387)): capture, region discovery, calibration with the firmware's `testpattern` (blinky-firmware#53: pixel walk, flash timing reference, PWM) and per-device detection-to-light reviews (#393, #396, #399). It is a secondary review, never a gate (`TEST_BOX_CAMERA_HARNESS_PLAN.md`).
- **Timing you can trust:** the clock sync times the reply line (#395, RTT 3-4 ms), runs record the audio start, clock and the chip's loop timing (#394, #399).
- **CI only runs what a change touches** (#397, blinky-firmware#55).
- **Serial:** a quiet device is no longer dropped as dead (#401).
- **Registry-driven chip config** (2026-11-27): on every connect the box's server checks a chip against its device config entry (`blinky_server/testing/box_config.py`; the entries are private, in the box's `~/.config/blinky/devices/`) and uploads it only if it differs, then reboots once to apply it when the chip is idle and past markStable. A chip with no entry (the ESP32-S3) is left alone. `TEST_BOX.md`, "Chip config from the box's entries".
- **Alerting** (2026-11-26): when the box, its runners or its chip break, the LGTM stack raises an alert from what the box ships, evaluated off the box, and the platform's triage agent files it as an issue in the observability repo (`TEST_BOX.md`, "Alerts"). Strict-rule warnings and scores stay reports.
- **The box can SSH to the build host** (owner-authorized key, 2026-11-24) and fixed the org's CI runners there (homelab-stacks#10).
- **blinky_time and blinky-firmware are decoupled** (owner, 2026-11-24: "they need to be completely decoupled"): no submodule, no source dependency, and neither repo reads the other's tree (#411, #413, #414 and the submodule's removal). They share three things: blinky-firmware's published artifacts (releases `bN` and staging builds with their manifest), which blinky-server and the console mirror download through blinky-server's own host tools (`blinky-server/hosttools/`); the mel contract files (`ml-training/spec/mel_frontend_v1.json` and `mel_golden_v1.npz`), carried identically by both and checked on each side in CI; and service APIs (the box's `Hardware test (staging)` status and promotion PRs, firmware's `release_promote.py` dispatching the console deploy, its `uf2_upload.py` calling blinky-server's HTTP API). A model reaches the firmware by a blinky-firmware PR that copies ml-training's exported header in.

**Next:**
- The ESP32-S3 ([blinky-firmware#50](https://github.com/Jdubz/blinky-firmware/issues/50)): its wiring and device config from the owner, a flash path on the box (P3 overlaps).
- Per-server device ownership (`TEST_BOX.md`, "Needed: per-server device ownership"): each device belongs to exactly one server; BLE testing on the box waits on it.
- Camera: the follow test against drum labels, sound-to-light, and a review attached to each staging report.
- Run the release gate on the box through the server API.
- Recovery: USB auto-recovery on the box's enrolled chip ([#219](https://github.com/Jdubz/blinky_time/issues/219)), server-triggered SWD, and the bootloader gate runner.
- Spark and the build host: put the surveyed services to work (#386): Grafana observability for the box, the artifact store for reports and captures, Spark `audio` / `gpu_task` jobs for heavy work.

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

- ~~blinky-firmware#49, a settings save stalls the loop ~85–115 ms~~: fixed 2026-11-26 by the settings journal ([blinky-firmware#83](https://github.com/Jdubz/blinky-firmware/pull/83): saves only write flash, erases happen at boot). On the box, windows with a save went from 115–119 ms to 15–19 ms, the same as windows without one.
- ~~blinky-firmware#23, JSON streaming stalls the loop~~: fixed 2026-11-26 ([blinky-firmware#85](https://github.com/Jdubz/blinky-firmware/pull/85): printf-exact integer float formatting, and serial output through a RAM buffer drained by a lower-priority task; whole stream lines skipped and counted in `streamDropped` if the host stops reading). On the box, `maxFrameMs` with any stream mode on is 15–18 ms, the same as streaming off (`nn` was 27–28 ms).
- ~~[blinky-firmware#14](https://github.com/Jdubz/blinky-firmware/issues/14), the LED DMA wait caps fps on large strips~~: fixed 2026-11-26 ([blinky-firmware#89](https://github.com/Jdubz/blinky-firmware/pull/89): `show()` stages the frame and a SEQEND-driven transmit task starts it, newest frame wins). On the box at 384 LEDs, loop time in `show()` dropped from 1.7 ms mean / 9.8 ms max to 0.5 / ≤1 ms, and `minFrameMs` from 12 (the wire time) to 4. At 64 LEDs, fps went from 165 to 182. Still open before the 1024-LED matrices: heap (~71 B/LED, so ~420 LEDs per chip) and render cost (~30 µs/LED). See blinky-firmware `docs/HARDWARE.md`, "LED output".
- ~~blinky-firmware#59, the ~30 ms detection-to-light tail~~: fixed 2026-11-25 by ACF phase 1 ([blinky-firmware#61](https://github.com/Jdubz/blinky-firmware/pull/61), exact incremental lag tables, each decision spread over its cycle). On the box: `maxFrameMs` 31–34 → 15 ms (streaming off), `acfUs` 17–19.5 → 6.4–7.2 ms, and a camera p90 of 29–31 → 19–21 ms; tracker harness unchanged. ACF phase 2 (narrower range or comb filterbank for pass 2, 32-bit table updates) is optional CPU headroom, not a smoothness blocker.
- The frame-time gate: **blocking since 2026-11-26** (owner, after readiness issues #444, #445, #448): longest frame ≤ 50 ms (was 100 ms), fps ≥ 60 and no overrun growth, over every run's `json info` reads with no exclusions, in the box's hardware checks ([`DEV_LOOP.md`](DEV_LOOP.md) "Strict rules") and blinky-firmware's `release_gate.py` alike. The box measures 15–27 ms in every window. The camera harness (P0) can confirm frames from outside.

## P3: ESP32-S3

Umbrella: [blinky-firmware#50](https://github.com/Jdubz/blinky-firmware/issues/50). Design: blinky-firmware `docs/NEXTGEN_ESP32S3_PLAN.md`. A XIAO ESP32-S3 is on the test box since 2026-11-24 (`TEST_BOX.md`): connected, unconfigured, old firmware; no flash path on the box yet.
- Bring the build back.
- Split the work across the two cores: the renderer never waits on audio.
- Answer whether a frame-level onset NN fits the audio core *and beats SuperFlux on accuracy*.

Training as a Spark gpu_task (#305) and the mel front end (blinky-firmware#21) belong here. Don't change deployed-fleet code for it.

## Later: after P1–P3

- **Record mode (#243):** wanted, but only after a pass at audio, smoothness and ESP32-S3 support (owner, 2026-11-23). Start with the #243 re-scope; its phone checks are listed there.
  - **It no longer fits on the nRF52840, and may be ESP32-S3 only** (owner, 2026-11-26: accepted for now). Record mode allocates one 32 KB block (`RecordStream.cpp`). The free heap fell from about 39 KB to about 25 KB this month: blinky-firmware#61's exact ACF tables took about 27 KB, #85 shrank the heap by about 8 KB, and #89 added the LED staging copy. So `record` now refuses to start with a `[FALLBACK]` error, loudly, not silently. Either it moves to the ESP32-S3 (8 MB PSRAM), or the nRF52 path gets a smaller buffer (under about 24 KB) or RAM back from elsewhere. Decide when #243 is picked up.

## Product: two UIs and the hosted tools (owner, 2026-11-25; the label reviewer is prioritised now, the rest isn't top priority)

Today one app, blinky-console ("Blinky Fleet"), is both the fleet UI (blinky-server mounts the build) and the public site blinky-time.joshwentworth.com (the same build on Firebase, with `VITE_FIRMWARE_UPDATES=on`). They become two UIs:

- **Fleet UI** (served by blinky-server on the fleet server and the test box): fleet operations over many devices, deploys, and the test box's state (mode, queue, hardware checks).
- **Public site** (blinky-time.joshwentworth.com): single-device management for wearables at shows and events.
  - Connect to one device: Web Serial now, Bluetooth once that path passes on hardware (`BLE_UPDATES_ENABLED`).
  - Change its settings.
  - Turn on record mode (#243). The feature itself still waits for P1–P3 (see "Later"); its controls live here.
  - Update its firmware with the signed OTA flow that is live today (blinky-firmware `docs/WEB_FLASHER.md`).
  - Link to further resources and pages, such as the label reviewer and docs.
- **Shared code:** the device protocol, schemas and firmware services stay one shared package, so the two UIs can't drift apart.
- **The label reviewer is a page of the public site, backed by a long-lived service on the build host** (owner, 2026-11-25; prioritised now). Both live in this repo: `labels-service/` (the service and the page) and `deploy/build-host/labels/` (its deployment).
  - **The page:** `https://blinky-time.joshwentworth.com/labels/`, built by the site's deploy from `labels-service/index.html`. Page and service come from the same commit, and the page asks for a reload when the server reports another version. It's ready to fold into the single-device site later.
  - **The API:** `https://blinky-labels-api.joshwentworth.com`. CORS allows only the site's origin.
  - **Simple by design** (owner: "there shouldn't even be the concept of a user"). There are no users, names, drafts, locks or Save button. Each track has one shared set of labels. Every edit saves by itself as an operation, and everyone's page pulls everyone else's edits within seconds.
  - **Autopublish, pulled by the corpus:** the service holds no credential. blinky-corpus's scheduled workflow pulls the changed tracks from the public API, checks them with `main`'s code, commits them as a `labels/auto-*` batch and auto-merges it. Corpus CI's label checks are anti-vandalism only (the schema, no mass deletion, no empty class). Partial progress is published, and the scorer uses a track once it's complete. Git history is the undo.
  - **Access:** no login (owner: nothing on the page is sensitive; watch the traffic and react). Rate limits per client address, logs and alerts in the LGTM stack.
  - **The network:** reached only through an existing Cloudflare tunnel, with no published port. Sandboxed, with egress to private and tailnet ranges blocked (only HTTPS to GitHub).
  - **Deploys:** the build host's pull deployer (`deploy/build-host/deployer/`, like the test box's) deploys a master commit once CI and the review pass. No GitHub runner is involved, and the build service can move onto it later.
  - **Topology:** the repos name only the public product URLs; every host value lives in a gitignored env file on the build host.

## Parked

- On-device NN training for the nRF52840 (see `ML_IMPROVEMENT_PLAN.md`).
- Bootloader self-update over BLE (blinky-firmware#10).
- BLE-DFU MTU (blinky-firmware#11).
- Fleet BLE reach (#221, blinky-firmware#16).
- Fleet deploy and ops checks that need the fleet in range (#215, #217).
- Low-value audio items off the flux path (blinky-firmware#18, #19).
- Drum labels on GPU models (#324): becomes active again with a P0 Spark workload or the ESP32-S3 NN.

## Open owner decisions

Decided 2026-11-23: the old SWD Pi is retired; SWD recovery is the test box's XIAO Debug Mate only. Record mode waits until after P1–P3.

Decided 2026-11-24: P0 is done when a full dev loop is iterable with adequate observability. What P0 uses from the platform (surveyed in [#386](https://github.com/Jdubz/blinky_time/issues/386#issuecomment-7225694177)): box logs, metrics and traces go to the existing Grafana stack on the NAS, reports and captures go to the artifact store, and heavy analysis runs as Spark jobs.

Still open:

