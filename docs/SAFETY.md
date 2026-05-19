# Flashing Safety Model

This document is a short index of the flashing/recovery safety mechanisms that protect the nRF52840 fleet. The authoritative behavioural rules live in `CLAUDE.md` at the repo root — this file only summarises where each mechanism is defined.

## The four invariants

1. **Never bypass `deploy.sh` for fleet flashes.** The server enforces an `X-Deploy-Tool` header on `/api/fleet/upload`, `/api/fleet/flash`, and the destructive `/api/devices/{id}/command` subset (`device upload`, `reboot`, `wipe_device_identity`). Direct `curl` returns 403 even with a valid API key.
   - Operator entry point: `scripts/deploy.sh` (requires explicit `--devices=` target).
   - Gated command list: `blinky_server/api/deps.py:_DEPLOY_GATED_COMMAND_LIST`.

2. **One flash path inside blinky-server.** Every flash routes through `FleetManager.flash_device()` → `_run_flash_job` → guarded `_impl` functions. Direct calls to `_uf2_write_impl_for_job` or `_ble_dfu_write_impl` raise `OutsideFlashJobContextError`. The audit log at `~/.local/share/blinky-server/flash_attempts.jsonl` is the persistent ledger.
   - Plan + sequencing: `docs/FLASH_LOCKDOWN_PLAN.md`.

3. **Bootloader changes go through `deploy-bootloader.sh`.** Wraps `scripts/verify_bootloader.py` (refuses BLs whose `check_dfu_mode()` reaches `bootloader_dfu_start()` without USB initialised — the literal bug that bricked the hat 2026-05-15). Pre-flash version log + post-flash version verify.
   - Diagnostic narrative: `docs/BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md`.
   - SWD recovery host: `swd-flash.local` (Raspberry Pi Zero W with known-good fleet BL + MBR + S140 staged).

4. **60-second rule between resets.** `SafeBootWatchdog::markStable()` is deferred to 60 s of uptime so mid-init crashes still count toward `RebootFrequencyCounter`. Any chain of resets within 60 s bumps the counter; at 5 cumulative trips, `configStorage.quarantineDeviceConfig()` flips `isValid=false` and the device boots into safeMode.
   - Code: `blinky-things/blinky-things.ino` (`markStable()` deferral, `quarantineDeviceConfig` call), `hal/SafeBootWatchdog.h`, `hal/RebootFrequencyCounter.h`.

## Firmware-side defences

- **Static assertions** on all stored config structs (`blinky-things/config/ConfigStorage.h` ~lines 388–398). Compile fails if a struct size changes without bumping `SETTINGS_VERSION` (currently 95).
- **Parameter validation** in `ConfigStorage.cpp::loadConfiguration()`. Out-of-range values fall back to defaults; corrupt configs cannot brick boot.
- **Flash address validation** before every config write — refuses writes into the bootloader region or off the end of the application slot. See `blinky-things/tests/SafetyTest.h::isFlashAddressSafe()`.
- **No silent fallbacks** — see CLAUDE.md "No Silent Fallbacks" section for the patterns we refuse to add (zero-fill array literals, ternary defaults, switch catchalls, etc.). Reference incident: v33 mel filterbank corruption (2026-04-27).

## Recovery path (in order)

1. `./scripts/deploy.sh --devices=<id>` (re-flashes via the orchestrator; respects dedup + max-attempts unless `--force`).
2. Wait ≥75 s uptime so `markStable()` clears the watchdog counter.
3. Push device config via the fleet API `device upload` command (deploy.sh-gated).
4. SWD via `swd-flash.local` if the device is off USB and unreachable over BLE.

For unenrolled bare chips on the bench, `make uf2-upload UPLOAD_PORT=/dev/ttyACM0` is the recovery path.

## Related plans

- `docs/FLASH_LOCKDOWN_PLAN.md` — server-side single-entry-point lockdown.
- `docs/SCULPTURE_BLE_RECOVERY_PLAN.md` — pre-install brick-proofing for sealed devices.
- `docs/BOOTLOADER_PRODUCTION_AUDIT_2026_05_15.md` — bootloader invariants and the 2026-05-15 hat-bricking incident.
- `docs/BLE_FLEET_RELIABILITY_PLAN.md` — remaining BLE-command-reliability work.

The pre-2026-05 version of this doc (Arduino IDE upload guidance, layered defense narrative, pre-commit hook references) is at `docs/archive/SAFETY_pre_2026_05.md`. It does not reflect the current safety model.
