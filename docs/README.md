# Blinky Time Documentation

This directory documents the architecture, plans, and reference material for the Blinky Time project. The repo-root `CLAUDE.md` holds operational behavioural rules — start there for "what am I not allowed to do." This file is the topical index.

Each plan carries a status header; check it before treating the plan as outstanding work. **GitHub issues are the system of record** for open work.

## Live reference (read these first)

| Doc | Purpose |
|-----|---------|
| [`OPEN_ISSUES_2026_10_15.md`](OPEN_ISSUES_2026_10_15.md) | Current index of open issues by area (rhythm-tracking drift, record mode, brick risk, CI, device checks); each item links its GitHub issue. |
| [`SAFETY.md`](SAFETY.md) | Flashing/recovery safety model — short index of the invariants and where each is enforced. |
| [`RELEASE_PROCESS.md`](RELEASE_PROCESS.md) | Firmware release channels (stable / candidate / dev), the pinned toolchain, cutting, bench-gating, promoting and rolling back releases. |
| [`BUILD_GUIDE.md`](BUILD_GUIDE.md) | Build setup (pinned toolchain), flashing paths and runtime device configuration. nRF52840 only. |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | Developer workflow: git hooks, settings-version bumps, console + server dev. |
| [`HARDWARE.md`](HARDWARE.md) | XIAO nRF52840 Sense device inventory, LED pin assignments, wiring. |
| [`GLOSSARY.md`](GLOSSARY.md) | Terminology — onset vs beat, ODF, pulse, phase, rhythmStrength, FrameOnsetNN. |
| [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md) | Where the system stands (verified against code) and what is left to do, plus a "don't retry" list. |

## Architecture / design

| Doc | Purpose |
|-----|---------|
| [`AUDIO_ARCHITECTURE.md`](AUDIO_ARCHITECTURE.md) | AudioTracker: mic normalization, onset detection (spectral flux on devices), multi-source ACF period detection, PLP epoch-fold pattern, `AudioControl` output. |
| [`AUDIO-TUNING-GUIDE.md`](AUDIO-TUNING-GUIDE.md) | Tunable audio parameters with current defaults, test procedures, troubleshooting. |
| [`GENERATOR_EFFECT_ARCHITECTURE.md`](GENERATOR_EFFECT_ARCHITECTURE.md) | Generator → Effect → Renderer pipeline (`RenderPipeline`). |
| [`VISUALIZER_GOALS.md`](VISUALIZER_GOALS.md) | Design philosophy — visual quality over metric perfection; detector vs. visual decisions. |
| [`WEB_FLASHER.md`](WEB_FLASHER.md) | Browser firmware updates at `blinky-time.joshwentworth.com/#firmware`: signed QSPI OTA over USB (Bluetooth built but hidden), signing, release mirror, rollout checklist, open items. |
| [`ML_STORAGE_LAYOUT.md`](ML_STORAGE_LAYOUT.md) | NVMe pool / SATA tier layout for ML data on the training server; disk budget; recovery. |

## Plans (some shipped, some open — each has a status header)

| Doc | Status |
|-----|--------|
| [`NEXTGEN_ESP32S3_PLAN.md`](NEXTGEN_ESP32S3_PLAN.md) | **Planned** — next-gen fixtures on the XIAO ESP32-S3 Sense, dual-core (renderer on one core, audio on the other). Doesn't change the deployed fleet. |
| [`TEST_BOX_BUILD_PLAN.md`](TEST_BOX_BUILD_PLAN.md) | **Planned** — sound-isolated test box: Pi 5 host, speaker, camera, test chips with SWD; acceptance gates. |
| [`PHONE_RECORD_MODE_PLAN.md`](PHONE_RECORD_MODE_PLAN.md) | **Implemented, not yet run on hardware** — Android Chrome ⇄ one device over Web Bluetooth, raw mic PCM + analysis. |
| [`ML_IMPROVEMENT_PLAN.md`](ML_IMPROVEMENT_PLAN.md) | Current ML roadmap. The onset NN is off on devices until a model beats spectral flux. |
| [`BLE_FLEET_RELIABILITY_PLAN.md`](BLE_FLEET_RELIABILITY_PLAN.md) | Items 1, 2, 3 and 5 shipped; field measurements (#221, #190) left. |
| [`BLUETOOTH_IMPLEMENTATION_PLAN.md`](BLUETOOTH_IMPLEMENTATION_PLAN.md) | BLE fleet management done and in daily use; mostly the build record. ESP32-S3/WiFi parts cut. |
| [`BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md`](BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md) | Bootloader fixes through 0.8.0-10 landed and bench-verified; fleet rollout of 0.8.0-10 pending. |
| [`FLASH_LOCKDOWN_PLAN.md`](FLASH_LOCKDOWN_PLAN.md) | **CLOSED** — `FleetManager.flash_device` is the single entry point; legacy paths deleted. Kept as architectural rationale. |
| [`SCULPTURE_BLE_RECOVERY_PLAN.md`](SCULPTURE_BLE_RECOVERY_PLAN.md) | **Done** — F1–F6 landed (DEFAULT_TO_OTA_DFU, RebootFrequencyCounter, deferred markStable). |
| [`FLEET_CONSOLE_REFACTOR_PLAN.md`](FLEET_CONSOLE_REFACTOR_PLAN.md) | **Done** apart from M10 (deferred) and M16's discovery side. |
| [`SERVER_CONSOLIDATION_PLAN.md`](SERVER_CONSOLIDATION_PLAN.md) | **Done** apart from two listed exceptions; testing lives in `blinky_server/testing/`. |
| [`LED_RENDER_OPTIMIZATION_PLAN.md`](LED_RENDER_OPTIMIZATION_PLAN.md) | **Mostly done**; 2b (write straight into the output buffer) partly done. |

## Reference / research

| Doc | Purpose |
|-----|---------|
| [`ONSET_DETECTION_LITERATURE_2026_05_02.md`](ONSET_DETECTION_LITERATURE_2026_05_02.md) | Published onset/drum-detection systems with citations — quote before claiming "the literature says X". |
| [`WEB_UPDATE_RESEARCH_2026_10.md`](WEB_UPDATE_RESEARCH_2026_10.md) | Research input for the web updater: platform constraints (iPhone, Windows UF2, Chrome's DFU blocklist) and the options weighed. Not the current design; see WEB_FLASHER.md. |
| [`TESTING_METHODOLOGY.md`](TESTING_METHODOLOGY.md) | Tiered testing approach, from the host harness up to full on-device validation. |
| [`VISUALIZATION_SAFETY_TESTING.md`](VISUALIZATION_SAFETY_TESTING.md) | Render-pipeline safety (frame clearing, brightness bounds); the safety test header exists but is not wired up yet. |
| [`AUDIO_SYSTEM_AUDIT_2026_04_24.md`](AUDIO_SYSTEM_AUDIT_2026_04_24.md) | Tier-1 instrumentation audit (all items shipped) + v30/v31 collapse diagnostic narrative. Historical. |
| [`HYBRID_FEATURE_ANALYSIS_PLAN.md`](HYBRID_FEATURE_ANALYSIS_PLAN.md) | Closed; kept for its five-gate method and the v27-hybrid lessons. |
| [`RFC_MUSICAL_PATTERN_VISUALIZATION.md`](RFC_MUSICAL_PATTERN_VISUALIZATION.md) | Phase 1 (PLP replaces PLL) implemented, the rest not pursued. Design rationale only; read AUDIO_ARCHITECTURE.md for current behavior. |

Image files (`fire-*.gif`, `fireGifs/`) are old fire-effect previews; no doc references them.

## Archive

Docs that no longer reflect the current system are in [`archive/`](archive/). They are kept for the diagnostic narrative they contain; **do not** treat them as current guidance:

- `archive/OPEN_ISSUES_2026_05_19.md`, `archive/OPEN_ISSUES_2026_05_20.md` — earlier open-issue snapshots; items still open from 2026-05-20 were carried into `OPEN_ISSUES_2026_10_15.md` §8.
- `archive/UPLOAD_OVERHAUL_PLAN.md`, `archive/UPLOAD_RELIABILITY_PLAN.md` — completed April 2026; the RAM-magic mechanism described still works and lives in firmware.
- `archive/FLASH_LOCKDOWN_AUDIT.md` — migration gap inventory; the migration finished.
- `archive/PHASE_CONFIDENCE_ARCHITECTURE.md` — explicitly superseded by RFC_MUSICAL_PATTERN_VISUALIZATION.md (PLL abandoned).
- `archive/SAFETY_pre_2026_05.md` — pre-FlashLockdown safety doc, replaced by `SAFETY.md`.
- `archive/POSTMORTEM_2026_05_18_LEDTYPE.md` — single-incident postmortem; lessons landed in registry CI lint + Nrf52PwmLedStrip offset validation.

## Documentation hygiene (per CLAUDE.md)

- Only create docs with future value: architecture specs, implementation plans, outstanding-action todo lists, testing/calibration guides.
- Do **not** create: post-mortems, vanity completion reports, "what we did" historical summaries (git log is the record), code-review notes (delete after fixes land).
- When a plan lands, either delete it or add a status header so the next reader doesn't treat it as outstanding work.
