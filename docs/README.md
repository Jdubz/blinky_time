# Blinky Time Documentation

This directory documents the tooling: blinky-server, blinky-console, ml-training, the test box and fleet operations. The firmware's docs (architecture, build, hardware, releases, browser updates) are in [blinky-firmware's `docs/`](https://github.com/Jdubz/blinky-firmware/tree/master/docs); see [Firmware](#firmware-blinky-firmware) below. The repo-root `CLAUDE.md` holds operational behavioural rules — start there for "what am I not allowed to do." This file is the topical index.

Each plan carries a status header; check it before treating the plan as outstanding work. **GitHub issues are the system of record** for open work.

## Live reference (read these first)

| Doc | Purpose |
|-----|---------|
| [`OPEN_ISSUES_2026_10_15.md`](OPEN_ISSUES_2026_10_15.md) | Current index of open issues by area (rhythm-tracking drift, record mode, brick risk, CI, device checks); each item links its GitHub issue. |
| [`SAFETY.md`](SAFETY.md) | Flashing/recovery safety model — short index of the invariants and where each is enforced. |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | Tooling development: git hooks, console + server dev. |
| [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md) | Where the system stands (verified against code) and what is left to do, plus a "don't retry" list. |

## Reference

| Doc | Purpose |
|-----|---------|
| [`TEST_BOX.md`](TEST_BOX.md) | The test box: hosts and roles, hardware as built, hands-off recovery chain, setup, blue/green deployer, staging runner, per-server device ownership. |
| [`TESTING_METHODOLOGY.md`](TESTING_METHODOLOGY.md) | Tiered testing, from blinky-firmware's host harness up to full validation on the box chip; drum metrics; removed metrics. |
| [`BLE_REFERENCE.md`](BLE_REFERENCE.md) | BLE today: NUS connections, fleet broadcast, the BLE-DFU protocol, known limitations. |
| [`ML_STORAGE_LAYOUT.md`](ML_STORAGE_LAYOUT.md) | Where the ML data is (bignasty archive, the Spark's object store) and the storage roots. |

## Plans (each has a status header)

| Doc | Status |
|-----|--------|
| [`ML_IMPROVEMENT_PLAN.md`](ML_IMPROVEMENT_PLAN.md) | NN shelved on the nRF52840; training paused; ESP32-S3 feasibility is the only active ML direction (P3). |
| [`TEST_BOX_CAMERA_HARNESS_PLAN.md`](TEST_BOX_CAMERA_HARNESS_PLAN.md) | **Planned, not built** — the box camera watching the 8x8 grid: render fidelity, latency distributions, follow test, freezes. No issue tracks it yet. |
| [`PHONE_RECORD_MODE_PLAN.md`](PHONE_RECORD_MODE_PLAN.md) | **Parked** — superseded by the #243 re-scope (live calibration-data capture). |

## Firmware (blinky-firmware)

| Doc | Purpose |
|-----|---------|
| [`RELEASE_PROCESS.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/RELEASE_PROCESS.md) | Release channels (stable / candidate / dev), staging and the box's hardware test, cutting, gating, promoting and rolling back. |
| [`BUILD_GUIDE.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/BUILD_GUIDE.md), [`DEVELOPMENT.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/DEVELOPMENT.md) | The pinned toolchain, flashing paths, device config; platform code, `SETTINGS_VERSION`, config safety. |
| [`AUDIO_ARCHITECTURE.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/AUDIO_ARCHITECTURE.md), [`GLOSSARY.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/GLOSSARY.md) | AudioTracker (onsets, ACF period, PLP pattern) and its terms. |
| [`ONSET_DETECTION_LITERATURE_2026_05_02.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/ONSET_DETECTION_LITERATURE_2026_05_02.md) | Published onset/drum-detection systems with citations — quote before claiming "the literature says X". |
| [`WEB_FLASHER.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/WEB_FLASHER.md) | Browser firmware updates (this repo's console page): signed QSPI OTA, signing, release mirror, rollout checklist. |
| [`HARDWARE.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/HARDWARE.md) | XIAO nRF52840 Sense / Sense Plus inventory, pins, wiring, the test box grid. |
| [`SWD_RECOVERY.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/SWD_RECOVERY.md), [`BOOTLOADER_WORKFLOW.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/BOOTLOADER_WORKFLOW.md) | SWD recovery through the test box's XIAO Debug Mate (the only SWD path); bootloader builds, invariants and recovery. |
| [`WEB_UPDATE_RESEARCH_2026_10.md`](https://github.com/Jdubz/blinky-firmware/blob/master/docs/WEB_UPDATE_RESEARCH_2026_10.md) | Research input for the web updater: platform constraints (iPhone, Windows UF2, Chrome's DFU blocklist). Not the current design. |

## Archive

Docs that no longer reflect the current system are in [`archive/`](archive/). They are kept for the diagnostic narrative they contain; **do not** treat them as current guidance:

- `archive/OPEN_ISSUES_2026_05_19.md`, `archive/OPEN_ISSUES_2026_05_20.md` — earlier open-issue snapshots; items still open from 2026-05-20 were carried into `OPEN_ISSUES_2026_10_15.md` §8.
- `archive/UPLOAD_OVERHAUL_PLAN.md`, `archive/UPLOAD_RELIABILITY_PLAN.md` — completed April 2026; the RAM-magic mechanism described still works and lives in firmware.
- `archive/FLASH_LOCKDOWN_AUDIT.md` — migration gap inventory; the migration finished.
- `archive/SAFETY_pre_2026_05.md` — pre-FlashLockdown safety doc, replaced by `SAFETY.md`.
- `archive/POSTMORTEM_2026_05_18_LEDTYPE.md` — single-incident postmortem; lessons landed in registry CI lint + Nrf52PwmLedStrip offset validation.
- `archive/FLASH_LOCKDOWN_PLAN.md` — the single-flash-entry-point lockdown (done); rationale for CLAUDE.md's invariant.
- `archive/BLUETOOTH_IMPLEMENTATION_PLAN.md`, `archive/BLE_FLEET_RELIABILITY_PLAN.md` — how BLE fleet management was built; current facts are in `BLE_REFERENCE.md`.
- `archive/SERVER_CONSOLIDATION_PLAN.md`, `archive/FLEET_CONSOLE_REFACTOR_PLAN.md` — done; the result is blinky-server's `testing/` and the fleet-first console.
- `archive/HYBRID_FEATURE_ANALYSIS_PLAN.md` — closed; its five-gate method and the v27-hybrid lessons.
- `archive/AUDIO_SYSTEM_AUDIT_2026_04_24.md` — the 2026-04 training/validation audit (all items shipped) and the v30/v31 collapse narrative.

## Documentation hygiene (per CLAUDE.md)

- Only create docs with future value: architecture specs, implementation plans, outstanding-action todo lists, testing/calibration guides.
- Do **not** create: post-mortems, vanity completion reports, "what we did" historical summaries (git log is the record), code-review notes (delete after fixes land).
- When a plan lands, either delete it or add a status header so the next reader doesn't treat it as outstanding work.
