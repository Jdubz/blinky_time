# Claude Code Instructions for Blinky Project

This file contains **critical behavior rules only**. Architecture, status, and historical context live in `docs/` (see the index at the bottom).

> ESP32-S3 support was cut March 2026. All active development targets **nRF52840** (the deployed fleet). The **next generation** of fixtures will be built on the XIAO ESP32-S3 Sense, dual-core (renderer on one core, audio listener on the other): see `docs/NEXTGEN_ESP32S3_PLAN.md`. Don't change deployed-fleet code for it.

## CRITICAL: Upload Safety (nRF52840)

**NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial` on nRF52840.** The DFU serial protocol has race conditions that can brick devices, requiring SWD hardware to recover.

**Flashing connected devices: every deploy must run deploy.sh's hardened pipeline.** What's required is the mechanisms, not the script's name:
- a **stable release only**, downloaded and sha256-verified (`tools/firmware_release.py download`);
- an **explicit target list** (or `all`), with every ID validated;
- the **single flash path** (`FleetManager.flash_device` / `flash_fleet`), with auto-recovery armed for exactly the targets;
- `restore_runtime_settings` + `save`;
- the **post-deploy assertion** (exact version, audio counters, no error/DFU state).

Dropping any of these is what's forbidden. Two entry points run all of them:
- `./scripts/deploy.sh` from a workstation;
- `POST /api/fleet/deploy` on blinky-server (`blinky_server/firmware/deploy_pipeline.py`, the same modules; #269). It never compiles, and never flashes a local build.

Direct `curl` against the low-level `/api/fleet/upload`, `/api/fleet/flash`, or `/api/devices/{id}/command` with a device-mutating command (`device upload`, `reboot`) skips the pipeline, so it's forbidden. The server enforces this with an `X-Deploy-Tool` header that only `deploy.sh` sets: manual curl gets 403, and the API key alone isn't enough. When adding a new deploy path, build it on the same modules rather than reimplementing or skipping a step. From a workstation, use:

`./scripts/deploy.sh` requires an explicit `--devices` target — passing `all`, an explicit comma-separated list of device IDs, or `list` to enumerate candidates. Unscoped fleet flashes were removed in PR #140 after the cart_inner brick incident.

```bash
./scripts/deploy.sh --devices=all                   # latest STABLE release → all devices + verify
./scripts/deploy.sh --devices=cart_inner,cart_outer # scope to a subset
./scripts/deploy.sh --devices=list                  # show candidates and exit
./scripts/deploy.sh --devices=ID --release b214     # a specific STABLE release (deploy.sh is stable-only, #251)
```

Candidates and `--dev` builds go only through `tools/deploy_local.py` to a test device on this machine's USB.

deploy.sh runs the full pipeline (select firmware → upload → flash → version-verify) and fails loud on any error. Bypassing it (even just to "save time") loses the version-verify step that catches partial flashes.

## CRITICAL: Only stable releases reach fleet and new devices

**Fleet and new devices get the latest *stable* GitHub release, never a local build.** See `docs/RELEASE_PROCESS.md`.

**Releases are vetted checkpoints the owner selects.** A merge to master never triggers or implies one. Cut a candidate only when the owner asks for a release.

The flow, when the owner asks:
1. The chosen commit is on master, with CI green.
2. `gh workflow run release.yml` builds a **candidate** (a pre-release `bN`) with the pinned toolchain.
3. `tools/release_gate.py bN` bench-gates it on a test device: version, fps (≥ 60), 5 min stability, asserts, music, operator confirmation.
4. `tools/release_promote.py bN` has CI sign it (after the gate), then makes it **stable** and *Latest*.

**Firmware that isn't stable** (candidates, and `--dev` builds of a clean tree, versioned `bN-dev-<sha>`) goes to exactly one device listed in `devices/test_devices.json`. The deploy tools refuse every other device. Never flash an uncommitted or unreleased build to any other device "just to try it": that is how b210 (12 fps) and b211 (silent mic) reached the hat.

**The deploy.sh-only rule is broader than firmware flashing.** It also covers any sequence of API calls that mutates persistent device state or device lifecycle: `device upload` (writes config to flash), `reboot`, `wipe_device_identity` (and its deprecated aliases `factory` / `reset`), fleet-wide config sweeps that loop over multiple settings, etc. Even though those go through `/api/devices/{id}/command` (not `/api/fleet/upload`), running them in an ad-hoc loop without a tested workflow leaves devices in `error`/`connecting` states and requires manual recovery. The server enforces this for the destructive subset (`device upload`, `reboot`, `wipe_device_identity`, `factory`, `reset` — see `blinky_server/api/deps.py:_DEPLOY_GATED_COMMAND_LIST` for the canonical list); for everything else the discipline is on the operator. **If a real test needs different device configurations, build firmware variants with each config baked in and deploy each via `deploy.sh` — do NOT script API config flips on running fleet devices.**

**Bare-chip recovery only:** `make uf2-upload UPLOAD_PORT=/dev/ttyACM0` (use when a device is bricked and not yet enrolled in the fleet).

**Device on THIS machine's USB (not blinkyhost's):** `python tools/deploy_local.py --port COM4 --device-id <id>`. It carries deploy.sh's gates locally:
- **Explicit target:** `json info` `device.id` must match `--device-id`.
- **Pre-flight:** the device must be configured, with ≥ 75 s uptime if its `json info` reports `resetReason` (its reboot counter counts only fault resets), else ≥ 5 min 15 s (older firmware counts every boot until 5 min of uptime).
- **Firmware:** the latest stable release by default (downloaded, sha256-verified). `--release bN` selects a specific release, and `--dev` a local build of a clean tree (test devices only). Then the server's hex/UF2 validators run.
- **Bootloader identity before writing:** the drive's USB serial must equal the chip's, and the bootloader must be 0.8.0-10.
- **Post-deploy:** version, identity, config and BLE are checked *before* `save`, then `restore_runtime_settings` + `save` and a 75 s stability wait.
- **Audit:** every attempt is logged to `~/.local/share/blinky/local_flash_attempts.jsonl`.

Use `--dry-run` first. If an identity check refuses the drive, the bootloader times out back to the old app on its own.

**A personal device, from a browser (anyone, no repo):** `blinky-time.joshwentworth.com/#firmware` (blinky-console `FirmwarePage`; live since 2026-10-21 (PR #266), USB only. The Firebase deploy sets `VITE_FIRMWARE_UPDATES=on`, and removing that is the kill switch). It installs **stable releases only**, through the device's **signed OTA** path over USB (Web Serial; Bluetooth/NUS is built but hidden, `BLE_UPDATES_ENABLED`, until it passes on hardware): an Ed25519-signed manifest, then the image staged in QSPI while the old firmware runs, then `ota commit` (re-verified) and the bootloader installs it. It runs deploy_local.py's gates in the browser, except the uptime holds: it waits for 75 s of uptime (not 5 min 15 s) before resetting, since signed OTA only runs on b215+, whose reboot counter counts only fault resets (not the soft reset `ota commit` ends in); it watches the first 75 s after an update in the background instead of blocking. Don't weaken a gate in `blinky-console/src/services/firmware/` or `blinky-things/ota/` without changing the matching Python tool first. It is not a fleet-ops tool; use deploy.sh for the fleet. Design, bench results, rollout checklist and open items: `docs/WEB_FLASHER.md`.

**Release signing key:** the private key is the secret `BLINKY_RELEASE_SIGNING_KEY` of the GitHub environment `release-signing` (branch policy: `master` only, so no other branch's workflow can read it; never make it a repository secret), used only by `sign-release.yml`, plus the maintainer's offline backup. **Only gated releases are signed:** every device installs a validly signed newer build, so `release_promote.py` has CI sign a release only after its bench gate passes; candidates are never signed. Its public half is compiled into the firmware (`blinky-things/ota/OtaKeys.h`); `firmware_release.py sign` refuses any other key. Never sign a dev or test build with it: bench the update path with `scripts/build.sh --dev --ota-bench-key` and the bench key (`~/.blinky/ota-bench-key.pem`). Losing the release key means devices can never accept a signed update again; rotating it means shipping, signed with the old key, a firmware that carries the new public key.

blinky-server owns all serial/BLE connections — no port contention. See `docs/SAFETY.md` for the full safety model, `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` for BLE DFU details.

**Risky firmware:** always test on unenclosed bare chips first (reset button + SWD pads accessible). Installed devices have no physical access.

**Unresponsive device recovery:** `uhubctl -a cycle -p <port>` → server flash → wait for re-enumeration. Last resort: physical reset.

## CRITICAL: Bootloader changes are higher-stakes than firmware changes

**Bootloader updates go through `./scripts/deploy-bootloader.sh`, not deploy.sh or direct UF2 drops.** That wrapper enforces three safety layers introduced after the 2026-05-15 hat-bricking incident:

1. **Source-level invariant check** (`scripts/verify_bootloader.py`): refuses to flash any BL whose `check_dfu_mode()` has an `if (_ota_dfu)` branch that does not initialize USB. This is the literal failure mode that bricked the hat — a BL change removed `usb_init()` from the OTA path, and when the firmware's `RebootFrequencyCounter` auto-quarantine triggered `DEFAULT_TO_OTA_DFU`, the BL came up with BLE only. No BLE host nearby → device unreachable → SWD required.
2. **Pre-flash version log**: captures the running BL's `INFO_UF2.TXT` so you know exactly what you're rolling back from if something goes wrong.
3. **Post-flash verification**: re-enters DFU after the device reboots and confirms the new BL version is what was intended.

**The invariant the verifier enforces** (do not break this without understanding why): *every code path inside `check_dfu_mode()` that reaches `bootloader_dfu_start()` must first initialize USB*. The bootloader has both a USB MSC transport (UF2 drag-drop) and a BLE OTA transport. Either can be the operator's reach to the device. Removing USB from any DFU entry path — even one that "seems like it'd only happen with BLE" — silently breaks recovery for the subset of devices that fall into that path without a BLE host nearby. Per the 2026-05-15 incident, this includes the `DEFAULT_TO_OTA_DFU` auto-recovery on invalid-app boot.

**SWD recovery for fully-bricked devices.** A Raspberry Pi Zero W at `swd-flash.local` is provisioned for SWD recovery: `ssh swd-flash.local` then `cd ~/swd-recovery && cat README.md` for wiring + commands. The Pi has the known-good fleet BL (0.8.0-4-g76d1e60), MBR, and SoftDevice S140 7.3.0 hex files staged. Use this as the recovery of last resort when a device drops off USB and isn't reachable via BLE. **Permanent SWD wiring:** SWDIO=BCM 24 (pin 18), SWCLK=BCM 25 (pin 22), **RESET=BCM 18 (pin 12)**, GND, 3V3. The reset pin is driven as a bare external pulse via `gpioset` (see `scripts/bl_iter_test.sh`), NOT as OpenOCD `srst`: on nRF52 a pin reset also resets the debug access port, so `reset_config srst_only` makes `reset halt` fail (`AP write error, reset will not halt`). **WDT caveat:** a normal `reset halt` SWD flash works fine on a blank/bricked chip (no app → no watchdog), but a *partial* SWD erase that leaves a valid-but-crashy app in place gets interrupted by that app's hardware WDT (`SafeBootWatchdog` arms it at `.ino:156`, `CONFIG.HALT=1`, and SYSRESETREQ can't stop it). For such a chip, don't try a surgical erase — mass-erase via `recover.sh --erase` (CTRL-AP, no halt needed) then full restore. **Reset-line crosstalk fix (confirmed 2026-05-20):** if a SWD restore fails on the SoftDevice write (`Wrong parity detected` / `Error waiting NVMC_READY` / `clearing lockup after double fault` / `external reset detected`), the floating reset line is glitching from SWDIO/SWCLK crosstalk during the long write — **hold BCM 18 high during the flash**: `sudo timeout <n> gpioset -c gpiochip0 18=1 &` then `recover.sh`. Afterward release it to input (`gpioget -c gpiochip0 18`); a `timeout`-killed `gpioset` can leave BCM 18 output-LOW, which holds the chip in reset (no USB enumeration).

**Test bootloaders on a SACRIFICIAL device.** Do not flash an experimental BL to the only available test device. If only one device is on the bench, build the BL change against the known-good (`git log blinky-local-patches` to find the last-stable BL commit) and verify with `scripts/verify_bootloader.py` BEFORE flashing. The verifier catches the specific bug class that bricked the hat; pass it before committing to a flash.

**Current fleet BL: 0.8.0-10-g4c5faae** (staged in `bootloader/update-bootloader-qspi-ota-default.uf2`; source is pinned at this commit via the `bootloader/src` submodule → `github.com/Jdubz/Adafruit_nRF52_Bootloader` `blinky-local-patches`. Fresh workstation: `git submodule update --init --recursive bootloader/src`). On top of 0.8.0-4's dual-transport baseline, 0.8.0-10 adds:
- `bootloader_settings_save` SD-aware (no more silent fail on UF2 path → was 80% hiccup pre-fix)
- BLE adv paused during USB MSC transfer + ADV_SET_TERMINATED auto-restart guard (eliminates 5-90s slow-completion tail)
- `msc_uf2_check_stuck()` self-recovery (8s idle + incomplete → DFU_RESET → boot to app; converts permanent stuck into ~13s recovery)
- App-handshake watchdog (OPEN_ISSUES §3.1): counter encoded as GPREGRET `0x1N` (hardware register, immune to firmware BSS-zero), bumped on every app-jump, cleared by firmware in `SafeBootWatchdog::begin()` and `markStable()`. After 3 consecutive misses (= pre-`begin()` crashes) BL forces DFU. Closes the "crashy-but-valid app" recovery gap. Earlier 0.8.0-9 RAM-based implementation was broken — counter at `0x20007F78` fell inside firmware `.bss` and was silently zeroed every boot.
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

## CRITICAL: Task is Drum-Hit Detection, Not Beat Detection

**The firmware detects DRUM HITS: kicks and snares** (owner, 2026-10-22). Hi-hats, cymbals and non-percussive changes (bass notes, synth stabs, swells, vocals) are not targets; a flash on one is a false positive. **Metrical position does not decide what counts:** a kick on an off-beat or a syncopated 8th is a target exactly like one on the downbeat. Beat-detection framing is still wrong: scoring against beat positions, or training on beat consensus, teaches the detector to suppress off-beat hits we want.

How strongly a hit shows is a separate, later layer: its weight in the learned rhythm *pattern* (bar-level, per drum class, syncopation-aware, not a quarter-note grid), applied as a strength multiplier, never as a gate on detection.

Concrete consequences:
- **Score detectors on kicks and snares**, not on all onsets: kick/snare recall at ±30/±50 ms and the share of detections that land on a kick or snare, against cleaned `kick_weighted` labels. All-onset F1 at ±100 ms scored 0.68 while the hat looked "almost entirely random" (#280); it is a secondary number at most.
- **Training labels must be onset-flavored drum labels** (`kick_weighted_drums`, `onsets_consensus`). Never use `consensus_v*` (beat tracker output) or anything from `allin1`/`beat_this`/`beatnet`/`demucs_beats` as a training target. Those directories are archived under `_archive_beats_2026_04_29/`. `prepare_dataset.py` refuses to load paths with those markers.
- **Eval ground truth is onset labels**, never `.beats.json`. The blinky-server validation harness reads `.onsets_consensus.json` (required since 2026-04-29). Beat GT files were archived out of `blinky-test-player/music/edm/`.
- **Beat-tracking systems (allin1, beat_this, beatnet, demucs_beats, the BPM half of madmom)** still exist as reference scripts but their *output is beats* and must not flow into the training-label pipeline.
- **BPM in firmware (PLP / ACF tempo tracking)** is fine: it phase-locks the visualizer animation and supplies period/phase to the rhythm-pattern layer. `beatGridPatternMin=0.0` (b149+ default) keeps the beat-grid AND-gate off.

## CRITICAL: Audio Changes Fix Root Causes, Backed by Literature

**No guess-and-check tuning and no stacked patches in the audio pipeline** (owner, 2026-10-22). Every change to detection, picking or gating must:
1. **name the root cause** it removes, shown with a measurement on edm/ (e.g. "61% of missed kicks are blocked by an earlier non-drum detection"), not a symptom;
2. **follow a published, proven method** for that problem, cited (paper + what it reports), and say where we deviate and why; see `docs/ONSET_DETECTION_LITERATURE_2026_05_02.md` and `reports/`;
3. **replace mechanisms rather than add to them:** when a new structure makes a gate, threshold or special case redundant, delete it in the same change. A new knob that compensates for another knob is a patch, and is refused;
4. **be measured for both detectors** through the `OnsetDetector` / `AudioTracker::pickOnset` abstraction, with the drum metrics above, before and after.

Parameter sweeps are fine *inside* a literature-backed design (to set its constants); they are not a design method.

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
# Paths come from the storage roots (docs/ML_STORAGE_LAYOUT.md): set
# BLINKY_DATA / BLINKY_WORK to wherever your local copy lives.
OUT="${BLINKY_WORK:-/mnt/nvme}/outputs"
tmux new-session -d -s training "cd ml-training && source venv/bin/activate && PYTHONUNBUFFERED=1 python train.py --config configs/<name>.yaml --output-dir $OUT/<experiment> 2>&1 | tee $OUT/<experiment>/training.log"
```

`train.py` enforces this — it refuses to start outside tmux/screen unless `--allow-foreground` is passed.

**GPU work runs on the DGX Spark as jobs through its job API** (my-spark): stem separation through blinky-server's Spark client (`blinky_server/spark/`, `/api/spark/*`; `ml-training/scripts/spark_jobs.py` is its CLI, #288), training as blinky's own code through the platform's `gpu_task` job type (my-spark plan 0239). `train.py` meets its supervision contract (`ml-training/scripts/supervision.py`, #304): `STEP n/total` lines, the checkpoint in `$SPARK_CHECKPOINT_DIR` with a run fingerprint checked before resuming, exit 42 after SIGTERM, the dataset from `$SPARK_INPUT_DATA` and results into `$SPARK_OUTPUT_MODEL`. A real eviction today arrives as SIGKILL, so the per-epoch checkpoint is what bounds the loss. Submitting it is #305. Large assets (stems) stay in the Spark's object store; the test box keeps only the ledger and test files. Everything goes through the job API: never start GPU processes over ssh (blinky's `blinky` account there is denied the GPU by design; ssh is for troubleshooting only). devtop runs one-shot CPU scripts only, with nothing left running.

## Build Workflow

**Always use `scripts/build.sh` to compile.** It regenerates `types/Version.h` (build number + git SHA). Release build numbers are assigned by the release workflow (`docs/RELEASE_PROCESS.md`); local builds are for compile checks and `--dev` test-device flashes only.

**Pinned, isolated toolchain (#252): once per machine, run `bash scripts/setup_toolchain.sh`** (or `make setup-toolchain`). It installs the pinned core, the libraries from `tools/firmware_toolchain.json`, TFLite and the core patches into `.arduino/` (gitignored). The arduino-cli version is pinned in the same file, and any other version is refused.
- **Isolated:** `build.sh` and the Makefile compile only inside it, so nothing in a machine's own Arduino folders can shadow a library. That's how an unpatched user-installed TinyUSB 3.4.5 got into b209–b213.
- **Checked:** `tools/check_build_libs.py` fails the build if any library resolves outside `.arduino/`, or if versions drift from `tools/firmware_toolchain.json`.
- **Guarded:** the sketch `#error`s if the TinyUSB it links lacks the CDC patch (`BLINKY_TINYUSB_CDC_PATCHED`).
- **Fixing it:** never "fix" a build by installing libraries elsewhere. Fix the pin, then re-run setup.

```bash
./scripts/build.sh                          # nRF52840 compile check (bumps BUILD_NUMBER)
./scripts/build.sh --no-bump                # recompile without incrementing
./scripts/build.sh --dev                    # dev build: no bump, version b<N>-dev-<sha>
./scripts/build.sh --build N                # exact build number (release CI)
```

Use `./scripts/deploy.sh` (stable by default) to flash devices.

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
| `docs/GENERATOR_EFFECT_ARCHITECTURE.md` | Generator / Effect / Renderer pattern; animations keep absolute time (`FrameClock`, physics substeps) |
| `docs/HARDWARE.md` | XIAO nRF52840 Sense hardware specs |
| `docs/NEXTGEN_ESP32S3_PLAN.md` | Next-gen fixtures on the XIAO ESP32-S3 Sense: dual-core architecture (renderer has priority, audio listener on the other core), carry-over, risks, bench gates |
| `docs/TEST_BOX_BUILD_PLAN.md` | Sound-isolated test box: Pi 5 host, I2S speaker, camera, test chips with SWD via Pi Zero W; enclosure construction, calibration, acceptance gates |
| `docs/TEST_BOX_CAMERA_HARNESS_PLAN.md` | Camera (OV5647) in the test box's validation harness: LED region discovery, visual onsets, render/sound-to-light latency as distributions, follow test against drum labels, calibration, provisional gates |
| `docs/BUILD_GUIDE.md` | Build and installation instructions |
| `docs/DEVELOPMENT.md` | Development guide, config management |
| `docs/SAFETY.md` | Flashing safety mechanisms |
| `docs/RELEASE_PROCESS.md` | Firmware release channels (stable / candidate / dev), cutting, gating, promoting and rolling back releases |
| `docs/WEB_FLASHER.md` | Browser firmware updates (`#firmware`, live, USB only) via signed QSPI OTA: signing, release mirror, checks, bench results, rollout checklist, open items |
| `docs/WEB_UPDATE_RESEARCH_2026_10.md` | Why browser updates go through the running app, not the bootloader: what each platform allows (iPhone, Windows UF2, Chrome's DFU blocklist) |
| `docs/FLASH_LOCKDOWN_PLAN.md` | blinky-server's single-flash-entry-point lockdown: the plan + sequencing for the orchestrator architecture |
| `docs/BLE_FLEET_RELIABILITY_PLAN.md` | Remaining work to make BLE fleet commands hit every device every time; covers firmware-side rxBuffer, command-id idempotency, broadcaster-dongle option |
| `docs/OPEN_ISSUES_2026_10_15.md` | **Current** open-issues index; GitHub issues are the system of record. Covers the 2026-10-14/15 reviews (rhythm-tracking drift, record mode, brick-risk, CI), the PR #152 device checklist, and (§8) the items carried over from the archived `docs/archive/OPEN_ISSUES_2026_05_20.md` (SWD recovery, BLE-DFU tooling, fleet ops). |
| `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` | BLE, WiFi, OTA, fleet server |
| `docs/PHONE_RECORD_MODE_PLAN.md` | Phone field recorder: Android Chrome ⇄ one device over Web Bluetooth, raw mic PCM + analysis for concert-volume tuning; wire format, integrity rules, bench gate |
| `docs/SCULPTURE_BLE_RECOVERY_PLAN.md` | Pre-install brick-proofing for sealed sculpture devices: bootloader DEFAULT_TO_OTA_DFU + watchdog/SafeMode fixes |
| `docs/FLEET_CONSOLE_REFACTOR_PLAN.md` | blinky-console refactor roadmap |
| `blinky-test-player/PARAMETER_TUNING_HISTORY.md` | Historical calibration results |
