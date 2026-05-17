# Flash Lockdown — Legacy Protections Audit

**Date:** 2026-05-18
**Purpose:** Catalog every protection currently provided by the legacy
flash paths, locate its home in the canonical path (or flag the gap),
and produce a checklist that gates L3d (legacy delete).

Per FLASH_LOCKDOWN_PLAN.md end-state constraint: when the lockdown
lands, every legacy protection must have a home in the canonical
path, or be explicitly removed with rationale. This document is the
audit.

## Legend

- **HAS** — canonical path already covers this.
- **GAP** — protection exists in legacy only; must migrate before
  L3d can delete the legacy code.
- **MOVE** — protection exists in legacy at the route layer; needs
  to move into `flash_device()` so all callers get it uniformly.
- **DROP** — explicitly dropping with rationale (no equivalent in
  canonical, and the protection is no longer required).

## The canonical path inventory

`FleetManager.flash_device()` → `_run_flash_job()` → `_run_uf2_flash()`
→ `firmware/uf2_upload.py:flash_uf2_for_job()` currently provides:

- FlashJob state machine + strict transitions (PENDING → SELECTING_TRANSPORT
  → WRITING → VERIFYING → COMPLETED/FAILED/ABANDONED).
- Per-device asyncio lock (`_flash_job_locks`).
- Transport-selector with explicit `select_transport(probe)`.
- Transport-type guard at UF2 entry (PR 142 round 2 fix).
- USB device path lookup + serial-number capture **before** transport disconnect.
- Transport disconnect (`contextlib.suppress(Exception)`).
- USB reset via `USBDEVFS_RESET` ioctl.
- 3-second wait + by-id re-discovery after USB reset.
- Subprocess exec with stdout/stderr streaming to journal.
- 300-second subprocess wall-clock.
- Write-complete detection (`_WRITE_COMPLETE_RE`) — write-seen is
  the success criterion, not subprocess exit code.
- `job.record_progress(bytes_written, bytes_total)`.
- `job.set_error()` + transition to FAILED on errors.
- `_recent_flash_attempts` dedup-timestamp on every terminal exit.
- Cancellation → ABANDONED; crash → FAILED.
- Progressive verify state machine (`firmware/verify.run_verify`).
- Anomaly detectors (`firmware/anomalies.check_anomalies`).
- 5-minute total verify wall-clock cap.
- `wait_until_terminal(timeout)` with absolute deadline.

## Gap audit

### `firmware/__init__.upload_firmware()` dispatch

| Protection | Status | Notes |
|---|---|---|
| `DFU_RECOVERY` state → BLE-DFU direct (skip bootloader entry) | **GAP** | `select_transport` probe doesn't see DFU_RECOVERY state. The probe consults `has_usb_app` and `has_ble_dfu_advert`; `_build_transport_probe` at `manager.py:514` stubs the BLE-DFU branch to False (Phase 5 TODO). L0 must wire this up before L2 can land. |
| BLE address missing → explicit error (no crash) | **GAP** | New path doesn't construct a BLE-DFU code path at all yet. Must check `device.ble_address` is non-None before entering DFU. |
| `ensure_dfu_zip(firmware_path)` in `asyncio.to_thread` | **GAP** | No DFU-zip build in canonical path (UF2 doesn't need it). `_run_ble_dfu_flash` must wrap this. |
| Transport-type dispatch (`serial` vs `ble`) | **HAS** | `select_transport(probe)` returns `UF2` or `BLE_DFU`. |
| Unknown transport type → explicit error | **HAS** | `select_transport` raises `NoReachableTransport`. The `_run_flash_job` else-branch also has an exhaustiveness guard. |
| "Progress callback lost" WARN log on BLE paths | **GAP** | The new path uses `job.record_progress` for bytes only. Phase-string progress isn't piped through; nothing warns. Either (a) wire phase progress into FlashJob, or (b) document the loss explicitly. |
| BLE bootloader entry via NUS `"bootloader ble"` (with `enter_bootloader_via_ble` closure) | **GAP** | The whole NUS-write-then-disconnect dance lives only in `upload_firmware()`. Must move into `_run_ble_dfu_flash`. Includes the idempotent-connect guard and the 0.5s post-write sleep. |
| NO_FALLBACK doctrine (chosen method fails → error, not retry-via-other-transport) | **HAS** | `FlashJob.set_transport()` is "final" (raises `InvalidTransition` on re-call). Documented in `feedback_flash_safety_policy`. |

### `firmware/uf2_upload.upload_uf2()`

| Protection | Status | Notes |
|---|---|---|
| `t0 = time.monotonic()` elapsed accounting | **HAS** | `FlashJob.duration_s` property (computed from `created_at` to `finished_at`). |
| `progress(phase, msg, pct)` inner with INFO log + callback forwarding | **GAP** | Canonical uses byte-count progress only. Phase tags ("prepare", "upload", "done") are diagnostic UX; lost on this path. **DROP candidate** if FlashJob state machine is deemed UX-sufficient, otherwise extend `FlashJob` with a phase field. |
| `_find_uf2_tool()` discovery + early error | **HAS** | Same call in `flash_uf2_for_job`. |
| Firmware-file existence check | **HAS** | Same. |
| USB device path lookup BEFORE disconnect | **HAS** | Same. |
| USB serial number extraction from by-id | **HAS** | Same. |
| `contextlib.suppress(Exception)` on disconnect | **HAS** | Same. |
| USB reset via `_usb_reset_device` | **HAS** | Same. |
| 3s wait + port re-discovery | **HAS** | Same. |
| 300s subprocess timeout | **HAS** | Same. |
| stdout streaming to journal | **HAS** | Same. |
| Last 500 bytes of output in result | **GAP** | `flash_uf2_for_job` puts error string into `job.set_error()`, no output snippet. For post-mortems on a **successful** flash where the operator wants to see the umount/eject diagnostic lines, the snippet is lost. **DROP candidate** — journal logs already preserve full output. |
| `protocol: Any = None` unused parameter | **DROP** | Dead parameter; not referenced inside the function. Drop on the rename. |

### `firmware/ble_dfu.upload_ble_dfu()` + helpers

| Protection | Status | Notes |
|---|---|---|
| `bootloader_address(app_address)` arithmetic | **GAP** | No BLE-DFU path exists in canonical. Must be invoked from `_run_ble_dfu_flash`. |
| DFU zip parsing with image_type detection | **GAP** | Same. |
| **`_preflight_ble_check` — RSSI + NUS round-trip BEFORE destructive bootloader entry** | **GAP** | **CRITICAL.** This is the brick-prevention guard. Without it, weak-signal BLE devices get bricked when the bootloader entry erases app flash and the link drops mid-DFU. Came from real 2026-05-16 cart_inner failure. `_run_ble_dfu_flash` MUST call this before `_ble_dfu_write_impl`. |
| `MIN_DFU_RSSI = -75 dBm` threshold | **GAP** | Same — preflight check. |
| `MAX_DFU_ATTEMPTS = 3` retry loop | **GAP** | No retry logic in canonical. Either replicate in `_ble_dfu_write_impl`, or expose as a `flash_device` kwarg. Today's behavior: 3 full DFU sequences with HCI reset between. |
| `_reset_hci_adapter()` between retry attempts | **GAP** | Best-effort `hciconfig hci0 reset` (falls back to `bluetoothctl power off/on`). Migrate alongside `MAX_DFU_ATTEMPTS`. |
| 20s wait between retries for bootloader re-advertising | **GAP** | Same — part of retry mechanism. |
| `clear_bluez_state` with `power_cycle=True` on first attempt | **GAP** | "bluetoothctl remove alone doesn't reliably clear it on all BlueZ versions" — comment is load-bearing. Need to preserve. |
| `DFU_TRANSFER_TIMEOUT = 900` (15-min single-transfer timeout) | **GAP** | Inside `_dfu_transfer`. Migrate as constants on the new path. |
| `INVALID_STATE` retry (15s wait + reboot detect) | **GAP** | Same. |
| Post-DFU verification (3 scan attempts, app addr first, fallback NUS+RSSI scan) | **GAP** | The canonical path uses `run_verify` for post-flash verification. `_run_ble_dfu_flash` should hand off to `run_verify` instead of duplicating — but `run_verify` is currently USB-CDC oriented (uses `is_serial_connected`). May need a BLE-specific verify path. **Design question: extend `run_verify` for BLE, or have `_run_ble_dfu_flash` keep its own post-DFU verify?** |
| Random-static-address-change fallback (NUS service + name + RSSI) | **GAP** | Same. nRF52840 can change BLE address after a full power cycle from DFU. The verify scan handles this case. Worth keeping. |
| `enter_bootloader_via_serial` callable (for routes_firmware path) | **MOVE/DROP** | If `_run_ble_dfu_flash` is called only via the orchestrator and the orchestrator owns transport selection, the "enter bootloader from serial" path becomes `select_transport`'s job. **Drop the pluggable callable; bake the behavior into the orchestrator.** |
| `enter_bootloader_via_ble` callable | **MOVE** | Same — bake into `_run_ble_dfu_flash`. |

### `manager._background_loop` auto-recovery branch

| Protection | Status | Notes |
|---|---|---|
| `_dfu_recovery_active: set[str]` global serialization gate | **MOVE** | Per-device lock + `_device_in_flight` (L1) covers in-flight. But "BLE adapter is exclusive" is a global constraint, not per-device. Need an `_adapter_in_use` flag. |
| Recovery firmware path + whitelist lazy load | **HAS** | `_recovery_firmware_path` / `_recovery_device_ids` exist on `FleetManager`. L3.1 fixes the reload-on-task-launch timing. |
| Empty whitelist = no-op | **HAS** | Same. |
| Filter: DFU_RECOVERY + known BLE address + in whitelist + `should_attempt_auto_recovery` | **HAS** | `should_attempt_auto_recovery` is on FleetManager. The pre-filter logic stays at the auto-recovery layer; `flash_device` is called only for already-filtered devices. |
| **`MAX_AUTO_RECOVERY_ATTEMPTS = 3` per-device limit** | **GAP** | No attempt-counting in canonical. Must add either as a `FlashJob` field or as state on `FleetManager` keyed by canonical device ID. |
| "Gave up" log fires once per giveup event (`gave_up_logged` flag) | **GAP** | Same. |
| Exponential backoff: 1/2/4/8 min (skipping 1/2/4/8 of the 60s ticks) | **GAP** | Same. |
| `get_dfu_lock(device.id)` non-blocking check (skip if manual flash holds it) | **HAS** | Replaced by L1's `_device_in_flight` check on the orchestrator side. |
| `pause_discovery()` for the duration | **GAP** | Need to move into `_run_ble_dfu_flash` (BLE-DFU specifically, not UF2). |
| `hold_reconnect(device.id, 600)` | **GAP** | Same. |
| `broadcaster.stop()` + verification guard | **GAP** | Need to move into `_run_ble_dfu_flash`. CRITICAL — BCM43455 single-radio constraint. |
| Broadcaster restart in `finally` | **GAP** | Same. |
| `ensure_dfu_zip` in `asyncio.to_thread` | **GAP** | Already noted — needs to move. |
| `asyncio.wait_for(upload_ble_dfu, timeout=600.0)` | **GAP** | Need to move. |
| Result-status check → device.state = DISCONNECTED on success | **GAP** | The canonical path doesn't reset device.state today. May or may not be needed depending on how the orchestrator emits state transitions. |
| `finally` cleanup ALWAYS (discard active, resume discovery, resume reconnect, restart broadcaster) | **GAP** | All four must move. |

### Route-level: `routes_firmware._flash_fleet_background`

| Protection | Status | Notes |
|---|---|---|
| `pause_discovery()` at job start | **MOVE** | Push into `flash_device()`. Every flash needs it. |
| `hold_reconnect(sid, 600)` for ALL serial devices at job start | **MOVE** | Same — but this is fleet-level (every sibling), not per-device. May belong at a thin "fleet-flash" orchestrator layer (a function that calls `flash_device()` for each device with this wrapper applied once). |
| `broadcaster.stop()` + verification guard | **MOVE** | Push into `flash_device()`. |
| Continue-on-error (one failed device doesn't block fleet) | **GAP** | Per-job error doesn't affect other jobs in canonical path (each is a separate `FlashJob`). Fleet-level "flash N devices serially" needs to keep this. Belongs in the new fleet-flash route. |
| Per-phase progress callback wired into `job.progress_message` | **GAP** | Canonical uses FlashJob seq + `bytes_written/bytes_total` for progress. Phase strings ("Releasing port" / "USB-resetting" / "Running uf2_upload.py") aren't propagated. **DROP candidate** if FlashJob state machine + bytes counter is UX-sufficient. |
| 600s per-device hard timeout | **GAP** | Canonical has 300s subprocess + 5-min verify but no overall flash-attempt timeout. Add a `flash_device(timeout=600)` kwarg or use `wait_until_terminal(600)` from the route layer. |
| `device.state = DeviceState.DISCONNECTED` in finally | **GAP** | See auto-recovery row above. |
| Inter-device 8s sleep + `udevadm settle --timeout=10` | **MOVE** | Fleet-level. Belongs in the new fleet-flash orchestrator that calls `flash_device()` in a loop, NOT in `flash_device()` itself. |
| `udevadm settle` non-zero exit → WARN | **MOVE** | Same. |
| Final 10s USB stabilization sleep | **MOVE** | Fleet-level cleanup. |
| `resume_reconnect` / `resume_discovery` / restart broadcaster in finally | **MOVE** | Need to land in `flash_device()` for per-flash, and at the fleet-flash layer for the all-siblings-hold. |
| Version verification phase (30s poll loop) | **GAP** | Canonical's `run_verify` does version match. But the route-level loop is fleet-wide — it polls UNTIL all flashed devices report a version, or 30s elapse. Need to keep this at the fleet orchestrator layer. |

### Route-level: `routes_devices` single-device flash

| Protection | Status | Notes |
|---|---|---|
| Hold time differs by transport (600s BLE / 120s serial) | **MOVE** | Push into `flash_device()` — the path knows the transport. |
| `get_dfu_lock` non-blocking 409 if already locked | **HAS** | L1's `_device_in_flight` covers it; the response status code is route-layer concern. |
| **Sibling-serial-device `hold_reconnect`** when flashing a serial device | **MOVE** | CRITICAL. UF2 USB reset disrupts ALL serial devices on the same hub. Must move into `flash_device()`. |
| Sibling holds INSIDE the lock context (guaranteed cleanup) | **MOVE** | Same. |
| Broadcaster stop + 503 if didn't stop | **MOVE** | Already covered above. |
| 600s timeout | **MOVE** | Already covered above. |
| 5s USB stabilization wait after serial flash (vs 10s on fleet) | **MOVE** | Reconcile to one value (suggest 8s — somewhere between the two). |
| `resume_reconnect` for primary + siblings, `resume_discovery`, restart broadcaster | **MOVE** | All into `flash_device()`. |

## Summary

**The new orchestrator (`_run_flash_job` / `_run_uf2_flash` /
`flash_uf2_for_job`) covers the per-flash protocol — the actual
write + verify mechanics — but NOT the surrounding fleet hygiene
(radio, discovery, reconnect, broadcaster, sibling holds).** Those
all live in the legacy route layer or in `_auto_recover_dfu_devices`.

For L3d (legacy delete) to be safe, **every "MOVE" / "GAP" row above
must have landed in either `flash_device()` (single-device wrappers)
or a new fleet-orchestrator layer (multi-device sequencing)**.

### Concrete L0 / L1 / L2 dependencies surfaced by this audit

**L0 — `_run_ble_dfu_flash` orchestrator** (prereq for L2):
1. DFU_RECOVERY state detection in `_build_transport_probe` (today
   stubbed False at `manager.py:514`).
2. `_preflight_ble_check` — RSSI + NUS round-trip. **Brick-prevention
   guard; do not skip.**
3. `bootloader_address()` + DFU zip parsing.
4. Bootloader entry via NUS (`"bootloader ble"`) with idempotent
   connect + 0.5s sleep — only when device is NOT already in DFU_RECOVERY.
5. `MAX_DFU_ATTEMPTS` retry with `_reset_hci_adapter` between + 20s
   re-advertise wait + cache clear with power-cycle on first attempt.
6. `DFU_TRANSFER_TIMEOUT = 900` per-transfer wrap.
7. Post-DFU verification (3 scan attempts with NUS+RSSI fallback for
   random-static-address-change case). **OR** hand off to a BLE-aware
   `run_verify` extension — design call.

**L1 additions** (per-device wrappers that move into `flash_device()`):
- `pause_discovery()` + `resume_discovery()` in `finally`.
- `hold_reconnect(device.id, hold_time)` + sibling holds (when serial).
- `broadcaster.stop()` + verification guard + restart in `finally`.
- `device.state = DeviceState.DISCONNECTED` in `finally`.
- 600s per-flash hard timeout.
- USB-stabilization sleep (8s, reconciling 5s/10s).
- Global `_adapter_in_use` flag for the BLE-exclusive constraint.

**L1 additions for migrated auto-recovery:**
- `MAX_AUTO_RECOVERY_ATTEMPTS = 3` per-device limit.
- Exponential backoff schedule (1/2/4/8 min).
- "Gave up" once-per-event log.

**Fleet-orchestrator layer (new, not yet built):**
- `flash_fleet(device_ids, firmware, ...)` — calls `flash_device` per
  device serially, owns inter-device `udevadm settle` + 8s sleep, fleet-
  wide hold_reconnect for all siblings, version-verification poll loop,
  continue-on-error semantics.

### DROP candidates (no equivalent needed in canonical)

- `phase` strings in progress callbacks (`"prepare"`, `"upload"`,
  `"done"`) — FlashJob state machine covers operator UX.
- `output[-500:]` snippet in result dict — journalctl has full output.
- `protocol: Any = None` unused parameter on `upload_uf2`.

### Open design questions

1. **BLE post-DFU verify vs canonical `run_verify`** — extend
   `run_verify` for BLE, or have `_run_ble_dfu_flash` keep its own
   3-attempt scan? Extending `run_verify` is cleaner but BLE re-discovery
   is fundamentally different from USB-CDC handshake (random-static
   address changes are a BLE-only concern).

2. **Phase progress strings** — useful operator UX or noise that
   FlashJob's state machine already covers? If the former, extend
   `FlashJob` with a phase field.

3. **600s overall flash timeout** — kwarg on `flash_device(timeout=)`,
   or call `wait_until_terminal(600)` from the route layer? The latter
   is cleaner (separation of concerns) but the route layer doesn't
   own the cancellation path that should abort an in-flight flash.

4. **Fleet-flash orchestrator location** — new function on
   `FleetManager` (`flash_fleet`), or new route handler that loops
   over `flash_device`? `FleetManager` is more testable.
