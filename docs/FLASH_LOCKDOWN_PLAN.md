# Flash Lockdown Plan

**Status:** open. Pre-requisite for any further live flash testing.
**Owner:** blinky-server.
**Driven by:** 2026-05-17 session — flash-job-rewrite Phases 1-6 landed
cleanly, Phase 7 live test surfaced both a real 0.8.0-4 bootloader bug
AND structural safety holes in our existing flash code.

**Revision 2026-05-17 (evening):** `cart_inner` recovered via operator-
directed `deploy.sh --devices=<BLE addr>` (F2 closed below). Recovery
exposed two additional gaps to fold into the lockdown: (a) the
identity-across-transports problem in `_recovery_device_ids` / any
future `_device_in_flight` (b) the whitelist-reload timing: today the
JSON file is loaded once at startup and only refreshed when
`set_recovery_firmware()` is called from `fleet_flash`. The running
server can hold a stale whitelist for hours after a file edit. Both
gaps captured in §L1/L3 below.

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
   on 0.8.0-4. **2026-05-17 PM refinement:** the failure mode has a
   second symptom besides stuck-MSC. After a clean UF2 write, the BL
   re-enumerates as bootloader (PID `0x0045`) and re-advertises
   `AdaDFU` over BLE while keeping MSC up — even though the app bytes
   are correctly committed to flash (verified byte-for-byte at
   `0x27000` against `CURRENT.UF2`). The "valid app" marker write
   silently fails (`bootloader_settings_save` ~80% hiccup pre-fix),
   so the BL stays in `DEFAULT_TO_OTA_DFU` mode and refuses to boot
   the (correctly-installed) app. Recovery requires either BL update
   to `0.8.0-7` OR a BLE-DFU through `deploy.sh` whose completion
   handshake clears the OTA flag via a different code path.

`cart_inner` was stuck in 0.8.0-4 bootloader MSC + BLE-DFU mode at
session start; recovered via BLE-DFU through `deploy.sh` (F2 below).
**Until the lockdown lands, do not retry any flash against any cart
device without explicit operator go-ahead per attempt.**

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

**L1.1 — Identity equivalence across transports.** A naive `set[str]`
treats `062CBD12EB6961C8` (cart_inner USB SN) and `E9:A8:5C:A5:BB:BE`
(cart_inner BLE app addr) as different devices, even though they are
the same physical chip. Today's `_recovery_device_ids` whitelist has
this exact bug: a flash that armed under one identity won't dedupe
against a recovery attempt under the other identity. Required for
L1 to actually be a single in-flight claim:

- `FleetManager` keeps an `identity_groups: dict[str, str]` (alias →
  canonical) that resolves all known identifiers for a physical device
  (USB SN, BLE app addr, BLE bootloader addr — the +1/-1 pair from
  `_bootloader_to_app_address()`) to a single canonical key. Use the
  USB SN where known, BLE app addr otherwise.
- `_device_in_flight` and `_recovery_device_ids` are stored against
  canonical keys. Membership tests run the input through
  `resolve_canonical(id)` first.
- Identity groups are populated when a device is first discovered on
  any transport — the discovery code already has the cross-transport
  visibility (`_bootloader_to_app_address` in `discovery.py:87`).
  Persist groups to disk so they survive restart.

Without L1.1, L1's "single in-flight set" still has the gap that bit
the 5/16 brick: the SN-only whitelist couldn't match the
BLE-addr-only device when it went into DFU-recovery state.

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
- **L3.1 — whitelist re-read on auto-recovery task launch.** Today
  `_recovery_device_ids` is loaded from
  `~/.local/share/blinky-server/recovery-firmware.json` exactly once
  at server startup and refreshed only when `fleet_flash` calls
  `set_recovery_firmware()`. A direct edit of the file requires a
  server restart to take effect — that's an unintended escape hatch
  going the wrong way (operator can think they've extended the
  whitelist when in-memory state is still the old list, and the next
  flash narrows it again). Fix: the auto-recovery branch in
  `_background_loop` should re-read the JSON file at task launch (or
  on a short TTL — e.g. 10 s cached). The file is small; the cost is
  trivial. Same canonical-resolution applies (L1.1).

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

### F2. cart_inner recovery (closed 2026-05-17 evening)

**Closed.** `cart_inner` was recovered the same day. Sequence:

1. Operator pulled `cart_inner` from the enclosure and reconnected it
   over USB. The XIAO came up in dual UF2-MSC + BLE-AdaDFU mode (the
   BL's standard "no valid app" recovery posture).
2. Tried UF2 drop via `tools/uf2_upload.py --already-in-bootloader`
   first — write succeeded byte-for-byte, but BL stayed in DFU mode
   (F1's second symptom, the `bootloader_settings_save` silent-fail).
3. Operator-directed `./scripts/deploy.sh --devices=E9:A8:5C:A5:BB:BE
   --skip-compile --no-bump` — BLE-DFU through the post-watchdog-fix
   pipeline completed cleanly in ~6 min (watchdog-pinger task
   confirmed alive throughout). BL exited DFU, app booted, both serial
   and BLE transports show `cart_inner` running `b166-c3acc4fd-dirty`,
   configured=true, safe_mode=false.
4. `save` step in deploy.sh timed out at the 2 s confirmation window;
   re-issued via `POST /api/devices/.../settings/save` directly
   (legitimate non-deploy-gated route per `deps.py:95`), firmware
   ACKed `OK`. Persistence confirmed.

Lessons captured in the F1 refinement above and in new memory
`project_bl_0804_settings_save_silent_fail`. The two L-additions
(L1.1 identity equivalence, L3.1 whitelist re-read) come from this
recovery's specific failure surfaces.

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
- `[[project-bl-0804-settings-save-silent-fail]]` (auto-memory, 2026-05-17)
- `CLAUDE.md` "Upload Safety (nRF52840)" section
- Commits 8a5701d6, 431c55bc, 7bb84e4e, 14cd1ba5, f2216da2, f5849e5b
  (flash-job rewrite Phases 1-6)
