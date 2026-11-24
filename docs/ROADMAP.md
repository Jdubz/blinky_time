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
| `parked` | Not scheduled | [blinky_time](https://github.com/Jdubz/blinky_time/issues?q=is%3Aopen+label%3Aparked) · [blinky-firmware](https://github.com/Jdubz/blinky-firmware/issues?q=is%3Aopen+label%3Aparked) |

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

**Where it stands (2026-11-23).** The box has its permanent test chip: a XIAO nRF52840 Sense Plus driving an 8x8 grid (`TEST_BOX.md`). Every chip on the box is a test chip (#382). The staging suite passes end to end on it, with the flash race fixed (#385), the health settle (#384) and review verdicts in the report (#383).

**Next:**
- Finish the flash fixes: blinky-firmware#47, then the submodule bump.
- The grid's registry entry, and registry-driven config for box chips.
- The camera harness for the grid ([#387](https://github.com/Jdubz/blinky_time/issues/387)): dropped frames, render fidelity and latency, measured from outside the chip.
- Run the release gate on the box through the server API.
- Recovery: auto-recovery on an enrolled box chip (#219), server-triggered SWD, and the bootloader gate runner.
- Spark and devtop: inventory what each offers and route every heavy workload there.

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
- Then tighten the staging frame-time gate. It allows 100 ms today, which the release docs call a visible freeze. The camera harness (P0) can confirm frames from outside.

## P3: ESP32-S3

Umbrella: [blinky-firmware#50](https://github.com/Jdubz/blinky-firmware/issues/50). Design: blinky-firmware `docs/NEXTGEN_ESP32S3_PLAN.md`.
- Bring the build back.
- Split the work across the two cores: the renderer never waits on audio.
- Answer whether a frame-level onset NN fits the audio core *and beats SuperFlux on accuracy*.

Training as a Spark gpu_task (#305) and the mel front end (blinky-firmware#21) belong here. Don't change deployed-fleet code for it.

## Parked

- On-device NN training for the nRF52840 (see `ML_IMPROVEMENT_PLAN.md`).
- Record mode (#243).
- Bootloader self-update over BLE (blinky-firmware#10).
- BLE-DFU MTU (blinky-firmware#11).
- Fleet BLE reach (#221, blinky-firmware#16).
- Fleet deploy and ops checks that need the fleet in range (#215, #217).
- Low-value audio items off the flux path (blinky-firmware#18, #19).
- Drum labels on GPU models (#324): becomes active again with a P0 Spark workload or the ESP32-S3 NN.

## Open owner decisions

- **devtop:** which services it runs, so P0 can use them. The repo only describes one-shot CPU scripts.
- **The `swd-flash.local` Pi:** retire it, or keep it as a second SWD route next to the Debug Mate.
- **The frame-time gate:** the target threshold once the P2 stalls are fixed.
