# Blinky Time Documentation

This directory documents the architecture, plans, and reference material for the Blinky Time project. The repo-root `CLAUDE.md` holds operational behavioural rules — start there for "what am I not allowed to do." This file is the topical index.

## Live reference (read these first)

| Doc | Purpose |
|-----|---------|
| [`OPEN_ISSUES_2026_05_19.md`](OPEN_ISSUES_2026_05_19.md) | Current snapshot of unresolved work (operational bugs, PR follow-ups, BL recovery gaps, Hub UI items). |
| [`SAFETY.md`](SAFETY.md) | Flashing/recovery safety model — index into the four invariants and the current code paths. |
| [`GLOSSARY.md`](GLOSSARY.md) | Terminology — onset vs beat, ODF, plpPhase, rhythmStrength, FrameOnsetNN. |
| [`HARDWARE.md`](HARDWARE.md) | XIAO nRF52840 Sense fleet device inventory + LED pin assignments. |
| [`BUILD_GUIDE.md`](BUILD_GUIDE.md) | Build + UF2 upload procedures. nRF52840 only (ESP32-S3 deprioritised March 2026). |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | Developer workflow: pre-deploy steps, settings-version bumps, console + server dev. |

## Architecture / design

| Doc | Purpose |
|-----|---------|
| [`AUDIO_ARCHITECTURE.md`](AUDIO_ARCHITECTURE.md) | AudioTracker pipeline (multi-source ACF + epoch-fold PLP, NN-primary pulse, AudioControl 6-field output). Snapshot through v32/b149 — see header. |
| [`AUDIO-TUNING-GUIDE.md`](AUDIO-TUNING-GUIDE.md) | Tunable parameters + testing procedures. Parameter snapshot through b153. |
| [`GENERATOR_EFFECT_ARCHITECTURE.md`](GENERATOR_EFFECT_ARCHITECTURE.md) | Generator → Effect → Renderer pipeline. |
| [`VISUALIZER_GOALS.md`](VISUALIZER_GOALS.md) | Design philosophy — visual quality over metric perfection. |
| [`ML_STORAGE_LAYOUT.md`](ML_STORAGE_LAYOUT.md) | NVMe pool / SATA tier layout on the ML training server. |

## Plans (some shipped, some open — each has status header)

| Doc | Status |
|-----|--------|
| [`FLASH_LOCKDOWN_PLAN.md`](FLASH_LOCKDOWN_PLAN.md) | **CLOSED** — `FleetManager.flash_device` is the single entry point; legacy paths deleted. Kept as architectural rationale. |
| [`SCULPTURE_BLE_RECOVERY_PLAN.md`](SCULPTURE_BLE_RECOVERY_PLAN.md) | **F1–F6 all landed** — DEFAULT_TO_OTA_DFU + RebootFrequencyCounter + deferred markStable. |
| [`BLE_FLEET_RELIABILITY_PLAN.md`](BLE_FLEET_RELIABILITY_PLAN.md) | Items 1–3 shipped (PR #144); items 4–5 optional experiments. |
| [`BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md`](BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md) | BL 0.8.0-7 fixes verified; fleet rollout + second-device validation outstanding. |
| [`FLEET_CONSOLE_REFACTOR_PLAN.md`](FLEET_CONSOLE_REFACTOR_PLAN.md) | Phases 1–5 mostly shipped; M10 (URL UI) + M15 (auth-gated flash UI) + Phase 6 (Web BT) deferred. |
| [`SERVER_CONSOLIDATION_PLAN.md`](SERVER_CONSOLIDATION_PLAN.md) | Phases 1–6 largely executed; `tools/uf2_upload.py` move was skipped (still invoked from `tools/`). |
| [`LED_RENDER_OPTIMIZATION_PLAN.md`](LED_RENDER_OPTIMIZATION_PLAN.md) | Phase 3a (Nrf52PwmLedStrip async DMA) shipped; Phase 1/2 micro-opts outstanding. |
| [`BLUETOOTH_IMPLEMENTATION_PLAN.md`](BLUETOOTH_IMPLEMENTATION_PLAN.md) | BLE NUS + BLE DFU + fleet discovery shipped; post-DFU USB re-enumeration + Web BT outstanding. |
| [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md) | **Snapshot through b153/v33** — historical trail. Current state is in OPEN_ISSUES + ML_IMPROVEMENT_PLAN. |
| [`ML_IMPROVEMENT_PLAN.md`](ML_IMPROVEMENT_PLAN.md) | Current ML roadmap. 2026-05-02 literature reframe sets next experiments (hand-curate GT, per-class F1, then hybrid supervision). |

## Reference / research

| Doc | Purpose |
|-----|---------|
| [`ONSET_DETECTION_LITERATURE_2026_05_02.md`](ONSET_DETECTION_LITERATURE_2026_05_02.md) | Published onset/drum-detection systems with citations — quote before claiming "the literature says X". |
| [`AUDIO_SYSTEM_AUDIT_2026_04_24.md`](AUDIO_SYSTEM_AUDIT_2026_04_24.md) | Tier-1 instrumentation audit (all items shipped) + v30/v31 collapse diagnostic narrative. Historical. |
| [`HYBRID_FEATURE_ANALYSIS_PLAN.md`](HYBRID_FEATURE_ANALYSIS_PLAN.md) | Five-gate methodology + v27-hybrid retroactive regression findings. Historical. |
| [`RFC_MUSICAL_PATTERN_VISUALIZATION.md`](RFC_MUSICAL_PATTERN_VISUALIZATION.md) | **IMPLEMENTED** — PLP-replaces-PLL design rationale. AudioTracker v8 shipped this. |
| [`TESTING_METHODOLOGY.md`](TESTING_METHODOLOGY.md) | Tiered testing approach (smoke / quick A/B / reliable A/B / full validation). |
| [`VISUALIZATION_SAFETY_TESTING.md`](VISUALIZATION_SAFETY_TESTING.md) | Render-pipeline safety (frame clearing, brightness bounds, current-limit envelope). |

## Archive

Docs that no longer reflect the current system are in [`archive/`](archive/). They are kept for the diagnostic narrative they contain; **do not** treat them as current guidance:

- `archive/UPLOAD_OVERHAUL_PLAN.md`, `archive/UPLOAD_RELIABILITY_PLAN.md` — completed April 2026; the RAM-magic mechanism described still works and lives in firmware.
- `archive/FLASH_LOCKDOWN_AUDIT.md` — migration gap inventory; the migration finished.
- `archive/PHASE_CONFIDENCE_ARCHITECTURE.md` — explicitly superseded by RFC_MUSICAL_PATTERN_VISUALIZATION.md (PLL abandoned).
- `archive/SAFETY_pre_2026_05.md` — pre-FlashLockdown safety doc, replaced by `SAFETY.md`.
- `archive/POSTMORTEM_2026_05_18_LEDTYPE.md` — single-incident postmortem; lessons landed in registry CI lint + Nrf52PwmLedStrip offset validation.

## Documentation hygiene (per CLAUDE.md)

- Only create docs with future value: architecture specs, implementation plans, outstanding-action todo lists, testing/calibration guides.
- Do **not** create: post-mortems, vanity completion reports, "what we did" historical summaries (git log is the record), code-review notes (delete after fixes land).
- When a plan lands, either delete it or add a status header so the next reader doesn't treat it as outstanding work.
