# Claude Code Instructions for Blinky Project

This file contains **critical behavior rules only**. Architecture, status, and historical context live in `docs/` (see the index at the bottom).

> ESP32-S3 support was cut March 2026. All active development targets **nRF52840**.

## CRITICAL: Upload Safety (nRF52840)

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial` on nRF52840.** The DFU serial protocol has race conditions that can brick devices, requiring SWD hardware to recover.

**Flashing connected devices: `./scripts/deploy.sh` is REQUIRED.** Direct `curl` against `/api/fleet/upload`, `/api/fleet/flash`, or `/api/devices/{id}/command` with a device-mutating command body (`device upload`, `reboot`) is forbidden — the server enforces this via an `X-Deploy-Tool` header check that only `deploy.sh` sets, so manual curl returns 403. The API key alone is not sufficient. Use:

`./scripts/deploy.sh` requires an explicit `--devices` target — passing `all`, an explicit comma-separated list of device IDs, or `list` to enumerate candidates. Unscoped fleet flashes were removed in PR #140 after the cart_inner brick incident.

```bash
./scripts/deploy.sh --devices=all                   # compile + upload + flash all devices + verify
./scripts/deploy.sh --devices=cart_inner,cart_outer # scope to a subset
./scripts/deploy.sh --devices=list                  # show candidates and exit
./scripts/deploy.sh --devices=all --skip-compile    # deploy already-compiled hex
./scripts/deploy.sh --devices=all --no-bump         # recompile without bumping build number
```

deploy.sh runs the full pipeline (compile → upload → flash → version-verify) and fails loud on any error. Bypassing it (even just to "save time") loses the version-verify step that catches partial flashes.

**The deploy.sh-only rule is broader than firmware flashing.** It also covers any sequence of API calls that mutates persistent device state or device lifecycle: `device upload` (writes config to flash), `reboot`, `wipe_device_identity` (and its deprecated aliases `factory` / `reset`), fleet-wide config sweeps that loop over multiple settings, etc. Even though those go through `/api/devices/{id}/command` (not `/api/fleet/upload`), running them in an ad-hoc loop without a tested workflow leaves devices in `error`/`connecting` states and requires manual recovery. The server enforces this for the destructive subset (`device upload`, `reboot`, `wipe_device_identity`, `factory`, `reset` — see `blinky_server/api/deps.py:_DEPLOY_GATED_COMMAND_LIST` for the canonical list); for everything else the discipline is on the operator. **If a real test needs different device configurations, build firmware variants with each config baked in and deploy each via `deploy.sh` — do NOT script API config flips on running fleet devices.**

**Bare-chip recovery only:** `make uf2-upload UPLOAD_PORT=/dev/ttyACM0` (use when a device is bricked and not yet enrolled in the fleet).

blinky-server owns all serial/BLE connections — no port contention. See `docs/SAFETY.md` for the full safety model, `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` for BLE DFU details.

**Risky firmware:** always test on unenclosed bare chips first (reset button + SWD pads accessible). Installed devices have no physical access.

**Unresponsive device recovery:** `uhubctl -a cycle -p <port>` → server flash → wait for re-enumeration. Last resort: physical reset.

## CRITICAL: Bootloader changes are higher-stakes than firmware changes

**Bootloader updates go through `./scripts/deploy-bootloader.sh`, not deploy.sh or direct UF2 drops.** That wrapper enforces three safety layers introduced after the 2026-05-15 hat-bricking incident:

1. **Source-level invariant check** (`scripts/verify_bootloader.py`): refuses to flash any BL whose `check_dfu_mode()` has an `if (_ota_dfu)` branch that does not initialize USB. This is the literal failure mode that bricked the hat — a BL change removed `usb_init()` from the OTA path, and when the firmware's `RebootFrequencyCounter` auto-quarantine triggered `DEFAULT_TO_OTA_DFU`, the BL came up with BLE only. No BLE host nearby → device unreachable → SWD required.
2. **Pre-flash version log**: captures the running BL's `INFO_UF2.TXT` so you know exactly what you're rolling back from if something goes wrong.
3. **Post-flash verification**: re-enters DFU after the device reboots and confirms the new BL version is what was intended.

**The invariant the verifier enforces** (do not break this without understanding why): *every code path inside `check_dfu_mode()` that reaches `bootloader_dfu_start()` must first initialize USB*. The bootloader has both a USB MSC transport (UF2 drag-drop) and a BLE OTA transport. Either can be the operator's reach to the device. Removing USB from any DFU entry path — even one that "seems like it'd only happen with BLE" — silently breaks recovery for the subset of devices that fall into that path without a BLE host nearby. Per the 2026-05-15 incident, this includes the `DEFAULT_TO_OTA_DFU` auto-recovery on invalid-app boot.

**SWD recovery for fully-bricked devices.** A Raspberry Pi Zero W at `swd-flash.local` is provisioned for SWD recovery: `ssh swd-flash.local` then `cd ~/swd-recovery && cat README.md` for wiring + commands. The Pi has the known-good fleet BL (0.8.0-4-g76d1e60), MBR, and SoftDevice S140 7.3.0 hex files staged. Use this as the recovery of last resort when a device drops off USB and isn't reachable via BLE.

**Test bootloaders on a SACRIFICIAL device.** Do not flash an experimental BL to the only available test device. If only one device is on the bench, build the BL change against the known-good (`git log blinky-local-patches` to find the last-stable BL commit) and verify with `scripts/verify_bootloader.py` BEFORE flashing. The verifier catches the specific bug class that bricked the hat; pass it before committing to a flash.

**Current fleet BL: 0.8.0-9-g0a2b140** (staged in `bootloader/update-bootloader-qspi-ota-default.uf2`; source is pinned at this commit via the `bootloader/src` submodule → `github.com/Jdubz/Adafruit_nRF52_Bootloader` `blinky-local-patches`. Fresh workstation: `git submodule update --init --recursive bootloader/src`). On top of 0.8.0-4's dual-transport baseline, 0.8.0-9 adds:
- `bootloader_settings_save` SD-aware (no more silent fail on UF2 path → was 80% hiccup pre-fix)
- BLE adv paused during USB MSC transfer + ADV_SET_TERMINATED auto-restart guard (eliminates 5-90s slow-completion tail)
- `msc_uf2_check_stuck()` self-recovery (8s idle + incomplete → DFU_RESET → boot to app; converts permanent stuck into ~13s recovery)
- App-handshake watchdog (OPEN_ISSUES §3.1): RAM counter at `0x20007F78` bumped on every app-jump, cleared by firmware at 60 s uptime; after 3 consecutive misses BL forces DFU. Closes the "crashy-but-valid app" recovery gap. Firmware-side clear is forward-compatible — pre-§3.1 firmwares are still safe on this BL.
- Bench measurement: 30/30 PASS at 30s timeout, mean 9.4s per UF2 drop (vs. unbounded hangs on 0.8.0-5).
See `docs/BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md` for the diagnostic narrative.

## CRITICAL: Single Flash Entry Point (blinky-server)

**Inside blinky-server, every flash routes through `FleetManager.flash_device()` → `_run_flash_job` → the guarded write impl. There is exactly one entry point; direct calls to the `_impl` functions are forbidden by design and raise `OutsideFlashJobContextError` at runtime.**

This is the structural fix for the 2026-05-17 cart_inner cascade: the legacy fleet had multiple parallel flash paths (`upload_firmware`, `upload_via_uf2`, `upload_uf2`, `upload_ble_dfu`, plus auto-recovery's separate code), each maintaining its own dedup state. A UF2 write succeeded but the route's verify timed out, the route then marked it FAILED, and auto-recovery's separate state machine saw "DFU device, not recently flashed" and re-flashed via BLE-DFU — over a successful UF2 write. Closing every parallel path is what makes the recurrence impossible. See `docs/FLASH_LOCKDOWN_PLAN.md` for the full plan + sequencing.

**The invariant** (do not break this without understanding why):

1. **There is exactly one public entry point: `FleetManager.flash_device(device_id, firmware_path, *, force=True)`.** Operator-facing routes (`POST /api/devices/{id}/flash`, `POST /api/fleet/flash` via `FleetManager.flash_fleet()`, `POST /api/flash-jobs` for explicit job creation) call it with `force=True` (default). The auto-recovery loop calls it with `force=False`. There is no other caller in production code.
2. **`force=False` consults dedup**: returns `None` if `should_attempt_auto_recovery` says no (in-flight, recent attempt within `FLASH_DEDUP_WINDOW_S=600s`, max attempts hit, or inside exponential backoff). `force=True` ignores those checks — the operator already accepted the cost.
3. **The actual writes live in guarded `_impl` functions**: `firmware.uf2_upload._uf2_write_impl_for_job` and `firmware.ble_dfu._ble_dfu_write_impl`. Each is `_`-prefixed, has `assert_inside_orchestrator(...)` as its first line, and raises `OutsideFlashJobContextError` if called outside `_run_flash_job` (the orchestrator that sets the `_inside_flash_job_orchestrator` ContextVar). The legacy public wrappers (`upload_uf2`, `upload_ble_dfu`, `upload_firmware`, `upload_via_uf2`) are deleted.
4. **The audit log is the persistent ledger.** Every terminal FlashJob writes one JSONL line to `~/.local/share/blinky-server/flash_attempts.jsonl`. On server start, the loader rebuilds `_recent_flash_attempts` from entries within the dedup window. The 10-minute "I just flashed this device" window survives restarts.

**Adding a new flash caller** (operator-facing, future MCP tool, etc.):
- Call `await fleet.flash_device(device_id, firmware_path)` with `force=True`.
- Wait on the returned `FlashJob` via `await job.wait_until_terminal(timeout=...)`.
- Inspect `job.state` for COMPLETED vs FAILED/ABANDONED, `job.error` for the failure detail.
- Do NOT import or call `_uf2_write_impl_for_job` / `_ble_dfu_write_impl` directly — the ContextVar guard will refuse them.

**Adding a new auto-recovery-style caller** (something that should respect dedup + max-attempts + backoff):
- Call with `force=False`. Returned `FlashJob` may be `None` if blocked.
- Don't track your own retry counter; the orchestrator's `_recovery_retry_state` is the single source of truth.

**Routes that mutate device state but aren't strictly "flashing"** still gate on `X-Deploy-Tool` (see the top of this file). Examples: `device upload <JSON>` (writes config to flash), `reboot`, `wipe_device_identity`. Those go through `/api/devices/{id}/command`, not `flash_device`. They have their own gating (`_DEPLOY_GATED_COMMAND_LIST` in `blinky_server/api/deps.py`) and should NOT route through the flash orchestrator — flashing is destructive in a specific way (writes the application slot of flash memory) that the orchestrator is calibrated for.

**Audit-trail use:** `flash_attempts.jsonl` is also a forensic record. If a device's behavior since deploy is suspicious, `grep <device_id> ~/.local/share/blinky-server/flash_attempts.jsonl` gives the canonical history of every flash that touched it, with timestamps, transport, source (operator vs auto-recovery), and final state.

## CRITICAL: 60-second rule between device resets

The firmware's `SafeBootWatchdog::markStable()` is **deferred to 60s of uptime** (`blinky-things.ino:613-614`) so that mid-init crashes still count toward the BLE-DFU recovery threshold (F3 in SCULPTURE_BLE_RECOVERY_PLAN.md). The corollary: **any sequence of resets faster than 60s apart bumps `RebootFrequencyCounter` and, at 5 cumulative trips, fires the quarantine hook (`configStorage.quarantineDeviceConfig()` in `.ino:189`)**. That hook flips `data_.device.isValid = false`, and the device boots into safeMode with `{"status":"unconfigured"}` the next time around — even though nothing actually crashed.

This burns hours when you don't know about it: you flash a BL, drop a UF2, send `reboot` over CDC, do another `bootloader` cmd, etc. — each is a real reset, the counter accumulates silently, and the device suddenly forgets its config. Re-uploading the config via `device upload` works, but only if you THEN let uptime cross 60s before the next reset.

Rules:
- **Between any two resets** (CDC `reboot`, CDC `bootloader`, SWD reset, UF2 drop), insert **at least one full 60-second uptime window** so `markStable()` fires and clears the counter.
- **Don't chain `device upload` + `reboot` + `device upload` + `reboot`** to "verify persistence" — that's the exact pattern that trips the trap. Upload once, wait >60s, optionally do one verification reboot, then wait >60s again.
- **If a previously-configured device boots into safeMode unexpectedly**, the watchdog quarantine is the most likely cause. Recovery, in order — do NOT shortcut to ad-hoc UF2 drops or direct `curl` against the device API:
  1. `./scripts/deploy.sh --devices=<id>` (or `--devices=<mac>` if not enrolled) → boots latest firmware to APP.
  2. Wait ≥75s of uptime so `SafeBootWatchdog::markStable()` fires and clears the RebootFrequencyCounter.
  3. Push the device config via the fleet API's `device upload` command — gated through deploy.sh's `X-Deploy-Tool` header (per the upload-safety section at the top of this file). If you have access only to local CDC, `device upload <JSON>` over `/dev/ttyACM0` is acceptable for the bench device but NEVER for an enrolled fleet device.
  4. Wait another ≥75s for the new config's first stable boot before any further reset.
- For BL iteration cycles (SWD-flash → bl_characterize → SWD-flash again), this is also the reason `bl_characterize.sh` works at 30s WAIT_TIMEOUT but back-to-back wait_test.sh runs are flaky — bl_characterize sleeps 2s between iters but each iter is itself >30s (timeout + recovery), keeping cycle time above 60s.

The 60s threshold is intentional and load-bearing for sculpture-device recovery. Don't tune it down; tune your test scripts up.

## CRITICAL: Task is Onset Detection, Not Beat Detection

**The model and firmware are doing ONSET DETECTION** — fire on every percussive event (kick, snare, ghost notes, hi-hats) regardless of metrical position. **We do NOT care whether an event lands on or off the beat.** Beat-detection framing is wrong for this task and creates noise — both metric noise (scoring against beat positions when the goal is to fire on every drum hit) and label noise (training on beat consensus teaches the model to suppress off-beat onsets, which we want to keep).

Concrete consequences:
- **Training labels must be onset-flavored** (`onsets_consensus`, `kick_weighted_drums`, etc.). Never use `consensus_v*` (beat tracker output) or anything from `allin1`/`beat_this`/`beatnet`/`demucs_beats` as a training target. Those directories are archived under `_archive_beats_2026_04_29/`. `prepare_dataset.py` refuses to load paths with those markers.
- **Eval ground truth is `.onsets_consensus.json`**, not `.beats.json`. The blinky-server validation harness reads onsets_consensus when present (since 2026-04-29 it requires it). Beat GT files were archived out of `blinky-test-player/music/edm/`.
- **Beat-tracking systems (allin1, beat_this, beatnet, demucs_beats, the BPM half of madmom)** still exist as reference scripts but their *output is beats* and must not flow into the training-label pipeline.
- **BPM in firmware (PLP / ACF tempo tracking)** is fine — it's used to phase-lock the visualizer animation, not to gate onset detection. `beatGridPatternMin=0.0` (b149+ default) keeps the AND-gate off so the visualizer fires on every onset, not only on beats.

When in doubt: an onset is *every percussive event*. A beat is *every metrical position*. We want the former. Beats are a strict subset of onsets, and a beat-detection model trained or scored on beats will *under-fire* on the deployment task.

## CRITICAL: Validation Corpus

**The only validation corpus is `blinky-test-player/music/edm/` (18 tracks).** It contains representative EDM with clear percussion (techno×5, trance×3, dnb×2, breakbeat×2, dubstep, garage, reggaeton, trap, afrobeat, amapiano) — the content the system is designed to perform on. Lock in performance here before measuring anything else.

**Do NOT introduce a "held-out" / "hard" / "adversarial" corpus and use it as a headline metric.** This has happened multiple times — most recently `edm_holdout/` (GiantSteps LOFI: ambient, phrase-shifting, intentionally challenging). Reporting F1 on adversarial content as the primary number normalises low scores ("F1 0.47 is fine, that's hard content") and burns weeks chasing model fixes for problems that look smaller on representative content. The adversarial-corpus pattern was deleted 2026-04-27 as part of a structural fix, not a policy.

If you need to test against harder material, do it *after* edm/ is solid and report it as a tier-2 number with separate framing, not next to the headline F1. New corpora must clear that bar before being added — and never as a default in scripts.

## CRITICAL: No Silent Fallbacks

**Errors must fail LOUDLY. Never silently substitute defaults, zero-fills, clamps, or empty values for invalid state.** A silent fallback is a production bug waiting to happen — it lets configuration drift, version mismatches, hardware faults, and model corruption masquerade as "working." When in doubt, assert, panic, or log at ERROR — never quietly continue with degraded data.

**The reference incident (2026-04-27, v33):** `MelBandDef MEL_BANDS[NUM_MEL_BANDS] = { /* 30 entries */ }` was left at the v32 size when `NUM_MEL_BANDS` bumped 30 → 50. C++ aggregate-init silently zero-filled the missing 20 entries to `{0,0,0}`, all reading FFT bin 0 (DC). The neural network received garbage in 40% of its input bands and the activation distribution collapsed. There was no compile error, no runtime error, no warning. Two weeks of training experiments and threshold sweeps were spent chasing a model-side hypothesis for a firmware-side data corruption.

**Concrete patterns to refuse:**

1. **Array literals smaller than the declared size.** Always either (a) static_assert that the initializer count equals the size, or (b) generate the full table programmatically. *Never rely on aggregate-init zero-fill for tail entries.*
2. **Ternary fallbacks that mask invalid state.** `(weightSum > 0) ? sum / weightSum : 0.0f` is wrong when `weightSum == 0` represents a configuration bug, not a runtime case — assert instead. The principled exception is denormal/silence handling where 0 is the *correct* answer (e.g., NaN guards on FFT magnitudes during true silence) — those must be commented as such.
3. **`validate*` / `clamp*` functions that silently coerce out-of-range values.** A corrupt config value should boot-loop with a logged error, not silently clamp to a default. If clamping is intentional (e.g., user-typed input), log every coercion at WARN with the original value.
4. **Functions that return `false` / `0` / `nullptr` / empty without a serial log.** Callers may not check; the symptom is a feature that "just doesn't work." If returning a sentinel is unavoidable, log on the way out.
5. **`switch` defaults / catchall `else` branches that absorb unexpected enum values.** Either handle every case explicitly or `panic("unhandled <enum>")` in the default. The whole point of the enum is exhaustiveness.
6. **`if (!initialized) { init_with_defaults(); }` patterns.** Lazy init can mask a forgotten explicit init. Fail at the call site if state is uninitialized.
7. **TFLite / TF status checks like `if (status != kTfLiteOk) return;` without a serial log.** The model silently stops inferring; the visualizer keeps running on stale activations.
8. **`#ifdef` / `#elif` chains with an unguarded `#else` that compiles to a placeholder.** Use `#error` in the unhandled case so the wrong build never ships.
9. **NaN/Inf substitution to 0** is acceptable only for known denormal cases (must be commented). For everything else, increment a counter *and* log on first occurrence.

When you discover a silent fallback, the fix is to (a) assert / panic at the failing site, (b) add a `[FALLBACK]` serial log with file:line and the offending value, or (c) add a compile-time check that prevents the bad state. Do not just "improve" the comment.

## CRITICAL: Long-Running Scripts

**NEVER run ML training or other long-running scripts as Claude session tasks** — they die when the session ends.

Always use tmux:
```bash
# Output dir defaults to /mnt/nvme/outputs (training server). On a laptop or
# any machine without /mnt/nvme mounted, override with BLINKY_OUTPUT_DIR=...
# before the command, or substitute a local path directly.
OUT="${BLINKY_OUTPUT_DIR:-/mnt/nvme/outputs}"
tmux new-session -d -s training "cd ml-training && source venv/bin/activate && PYTHONUNBUFFERED=1 python train.py --config configs/<name>.yaml --output-dir $OUT/<experiment> 2>&1 | tee $OUT/<experiment>/training.log"
```

`train.py` enforces this — it refuses to start outside tmux/screen unless `--allow-foreground` is passed.

## Build Workflow

**Always use `scripts/build.sh` to compile.** It auto-increments `blinky-things/BUILD_NUMBER` and regenerates `types/Version.h`. Build numbers are the source of truth for deployed firmware — git is for collaboration, not deployment.

```bash
./scripts/build.sh                          # nRF52840 compile only
./scripts/build.sh --upload /dev/ttyACM0    # compile + UF2 upload
./scripts/build.sh --no-bump                # recompile without incrementing (e.g. after failed upload)
```

Use `./scripts/deploy.sh` for full compile → upload → fleet flash → verify.

## Device Workflow Discipline

- Don't ad-hoc flash or reconfigure devices; use scripted workflows.
- Validation resets devices to defaults before each run — device settings persist across flashes and can corrupt tests otherwise.
- `run_test` / `run_validation_suite` / `check_test_result` MCP tools submit jobs to blinky-server; no direct port management needed.

## Documentation Guidelines

**Only create docs with future value:** architecture specs, implementation plans, outstanding-action todo lists, testing/calibration guides.

**Do NOT create:**
- Code review documents (delete after fixes land)
- Analysis reports of completed work (vanity docs)
- Historical "what we did" summaries (git log is the record)
- Post-mortems (capture lessons in architecture docs or commit messages)

Reviews and analysis must focus on **outstanding actions**, not documenting past work.

## Documentation Index

| Document | Purpose |
|----------|---------|
| `docs/VISUALIZER_GOALS.md` | Design philosophy — visual quality over metrics |
| `docs/AUDIO_ARCHITECTURE.md` | AudioTracker, FrameOnsetNN, PLP, spectral analysis |
| `docs/ML_STORAGE_LAYOUT.md` | NVMe pool / SATA tier layout for ML data; disk budget; recovery |
| `docs/AUDIO-TUNING-GUIDE.md` | Main testing guide, tunable params, test procedures |
| `docs/IMPROVEMENT_PLAN.md` | Current status and roadmap |
| `docs/ML_IMPROVEMENT_PLAN.md` | NN training roadmap and experiment history |
| `docs/ONSET_DETECTION_LITERATURE_2026_05_02.md` | Published onset/drum-detection systems with F1 numbers, architectures, label sources — quote this before claiming "the literature says X" |
| `docs/AUDIO_SYSTEM_AUDIT_2026_04_24.md` | Point-in-time audit of training/firmware/validation pipelines with prioritized fix plan (tasks #77–#88) |
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator / Effect / Renderer pattern |
| `docs/HARDWARE.md` | XIAO nRF52840 Sense hardware specs |
| `docs/BUILD_GUIDE.md` | Build and installation instructions |
| `docs/DEVELOPMENT.md` | Development guide, config management |
| `docs/SAFETY.md` | Flashing safety mechanisms |
| `docs/FLASH_LOCKDOWN_PLAN.md` | blinky-server's single-flash-entry-point lockdown: the plan + sequencing for the orchestrator architecture |
| `docs/BLE_FLEET_RELIABILITY_PLAN.md` | Remaining work to make BLE fleet commands hit every device every time; covers firmware-side rxBuffer, command-id idempotency, broadcaster-dongle option |
| `docs/OPEN_ISSUES_2026_05_19.md` | Snapshot of all unresolved work at the end of the 2026-05-19 marathon session — operational bugs, PR review remainders, BL recovery gaps, service hardening gaps, Hub UI follow-ups |
| `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` | BLE, WiFi, OTA, fleet server |
| `docs/SCULPTURE_BLE_RECOVERY_PLAN.md` | Pre-install brick-proofing for sealed sculpture devices: bootloader DEFAULT_TO_OTA_DFU + watchdog/SafeMode fixes |
| `docs/FLEET_CONSOLE_REFACTOR_PLAN.md` | blinky-console refactor roadmap |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Historical calibration results |
| `blinky-test-player/NEXT_TESTS.md` | Priority testing tasks |
