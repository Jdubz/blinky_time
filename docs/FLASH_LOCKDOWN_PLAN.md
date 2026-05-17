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

**Revision 2026-05-18:** F3 closed (server-side BLE atomic-scene
committed in `752ebc16`, operator confirmed fleet-wide ≥b164 floor).
Three rounds of PR 142 review hardened lockdown-adjacent code without
adding scope: silent fallback in `_run_uf2_flash` replaced with a
`transport_type == "serial"` guard; `SignalHistory.previous_version`
wired from `device.version` so `detect_stale_firmware` actually runs;
`_FleetVerifySignals.history` is now constructor-injected (was a
post-construction settable attribute — invisible-dependency hazard);
`wait_until_terminal` uses an absolute deadline (Phase 11 deploy.sh
polling will depend on this); `_recent_flash_attempts` has an explicit
`TODO(L4)` marker; `POST /api/flash-jobs` constrains `firmware_path`
to `firmware_dir()` (closes the arbitrary-path probe vector); GET
flash-jobs endpoints now require `X-API-Key`; the version-key lookup
is consolidated in `firmware/utils.py`. None of these change the L1–L6
plan below; they reduce noise around it.

**End-state constraint (2026-05-18, operator direction):** when the
lockdown lands, **zero legacy / deprecated flash code remains**. The
parallel-paths-existing-in-the-first-place is what produced today's
cascade; "the new lockdown is in place AND the old uploader scripts
still exist as a fallback" is explicitly NOT the target state. Every
protection currently in the legacy paths MUST be migrated to the
canonical path before the legacy code is deleted — no regressions,
no orphan wrappers, no `# deprecated` stubs that linger past the
migration commit. See "Audit before deletion" below for the checklist.

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

| Entry point | Calls | Goes through `FleetManager.flash_device()`? |
|---|---|---|
| `POST /api/fleet/flash` (`routes_firmware.py:_flash_fleet_background`) | `firmware/__init__.upload_firmware()` → `firmware/uf2_upload.py:upload_uf2()` | **No** (legacy; used by `scripts/deploy.sh`) |
| `POST /api/devices/{id}/flash` (`routes_devices.py`) | `firmware/__init__.upload_firmware()` → same | **No** (legacy) |
| `_background_loop` auto-recovery (`manager.py:1561`) | `firmware/ble_dfu.py:upload_ble_dfu()` direct | **No** (legacy; guarded by `_dfu_locks` + the `should_attempt_auto_recovery` dedup gate added in Phase 7) |
| `POST /api/flash-jobs` (`routes_flash_jobs.py:create_flash_job`) | `FleetManager.flash_device()` → `_run_flash_job` → `_run_uf2_flash` → `firmware/uf2_upload.py:flash_uf2_for_job()` | **Yes** (Phase 4+ canonical path) |
| Out-of-band tools (manual `cp`, direct subprocess invocations) | — | **No** (caused today's third-flash incident) |

There are also two **separate** lock sets:

- `_dfu_locks: dict[str, asyncio.Lock]` (`manager.py:130`) — used by legacy `upload_ble_dfu`.
- `_flash_job_locks: dict[str, asyncio.Lock]` (`manager.py:164`) — used by new `flash_device`.

Phase 8 of the rewrite (originally) was supposed to fold these
together. The plan below makes that happen earlier, with stronger
guarantees.

### Audit before deletion (per the end-state constraint)

Before any legacy entry point is deleted, every protection it
currently provides MUST be present in the canonical path. Inventory:

**`firmware/__init__.upload_firmware()`** dispatches to UF2 or BLE-DFU
by transport type and handles `DeviceState.DFU_RECOVERY` specially
(skip the bootloader-entry step, go straight to BLE-DFU on the
already-stuck device). Migration target: a sibling to `_run_uf2_flash`
named `_run_ble_dfu_flash` (the comment at `manager.py:395` calls this
out as Phase 9 future work). Must preserve:
- DFU_RECOVERY direct-to-BLE-DFU dispatch (don't try to re-enter
  bootloader on a device that's already in bootloader).
- BLE bootloader entry via NUS `"bootloader ble"` command (the
  `enter_bootloader_via_ble` closure in `firmware/__init__.py:132`).
- `dfu_zip` build via `ensure_dfu_zip(firmware_path)`.
- `progress_callback` plumbing (currently UF2-only — the BLE paths
  warn-log when a callback is provided and dropped; preserve that
  shape).

**`firmware/uf2_upload.upload_uf2()`** has its own protections beyond
what `flash_uf2_for_job` covers today:
- USB-reset to clear stale CDC state (`_usb_reset_device`).
- Server-transport disconnect/reconnect dance around the flash.
- Phase-callback wrapping (`progress` inner function).
- Elapsed-time accounting + `t0 = time.monotonic()`.

Each of these must be present in the merged write path, or
explicitly removed with rationale.

**`firmware/ble_dfu.upload_ble_dfu()`** has retry logic (see comment
at `ble_dfu.py:138`: "retry attempts in upload_ble_dfu()"). Don't
lose that.

**`manager.py:1561` legacy auto-recovery branch:**
- `MAX_AUTO_RECOVERY_ATTEMPTS = 3` per-device limit (`manager.py:47`).
- Exponential backoff schedule (`manager.py:1069`: 10s, 20s, 40s,
  80s, 160s, capped at 5 min).
- `should_attempt_auto_recovery` dedup gate (already consults the
  new tables; this part is forward-compatible).

The migration sub-commits in L3 must each name which protection is
moving where, and the final delete-legacy commit must show every
protection has a new home (or an explicit "intentionally dropped"
rationale).

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

### L2. Privatize the transport implementations

Three public functions today; all become private + guarded:

- `firmware/uf2_upload.py:upload_uf2()` → `_uf2_write_impl()`.
- `firmware/uf2_upload.py:flash_uf2_for_job()` → `_uf2_write_impl_for_job()`
  (the new orchestrator's helper — also gets the guard, since "the new
  path" doesn't excuse bypassing the in-flight check).
- `firmware/ble_dfu.py:upload_ble_dfu()` → `_ble_dfu_write_impl()`.

Plus the re-export shims in `firmware/__init__.py` (`upload_via_uf2`,
`upload_firmware`) get removed by L3 — those callsites migrate to
`FleetManager.flash_device()`.

Guard implementation: a module-level `ContextVar[bool]` (probably in
`firmware/_guard.py` so both impl modules import the same one):

```python
# firmware/_guard.py
_inside_flash_job_orchestrator: ContextVar[bool] = ContextVar(
    "_inside_flash_job_orchestrator", default=False
)

class OutsideFlashJobContextError(RuntimeError):
    """Raised when a write impl is called outside _run_flash_job.

    The lockdown rule (FLASH_LOCKDOWN_PLAN.md) is that exactly one
    code path produces flashes — FleetManager.flash_device(). If you
    see this exception, you reached an impl through a back door.
    """
```

`_run_flash_job` sets the var to True before calling either impl and
resets in `finally`. Every `_impl` function's first line checks
`_inside_flash_job_orchestrator.get()` and raises
`OutsideFlashJobContextError` if False.

Effect: any external caller (route, test, ad-hoc script) that tries
to call the impl directly fails loudly with a clear message pointing
at `FleetManager.flash_device()`.

**A `_run_ble_dfu_flash` orchestrator method MUST exist before L2 can
land** — otherwise privatizing `upload_ble_dfu` removes the only
available BLE-DFU code path and breaks auto-recovery + DFU_RECOVERY
state handling. The comment at `manager.py:395` flags this as Phase 9
work; the lockdown promotes it to a prerequisite. See the audit
checklist above for the protections that must come with it.

### L3. Refactor the three legacy entry points

Three sub-commits, smallest blast radius first:

**L3a — `POST /api/devices/{id}/flash`** (`routes_devices.py`):
single-device call → `fleet.flash_device(device_id, fw_path)`. Drop
the `firmware/__init__.upload_firmware` import. **Then delete the
function** — see end-state constraint.

**L3b — `POST /api/fleet/flash`** (`routes_firmware.py:_flash_fleet_background`):
creates N `FlashJob`s **strictly serially** (per
`feedback_flash_safety_policy`: "one device at a time, finish fully
before next"). Returns the list of job IDs. The deploy.sh + UI
polling shape uses `FlashJob.wait_until_terminal` with an absolute
deadline (already in place per PR 142 review). Drop the
`upload_firmware` import. Then delete `_flash_fleet_background` and
the `upload_firmware` re-exports in `firmware/__init__.py`.

**L3c — `_background_loop` auto-recovery** (`manager.py:1561`):
calls `flash_device(force=False)` so dedup applies. Drop the separate
`_dfu_locks` set in the same commit. **MAX_AUTO_RECOVERY_ATTEMPTS = 3
and the exponential backoff schedule (`manager.py:1069`) must move
into the orchestrator** — either as `FleetManager.flash_device`
kwargs or as fields on `FlashJob`. Then delete the direct
`upload_ble_dfu` import + the duplicated lock-handling code in the
auto-recovery branch.

**L3.1 — whitelist re-read on auto-recovery task launch.** Today
`_recovery_device_ids` is loaded from
`~/.local/share/blinky-server/recovery-firmware.json` exactly once
at server startup and refreshed only when `fleet_flash` calls
`set_recovery_firmware()`. A direct edit of the file requires a
server restart to take effect — that's an unintended escape hatch
going the wrong way (operator can think they've extended the
whitelist when in-memory state is still the old list, and the next
flash narrows it again). Fix: the auto-recovery branch should
re-read the JSON file at task launch (or on a short TTL — e.g. 10 s
cached). Same canonical-resolution applies (L1.1). Land alongside L3c.

**L3d — final legacy delete.** Files / functions that must be gone:

- `firmware/__init__.py:upload_firmware()` — entire dispatch function.
- `firmware/__init__.py:upload_via_uf2()` — UF2 wrapper, deprecated by L2.
- `firmware/uf2_upload.py:upload_uf2()` — only the public name; the
  guarded `_uf2_write_impl` from L2 is what remains.
- `firmware/ble_dfu.py:upload_ble_dfu()` — same.
- `manager.py:_dfu_locks` field and any `_get_dfu_lock` helpers.
- The direct `from ..firmware.ble_dfu import upload_ble_dfu` import
  in `manager.py`.

Verification: `grep -rn "upload_uf2\|upload_ble_dfu\|upload_firmware\|upload_via_uf2\|_dfu_locks" blinky_server/`
returns zero hits across non-test source after L3d lands. Tests that
referenced the old names must either be migrated to call
`flash_device()` or be deleted as superseded.

After this, the only code that ever calls the `_impl` functions is
`_run_flash_job` (verified by L5's spy test).

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

0. **Build `_run_ble_dfu_flash` orchestrator method first** — L2's
   privatization of `upload_ble_dfu` removes the only BLE-DFU
   code path otherwise, breaking auto-recovery + DFU_RECOVERY state
   handling on the spot. The new method takes the DFU_RECOVERY
   dispatch + bootloader-entry-via-BLE + dfu_zip build that
   `firmware/__init__.upload_firmware` currently owns, plus
   `MAX_AUTO_RECOVERY_ATTEMPTS` accounting.
1. **L1 + L1.1 together** — canonical-key resolver + single
   in-flight set. No transport-impl changes yet; the legacy paths
   still work but now go through dedup if they happen to consult the
   set (they don't, until L3).
2. **L2 with shims** — rename to `_impl` + add ContextVar guard +
   keep the legacy public names as a *one-line wrapper* that calls
   the impl with the context-var manually set. The wrappers are
   tagged `# REMOVE IN L3d` and exist ONLY for the duration of L3a
   through L3c so legacy callers don't break atomically. L5 tests
   land in this same commit.
3. **L3a** — `POST /api/devices/{id}/flash` migrates to
   `flash_device()`. Delete the route's `upload_firmware` import +
   the per-route legacy wrapper code.
4. **L3b** — `POST /api/fleet/flash` migrates. `deploy.sh` continues
   to work because the route's external shape is unchanged; only the
   internals route through `flash_device()`.
5. **L3c + L3.1** — `_background_loop` auto-recovery migrates +
   whitelist re-read on task launch. Drop `_dfu_locks` set. Migrate
   `MAX_AUTO_RECOVERY_ATTEMPTS` + backoff schedule into the
   orchestrator.
6. **L3d** — delete every legacy public name + wrapper. Verification
   grep returns zero hits. **End-state achieved.**
7. **L4** persistent audit log.
8. **L6** CLAUDE.md doc.

No live test until at least L1 + L1.1 + L2 + L5 + L3a are committed
and the test suite proves the invariant. The lockdown's effective
moment is L3d; everything before is incremental migration with both
old and new paths coexisting (each new commit shrinks the legacy
surface).

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

### F3. BLE atomic-scene fix (closed 2026-05-17 evening)

**Closed.** The server-side changes (`scene_to_commands` returning a
single command, `COMMAND_ONAIR_MS = 800`) landed in commit
`752ebc16` ("Address PR 142 review feedback + BLE atomic-scene
rollout"). Firmware `handleSceneCommand` is in b166 on cart_inner +
cart_outer. Operator confirmed the fleet-wide ≥b164 floor; older
firmware would silently ignore the `scene` command but no enrolled
device is below b164.

The live BLE broadcast smoke test (one Hub-UI press → both cart
devices snap together within ~1 s, capture rate ≥95%) is the
remaining acceptance check; not gated by the lockdown.

## References

- `[[feedback-flash-safety-policy]]` (auto-memory)
- `[[feedback-no-unauthorized-retries]]` (auto-memory)
- `[[project-deploy-flash-cascade-bug]]` (auto-memory)
- `[[project-bl-0804-settings-save-silent-fail]]` (auto-memory, 2026-05-17)
- `CLAUDE.md` "Upload Safety (nRF52840)" section
- Commits 8a5701d6, 431c55bc, 7bb84e4e, 14cd1ba5, f2216da2, f5849e5b
  (flash-job rewrite Phases 1-6)
