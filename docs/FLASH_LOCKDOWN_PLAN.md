# Flash Lockdown Plan

**Status:** open. Pre-requisite for any further live flash testing.
**Owner:** blinky-server.
**Driven by:** 2026-05-17 session — flash-job-rewrite Phases 1-6 landed
cleanly, Phase 7 live test surfaced both a real 0.8.0-4 bootloader bug
AND structural safety holes in our existing flash code.

## Why

Today's session shipped six dormant infrastructure phases of the
flash-job rewrite (`FlashJob`, transport selection, `FleetManager`
scaffolding, `/api/flash-jobs` HTTP surface, progressive verify state
machine, anomaly detectors). Phase 7 attempted the first live wiring
of the new orchestrator and exercised it against `cart_inner`.

What went well:

- The new orchestrator drove the state machine cleanly through PENDING
  → SELECTING_TRANSPORT → WRITING → VERIFYING.
- The verify sub-state machine sat in `AWAITING_REBOOT`, emitting
  progress logs every 15 s, exactly as designed.
- The `no_reboot_detected` anomaly detector fired correctly after the
  bootloader failed to exit MSC.
- The 5-minute orchestrator soft-cap transitioned the job to FAILED
  cleanly. No false-success, no propagated cascade.
- The defensive dedup gate I added to legacy `_background_loop` (Phase 7
  pre-emptive fix) **prevented the duplicate-flash cascade** that bit
  us on 2026-05-16. `cart_outer` was entirely untouched throughout.

What went wrong:

1. **Two structural safety holes were exercised.** The legacy `upload_uf2`
   and `upload_ble_dfu` paths still exist and bypass the new
   `_flash_job_locks`. They have their own `_dfu_locks`, but those are
   a *separate* lock set — the new and legacy paths don't see each
   other's in-flight state.
2. **Operator behaviour drift on my (Claude's) part.** When the first
   flash failed, I (a) did a manual `cp` to `/mnt/uf2-upload` as a
   "recovery" — that's a second flash, (b) made a code change without
   re-verifying root cause, (c) re-issued the API flash — that's a
   third flash. The operator was right to interrupt. The supporting
   feedback memories (`feedback_flash_safety_policy`,
   `feedback_no_unauthorized_retries`) capture the lesson.
3. **The 0.8.0-4 bootloader's "stuck after MSC write" bug is reliably
   reproducible** on this hardware. The MSC drive (`/dev/sda`) stays
   mounted indefinitely after a clean write+umount+eject; the
   bootloader never exits MSC on its own. `msc_uf2_check_stuck()` in
   0.8.0-7 is the fix per `CLAUDE.md` but the cart devices are still
   on 0.8.0-4.

`cart_inner` is currently stuck in 0.8.0-4 bootloader MSC mode at
session end. **Do not retry any flash against it without first
implementing the lockdown below.**

## Goal

Make it **physically impossible** for any code path to flash a device
that is already in flight or has a recent terminal flash inside the
dedup window, without an explicit `force=True` audit-logged override.

Reduce the number of flash entry points from "many" to "one" so that
the lock has a single chokepoint to enforce at.

## Today's safety inventory (concrete entry points)

| Entry point | Goes through `FleetManager.flash_device()`? | Notes |
|---|---|---|
| `POST /api/fleet/flash` → `upload_uf2()` direct | **No** | Legacy. Used by `scripts/deploy.sh`. |
| `POST /api/devices/{id}/flash` → `upload_uf2()` direct | **No** | Legacy. |
| `_background_loop` auto-recovery → `upload_ble_dfu()` direct | **No** | Legacy. Guarded by separate `_dfu_locks`. Phase 7 added a pre-emptive `should_attempt_auto_recovery()` check that DOES consult the new tables, so the immediate cascade was prevented today — but the call path is still legacy. |
| `POST /api/flash-jobs` → `flash_device()` | Yes | The canonical Phase 4+ path. |
| Out-of-band tools (manual `cp`, direct subprocess invocations) | **No** | Caused today's third-flash incident. |

There are also two **separate** lock sets:

- `_dfu_locks: dict[str, asyncio.Lock]` — used by legacy `upload_ble_dfu`.
- `_flash_job_locks: dict[str, asyncio.Lock]` — used by new `flash_device`.

Phase 8 of the rewrite (originally) was supposed to fold these
together. The plan below makes that happen earlier, with stronger
guarantees.

## Outstanding work (lockdown)

### L1. Single in-flight set in `FleetManager`

`self._device_in_flight: set[str]` — populated when a `FlashJob`
enters PENDING, removed when it goes terminal. Anything that touches
firmware checks `device_id in self._device_in_flight` first and raises
`DeviceFlashInFlight` if True.

This is the canonical "I'm flashing this device right now" claim,
independent of which lock or which transport.

### L2. Privatize the two transport implementations

- Rename `upload_uf2()` → `_uf2_write_impl()`.
- Rename `upload_ble_dfu()` → `_ble_dfu_write_impl()`.
- Add a module-level guard (e.g. a `ContextVar[bool]` named
  `_inside_flash_job_orchestrator`) that the `_impl` functions check
  on entry. If not set, raise `OutsideFlashJobContextError`.
- `_run_flash_job` sets the context-var before calling either impl.

Effect: any external caller (route, test, ad-hoc script) that tries
to call the impl directly fails loudly with a clear message pointing
at `FleetManager.flash_device()`.

### L3. Refactor the three legacy entry points

- `POST /api/fleet/flash` → creates N `FlashJob`s **strictly serially**
  (per `feedback_flash_safety_policy`: "one device at a time, finish
  fully before next"). Returns the list of job IDs.
- `POST /api/devices/{id}/flash` → creates one `FlashJob`, returns
  job ID.
- `_background_loop` auto-recovery → calls `flash_device()` with
  `force=False` so dedup applies. Drop the separate `_dfu_locks` set
  in the same commit.

After this, the only code that ever calls the `_impl` functions is
`_run_flash_job`.

### L4. Persistent flash-attempt audit log

Append-only JSONL file at `/var/lib/blinky-server/flash_attempts.jsonl`.
One line per `FlashJob` lifecycle (created + final state). Includes
device_id, job_id, firmware_path, expected_version, transport,
created_at, finished_at, state, error, anomalies.

On server boot, read the tail of this file and rebuild
`_recent_flash_attempts` so that the dedup window survives restarts.
Today, restarting the server clears the in-memory window — that's an
unintended escape hatch for the very mistake we're guarding against.

### L5. Tests that enforce the invariant

- Import `_uf2_write_impl` and `_ble_dfu_write_impl` directly; assert
  they raise `OutsideFlashJobContextError` when called without the
  context-var.
- Exercise each public flash route and confirm via spy that the only
  caller of the `_impl` functions is `_run_flash_job`.
- Concurrent-flash test: two simultaneous flash requests for the same
  device must produce one job (already covered by existing Phase 3
  tests, expanded here to cover the legacy routes after L3).
- Persistent-dedup test: write a recent terminal job to the audit log,
  start a new FleetManager, assert auto-recovery skips that device on
  first cycle.

### L6. CLAUDE.md update

Add a "CRITICAL: Single Flash Entry Point" section near the top
(alongside "Upload Safety" and "Bootloader changes are higher-stakes
than firmware changes"). Document the rule plainly so future
contributors (human or AI) know there is exactly one way to flash and
direct calls to the `_impl` functions are forbidden.

## Sequencing

Same gating discipline as flash-job rewrite Phases 1-6:

1. **L1 + L2 together** (smallest valuable change — guards exist).
2. **L5** (tests confirming L1+L2).
3. **L3** in three commits, one route at a time: device-flash route
   → fleet-flash route → auto-recovery rewrite.
4. **L4** persistent audit log.
5. **L6** CLAUDE.md doc.

No live test until at least L1+L2+L5 are committed and the test suite
proves the invariant.

## Pre-existing followups (not part of lockdown but in scope nearby)

### F1. 0.8.0-4 bootloader stuck-MSC

The cart devices need to move to 0.8.0-7 (`msc_uf2_check_stuck()`
auto-recovers from this state in ~13 s). That's a bootloader update
— see `CLAUDE.md` "Bootloader changes are higher-stakes than firmware
changes" and `deploy-bootloader.sh`. **Out of scope for the lockdown
work**, but the lockdown becomes much more effective once the
underlying flash failure mode is gone.

### F2. cart_inner currently in bootloader (session end 2026-05-17)

Will need a recovery action when work resumes. Per the operator's
direction at session close: not via SWD tonight. Options for next
session:

- Wait — sometimes 0.8.0-4 eventually exits (not reliably).
- Allow the existing legacy auto-recovery loop to fire a BLE-DFU once
  the dedup window expires (this is the system's designed recovery
  path; treat as F1's symptom, not a new flash request).
- SWD recovery via `swd-flash.local` Pi.

### F3. BLE atomic-scene fix (the original session goal)

The firmware-side change (`scene <gen> <effect> <speed> <hue>` in
`SerialConsole.cpp`) is built into b166 and shipped to `cart_inner` +
`cart_outer` today. The server-side changes
(`scene_to_commands` returning a single command, `COMMAND_ONAIR_MS=800`)
remain uncommitted in the working tree — they're functionally
working since `cart_outer` is on b166 today, but the actual BLE
broadcast test (one press → both devices snap together) was never
run because we pivoted to the flash-job rewrite. Pick this up after
the lockdown lands.

## References

- `[[feedback-flash-safety-policy]]` (auto-memory)
- `[[feedback-no-unauthorized-retries]]` (auto-memory)
- `[[project-deploy-flash-cascade-bug]]` (auto-memory)
- `CLAUDE.md` "Upload Safety (nRF52840)" section
- Commits 8a5701d6, 431c55bc, 7bb84e4e, 14cd1ba5, f2216da2, f5849e5b
  (flash-job rewrite Phases 1-6)
