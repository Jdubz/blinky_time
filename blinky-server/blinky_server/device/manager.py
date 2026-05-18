from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import os
import time as _time
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, overload

from .. import systemd_notify
from ..ble.advertiser import FleetBroadcaster
from ..firmware import ble_dfu as _ble_dfu_mod
from ..firmware._guard import (
    enter_orchestrator_context,
    reset_orchestrator_context,
)
from ..firmware.anomalies import SignalHistory
from ..firmware.anomalies import check_all as check_anomalies
from ..firmware.flash_job import (
    FlashConflict,
    FlashJob,
    FlashJobState,
    FlashTransport,
    NoReachableTransport,
    TransportProbe,
    select_transport,
)
from ..firmware.uf2_upload import _uf2_write_impl_for_job
from ..firmware.verify import run_verify
from ..transport.base import Transport
from ..transport.discovery import (
    DiscoveredDevice,
    cleanup_stale_ble_connections,
    discover_all,
)
from ..transport.serial_transport import SerialTransport
from .device import Device, DeviceState
from .protocol import DeviceProtocol

log = logging.getLogger(__name__)

DISCOVERY_INTERVAL_S = 10
RECONNECT_INTERVAL_S = 5

# Max auto-recovery attempts per device. After this many consecutive
# failures, halt auto-recovery for that device and surface via the
# /api/fleet/status endpoint as recovery_blocked. Operator must clear
# the state manually (e.g. by physically inspecting the device) before
# auto-recovery resumes. Prevents hammering a device that's hung in a
# way our flash sequence can't fix.
MAX_AUTO_RECOVERY_ATTEMPTS = 3

# Exponential backoff between auto-recovery attempts (in seconds).
# Indexed by the prior consecutive-failure count: after fails=1 wait
# 60s, after fails=2 wait 120s, etc. Capped at the last entry. Keeps
# us from hammering a flaky BLE radio in tight succession while still
# being responsive enough that a transient interference window doesn't
# stretch into an outage.
RECOVERY_BACKOFF_SECONDS: tuple[float, ...] = (60.0, 120.0, 240.0, 480.0)

# How long to cache the on-disk whitelist file before rechecking its
# mtime. 10 s is long enough that a busy auto-recovery cycle doesn't
# stat() the file every iteration, short enough that an operator
# editing the JSON sees their change reflected on the next cycle.
RECOVERY_WHITELIST_RELOAD_TTL_S = 10.0


@dataclass
class _RecoveryRetryEntry:
    """Per-canonical auto-recovery state.

    Lives on FleetManager keyed by canonical device id. The orchestrator's
    `_run_flash_job` finally block writes it (increment on FAILED/ABANDONED,
    clear on COMPLETED); `should_attempt_auto_recovery` reads it (max
    attempts + exponential backoff). Auto-recovery loop calls
    `flash_device(force=False)` which consults the same checks.
    """

    fails: int = 0
    last_failure_at: float | None = None
    gave_up_logged: bool = False


# Reap non-recoverable devices that haven't communicated in this long.
# Removed from /api/devices so dead slots stop accumulating. Re-discovered
# fresh on the next scan if the device comes back.
# DFU_RECOVERY excluded — it represents a device stuck in bootloader; reaping
# would let it be re-discovered and stuck again in a flap loop.
REAP_THRESHOLD_S = 900.0  # 15 min
REAP_INTERVAL_CYCLES = 6  # every 6th background cycle (~60s)


def _create_transport(disc: DiscoveredDevice) -> Transport:
    """Create the appropriate transport for a discovered device."""
    if disc.transport_type == "serial":
        return SerialTransport(disc.address)
    elif disc.transport_type == "ble":
        from ..transport.ble_transport import BleTransport

        return BleTransport(disc.address)
    elif disc.transport_type == "wifi":
        from ..transport.wifi_transport import WifiTransport

        host = disc.extra.get("host")
        if not host:
            raise ValueError(
                f"WiFi device {disc.device_id!r} has no 'host' in extra: {disc.extra!r}"
            )
        port = disc.extra.get("port", 3333)
        return WifiTransport(host, port)
    else:
        raise ValueError(f"Unknown transport type: {disc.transport_type}")


class FleetManager:
    """Manages all connected blinky devices across all transport types."""

    def __init__(
        self,
        enable_ble: bool = True,
        enable_serial: bool = True,
        ble_timeout: float = 5.0,
        wifi_hosts: list[dict[str, Any]] | None = None,
    ) -> None:
        self._devices: dict[str, Device] = {}  # keyed by device_id
        self._discovery_task: asyncio.Task[None] | None = None
        self._background_tasks: set[asyncio.Task[None]] = (
            set()
        )  # prevent GC of fire-and-forget tasks
        self._running = False
        self._enable_ble = enable_ble
        self._enable_serial = enable_serial
        self._ble_timeout = ble_timeout
        self._wifi_hosts = wifi_hosts or []
        # Track discovery metadata for reconnection
        self._device_discovery: dict[str, DiscoveredDevice] = {}
        # In-memory reconnect blackout (device_id -> monotonic deadline)
        self._reconnect_blackout: dict[str, float] = {}
        # Reconnect failure count for exponential backoff (device_id -> count)
        self._reconnect_failures: dict[str, int] = {}
        # Per-device backoff skip counter (device_id -> cycles skipped so far)
        self._backoff_skip: dict[str, int] = {}
        # Discovery pause counter — when > 0, background loop skips discovery/reconnect.
        # Used during BLE DFU to avoid BleakScanner conflicts (only one scan at a time).
        self._discovery_pause_count: int = 0
        # BLE addresses excluded from discovery because they were deduped with a
        # serial device. Prevents the thrashing loop where a deduped device is
        # immediately rediscovered and reconnected on every cycle.
        self._dedup_excluded: set[str] = set()
        # DFU recovery state (per-device tracking for auto-retry).
        self._recovery_firmware_path: str | None = None
        # Whitelist of device IDs eligible for auto-recovery. Empty = nothing
        # auto-recovers, even if a firmware path is set — the whitelist is
        # the safety belt against flashing a dev unit that happens to be in
        # DFU at the same time the cart fleet is being deployed.
        self._recovery_device_ids: set[str] = set()
        # Per-canonical auto-recovery retry state. The orchestrator's
        # `_run_flash_job` finally writes it; `should_attempt_auto_recovery`
        # reads it to enforce max attempts + exponential backoff for
        # `flash_device(force=False)` callers. The `_device_in_flight` set
        # is the cross-caller mutex for "at most one flash per device."
        self._recovery_retry_state: dict[str, _RecoveryRetryEntry] = {}
        # L3.1: on-disk whitelist reload cache. We stat() the file at most
        # once per RECOVERY_WHITELIST_RELOAD_TTL_S, and only re-load the
        # JSON when mtime changes. Closes the "operator edited the file
        # but the server has the old whitelist cached" gap.
        self._last_whitelist_check_at: float = 0.0
        self._last_whitelist_mtime: float | None = None
        # One-shot WARN latch for "file disappeared after we'd loaded it"
        # — prevents the auto-recovery cycle (every ~60s) from spamming
        # the journal with the same message indefinitely. Reset when the
        # file becomes readable again.
        self._whitelist_missing_warned: bool = False
        # ── Flash-job rewrite (Phase 3) ───────────────────────────────────
        # The next-generation flash entry point lives in ``flash_device``
        # below. Per-canonical in-flight set + ``_flash_job_locks`` enforce
        # "at most one flash per device" across all callers (the L3a/L3b/L3c
        # routes + auto-recovery).
        #
        # _flash_jobs: device_id → currently-or-most-recently-active FlashJob.
        # Cleared from active by ``_run_flash_job`` when the job terminates;
        # see ``_recent_flash_attempts`` for completed-flash dedup state.
        self._flash_jobs: dict[str, FlashJob] = {}
        # _recent_flash_attempts: device_id → time.time() of the most recent
        # terminal transition. ``flash_device(force=False)`` consults this
        # (via ``should_attempt_auto_recovery``) and the window below to
        # dedup; explicit operator requests (``force=True``) ignore the
        # window (operator already accepted the cost). Closes the cascade
        # bug from 2026-05-17 where UF2 succeeded, was mislabelled as
        # failed, and then auto-recovery re-flashed via BLE-DFU. See
        # [[project-deploy-flash-cascade-bug]].
        #
        # TODO(L4): this is an in-memory dict; the dedup window is lost on
        # server restart. A device that failed 30 s ago looks "clean" after
        # a quick restart, so the auto-recovery loop could re-fire
        # immediately. The persistent flash-attempt audit log specced in
        # docs/FLASH_LOCKDOWN_PLAN.md §L4 closes this gap by rebuilding
        # this dict from `/var/lib/blinky-server/flash_attempts.jsonl` on
        # boot. Until L4 lands, restart timing matters — the auto-recovery
        # loop's per-cycle dedup-gate is the only line of defense.
        self._recent_flash_attempts: dict[str, float] = {}
        # 10-minute dedup window. Slack accommodates: a slow UF2 + bootloader
        # reboot (up to ~2 min on 0.8.0-4), plus the app's first-boot init,
        # plus a margin for the operator to notice anomalies. Tuned via
        # ``FLASH_DEDUP_WINDOW_S`` class constant — overridable per test.
        self._flash_job_locks: dict[str, asyncio.Lock] = {}
        # ── L1.1: canonical-key resolver for cross-transport identity ────
        # A single physical device can be addressed by multiple IDs over
        # its lifetime: its USB serial number (when plugged in app-mode),
        # its BLE app-mode address (when present-only), and its BLE
        # bootloader address (`app_address - 1`, when in DFU recovery).
        # All three are "the same device" — but absent a canonical key,
        # data structures like `_recovery_device_ids` (whitelist) or
        # the new `_device_in_flight` set treat them as different
        # entities. That gap is what kept the 2026-05-16 cart_inner
        # SN-only whitelist from matching the BLE-only DFU advert.
        #
        # `_identity_groups` maps every known alias to its canonical key.
        # Canonical preference: USB serial number (no colons, 16 hex
        # chars typical) > BLE app address > first ID seen. The
        # `resolve_canonical()` method is a direct dict lookup that
        # returns the input unchanged for unknown IDs — so callers can
        # always wrap a lookup, and new IDs flow through unchanged
        # until `register_identity_alias()` registers them.
        #
        # Wired by discovery in follow-up work; L1.1 lands the
        # infrastructure standalone so L1's `_device_in_flight` can
        # use canonical keys from day one.
        self._identity_groups: dict[str, str] = {}
        # ── Single in-flight set ────────────────────────────────────────
        # Populated when a `FlashJob` enters PENDING, cleared on terminal
        # transition. Anything that touches firmware MUST check
        # membership here first (after resolving to canonical). This is
        # the single mutex for "at most one flash per device" — every
        # caller (operator route, fleet flash, auto-recovery) routes
        # through `flash_device` which honors this. Keys are canonical
        # IDs from `resolve_canonical()`.
        self._device_in_flight: set[str] = set()
        # Background-loop health. Updated after every successful cycle so
        # external watchdogs (systemd, /api/fleet/status) can detect a wedge
        # — `_running == True` is not enough, the loop can be alive but stuck.
        self._loop_last_ok: float | None = None
        self._loop_cycles: int = 0
        self._loop_consecutive_errors: int = 0
        # Separate watchdog-pinger task. Runs independently of the main
        # background loop so a long inline await (e.g. an 8-min BLE DFU
        # called from inside the loop's auto-recovery branch) does NOT
        # block the systemd watchdog ping. Whatever the main loop is
        # doing, this task pings on a fixed cadence while the asyncio
        # event loop is alive — which is exactly what the systemd
        # watchdog wants to know. A hard event-loop deadlock still
        # SIGKILLs us, as intended.
        # Verified necessary 2026-05-16: a ping inside the loop's pause
        # path doesn't fire when an auto-recovery iteration awaits a
        # multi-minute BLE-DFU flash inline — the loop never returns to
        # the top of while until the orchestrator's wait completes.
        self._watchdog_task: asyncio.Task[None] | None = None
        # Fleet broadcaster — BLE radio-style command channel. Owned by
        # the manager so its lifecycle matches the fleet's; routes get at
        # it via ``self.broadcaster``. Started in :meth:`start`; may be
        # None when BLE is disabled (``enable_ble=False``).
        self.broadcaster: FleetBroadcaster | None = None

    @property
    def devices(self) -> dict[str, Device]:
        return self._devices

    def loop_health(self) -> dict[str, Any]:
        """Background-loop health snapshot for external watchdogs and APIs.

        ``last_cycle_ago`` is None until the first successful cycle finishes
        (boot window); after that, ``None`` should be treated as a hang by
        consumers. ``consecutive_errors > 0`` means the loop is failing but
        still trying.
        """
        last_ok = self._loop_last_ok
        ago = (_time.monotonic() - last_ok) if last_ok is not None else None
        return {
            "running": self._running,
            "cycles": self._loop_cycles,
            "last_cycle_ago": round(ago, 1) if ago is not None else None,
            "consecutive_errors": self._loop_consecutive_errors,
        }

    def get_device(self, device_id: str) -> Device | None:
        """Look up device by ID. Supports partial ID prefix match."""
        if device_id in self._devices:
            return self._devices[device_id]
        # Try prefix match (for convenience with long serial numbers)
        matches = [d for did, d in self._devices.items() if did.startswith(device_id)]
        return matches[0] if len(matches) == 1 else None

    def get_all_devices(self) -> list[Device]:
        return list(self._devices.values())

    # ────────────────────────────────────────────────────────────────────
    # Flash-job API (see ``docs/FLASH_LOCKDOWN_PLAN.md`` and
    # ``[[project-deploy-flash-cascade-bug]]``)
    #
    # ``flash_device`` is THE single entry point — operator routes
    # (``POST /api/devices/{id}/flash``, fleet flash via ``flash_fleet``),
    # auto-recovery, and any future caller all converge here. The
    # per-device lock + ``_flash_jobs`` table + ``_device_in_flight``
    # canonical-keyed set enforce "at most one in-flight flash per
    # device" — preventing the duplicate write the 2026-05-17 cascade
    # produced. The orchestrator-context ContextVar (see
    # ``firmware/_guard.py``) makes the invariant structural: a write
    # impl called outside ``_run_flash_job`` raises before it touches
    # firmware.
    # ────────────────────────────────────────────────────────────────────

    # Window in seconds during which a freshly-completed (or freshly-failed)
    # flash blocks auto-recovery from triggering a new flash on the same
    # device. Operator-initiated flashes via ``flash_device`` ignore this.
    # Per-test override possible by mutating ``FleetManager.FLASH_DEDUP_WINDOW_S``.
    FLASH_DEDUP_WINDOW_S: float = 600.0  # 10 minutes

    # ── L1.1: canonical-key resolver ───────────────────────────────────
    #
    # A device can have multiple IDs in flight at once (USB SN, BLE app
    # addr, BLE bootloader addr = app addr - 1). The canonical key
    # collapses them so a flash attempt or whitelist match doesn't miss
    # the device just because it transitioned transports between the
    # check and the action. The 2026-05-16 cart_inner brick was the
    # textbook case: the recovery whitelist had the SN, but the device
    # was in DFU advertising only its BLE bootloader address — so the
    # whitelist couldn't match and auto-recovery never fired.

    def resolve_canonical(self, device_id: str) -> str:
        """Return the canonical key for `device_id`.

        Returns the input unchanged if no alias has been registered for
        it yet — this is the right behavior for "first time we've seen
        this ID" and makes the resolver safe to wrap around any lookup.
        """
        return self._identity_groups.get(device_id, device_id)

    def register_identity_alias(self, *ids: str) -> str:
        """Register multiple IDs as aliases for one physical device.

        Picks a canonical key from the union of all known aliases of
        any input ID, plus the inputs themselves. Preference order:

        1. A USB serial number (no colons, ≥12 chars). Most stable —
           survives BLE-address-rotation on power cycle.
        2. A BLE app address (with colons).
        3. The first ID passed.

        Returns the canonical key. Safe to call repeatedly with
        overlapping ID sets; each call collapses any existing groups.
        Raises ``ValueError`` on empty input — the API contract is
        "tell me what's the same device," empty has no meaning.
        """
        if not ids:
            raise ValueError("register_identity_alias requires at least one id")

        # Collect every alias that's already known for any input ID,
        # plus the inputs themselves. The union is "everything the
        # caller has just said belongs to one physical device."
        all_aliases: set[str] = set(ids)
        for i in ids:
            existing = self._identity_groups.get(i)
            if existing is not None:
                all_aliases.add(existing)
                # Also pull in everything that pointed to this canonical.
                for alias, canonical in self._identity_groups.items():
                    if canonical == existing:
                        all_aliases.add(alias)

        # Pick canonical from the merged group.
        def _looks_like_sn(s: str) -> bool:
            return ":" not in s and len(s) >= 12

        sn_candidates = [a for a in all_aliases if _looks_like_sn(a)]
        if sn_candidates:
            canonical = sorted(sn_candidates)[0]  # deterministic if multiple
        else:
            ble_candidates = [a for a in all_aliases if ":" in a]
            canonical = sorted(ble_candidates)[0] if ble_candidates else sorted(all_aliases)[0]

        # Map every alias (including the canonical) to the canonical.
        for alias in all_aliases:
            self._identity_groups[alias] = canonical

        return canonical

    def get_flash_job(self, device_id: str) -> FlashJob | None:
        """Return the most recent ``FlashJob`` for the device, or None.

        Resolves through ``_identity_groups`` so a caller passing the
        BLE address gets the job created under the device's USB SN
        (and vice versa) as long as the alias has been registered.
        """
        return self._flash_jobs.get(self.resolve_canonical(device_id))

    def list_flash_jobs(self, active_only: bool = False) -> list[FlashJob]:
        """Snapshot of all known flash jobs. Used by ``GET /api/jobs``."""
        if active_only:
            return [j for j in self._flash_jobs.values() if j.is_active]
        return list(self._flash_jobs.values())

    def should_attempt_auto_recovery(self, device_id: str) -> bool:
        """Decide whether auto-recovery may flash this device right now.

        Returns False if any of:
          - the device (under its canonical ID) is in the
            ``_device_in_flight`` set, OR
          - an active flash job already exists for the device, OR
          - a recent flash terminated within ``FLASH_DEDUP_WINDOW_S``, OR
          - the device has hit ``MAX_AUTO_RECOVERY_ATTEMPTS`` consecutive
            auto-recovery failures (L3c), OR
          - the device is inside the exponential backoff window for its
            current `fails` count (L3c — schedule from
            ``RECOVERY_BACKOFF_SECONDS``).

        Caller passes whatever ID they have — BLE addr or SN. The
        canonical resolver collapses aliases so a flash that started
        under one ID dedups against an auto-recovery attempt under the
        other. This is the 2026-05-16 cart_inner brick avoidance: the
        legacy SN-only whitelist couldn't match the BLE-only DFU
        device, so auto-recovery never fired even though the device
        was eligible.

        Auto-recovery calls this via ``flash_device(force=False)``: that
        path consults this method and returns ``None`` if False. The
        explicit-flash path (``force=True``, the route default) ignores
        this check — the operator has already accepted the cost.
        """
        canonical = self.resolve_canonical(device_id)
        if canonical in self._device_in_flight:
            return False
        existing = self._flash_jobs.get(canonical)
        if existing is not None and existing.is_active:
            return False
        last = self._recent_flash_attempts.get(canonical)
        if last is not None and (_time.time() - last) < self.FLASH_DEDUP_WINDOW_S:
            return False
        # Max-attempts + exponential backoff. The orchestrator owns
        # this gating so it applies uniformly to every caller that
        # invokes `flash_device(force=False)` (currently just the
        # auto-recovery loop; future callers get the same protection).
        retry = self._recovery_retry_state.get(canonical)
        if retry is not None:
            if retry.fails >= MAX_AUTO_RECOVERY_ATTEMPTS:
                return False
            if retry.last_failure_at is not None and retry.fails > 0:
                # Pick backoff seconds for this fails count (clamped to the
                # last entry — the highest fails count just keeps that
                # ceiling indefinitely until max-attempts kicks in).
                idx = min(retry.fails - 1, len(RECOVERY_BACKOFF_SECONDS) - 1)
                backoff_s = RECOVERY_BACKOFF_SECONDS[idx]
                if (_time.time() - retry.last_failure_at) < backoff_s:
                    return False
        return True

    def _get_flash_job_lock(self, device_id: str) -> asyncio.Lock:
        """Lazy per-device lock. Creates on first request."""
        lock = self._flash_job_locks.get(device_id)
        if lock is None:
            lock = asyncio.Lock()
            self._flash_job_locks[device_id] = lock
        return lock

    @overload
    async def flash_device(
        self,
        device_id: str,
        firmware_path: Path,
        *,
        expected_version: str | None = None,
        force: Literal[True] = True,
    ) -> FlashJob: ...

    @overload
    async def flash_device(
        self,
        device_id: str,
        firmware_path: Path,
        *,
        expected_version: str | None = None,
        force: Literal[False],
    ) -> FlashJob | None: ...

    async def flash_device(
        self,
        device_id: str,
        firmware_path: Path,
        *,
        expected_version: str | None = None,
        force: bool = True,
    ) -> FlashJob | None:
        """Flash request — operator-explicit (``force=True``, default) or
        auto-recovery (``force=False``).

        Idempotent under concurrent calls: if another caller's flash job
        for the same device is already in flight, returns that job rather
        than creating a second one. (This is the structural fix for the
        cascade bug — there is no path to "two writes for one device.")

        ``force=True`` (operator-explicit): always proceeds. Does NOT
        consult the dedup window or the auto-recovery retry counter —
        the operator has already accepted the cost.

        ``force=False`` (auto-recovery, L3c): consults
        ``should_attempt_auto_recovery`` (dedup window, max attempts,
        exponential backoff) and returns ``None`` if any of those
        guards say no. Returning a ``FlashJob`` means we passed the
        guards and the orchestrator is scheduled.

        The actual write happens on a background task, so the returned job
        starts in ``PENDING`` or ``SELECTING_TRANSPORT`` and the caller
        observes progress via ``job.wait_until_terminal()`` /
        ``GET /api/jobs/{job_id}`` polling.
        """
        # Normalize the caller-supplied identifier to the FULL device.id
        # stored in `self._devices`. `get_device` accepts a prefix or
        # alias and returns the matching `Device`; we then carry its
        # `.id` through the rest of the function. This matters because
        # `_run_flash_job` and the branch methods (`_run_uf2_flash`,
        # `_run_ble_dfu_flash`) probe `self._devices.get(job.device_id)`
        # downstream — and that lookup requires the FULL key, not a
        # prefix or an alias. A caller passing "FA:E6" (prefix) would
        # otherwise get a job whose orchestrator path immediately fails
        # with "device disappeared between selection and write."
        # Unknown devices fall through with the original input so the
        # orchestrator's not-found path fires with the operator's input
        # quoted back at them.
        device_obj = self.get_device(device_id)
        full_device_id = device_obj.id if device_obj is not None else device_id

        # Resolve to canonical key for lookups + in-flight dedup. Two
        # operators racing on the same physical device via different
        # aliases (SN vs BLE addr) both resolve to the same canonical
        # and dedup correctly.
        canonical = self.resolve_canonical(full_device_id)
        lock = self._get_flash_job_lock(canonical)
        async with lock:
            existing = self._flash_jobs.get(canonical)
            if existing is not None and existing.is_active:
                # Idempotent return ONLY for identical requests
                # (same firmware path + same expected_version). A
                # different firmware on a flash-in-flight is a
                # genuine conflict: returning the existing job would
                # report the FIRST request's success/failure as if it
                # were the SECOND request's, and the operator would
                # assume their NEW image landed.
                same_firmware = (
                    str(existing.firmware_path) == str(firmware_path)
                    and existing.expected_version == expected_version
                )
                if same_firmware:
                    log.info(
                        "flash_device(%s): returning in-flight job %s (state=%s, same request)",
                        device_id,
                        existing.job_id,
                        existing.state.value,
                    )
                    return existing
                if not force:
                    # Auto-recovery defers to an in-flight operator
                    # flash even with different firmware — better to
                    # let the operator's image land than to schedule
                    # a parallel auto-recovery write (which would
                    # block on the in-flight set anyway). Returning
                    # the existing job lets the caller wait on it.
                    log.info(
                        "flash_device(%s, force=False): in-flight job %s has "
                        "different firmware (%s vs %s); deferring (returning "
                        "in-flight job)",
                        device_id,
                        existing.job_id,
                        existing.firmware_path,
                        firmware_path,
                    )
                    return existing
                raise FlashConflict(
                    f"flash for {device_id[:12]} already in flight with a "
                    f"different firmware (existing: {existing.firmware_path} "
                    f"expected_version={existing.expected_version!r}; "
                    f"requested: {firmware_path} "
                    f"expected_version={expected_version!r}). Wait for the "
                    f"in-flight job {existing.job_id} or cancel it before "
                    f"re-flashing with a different image."
                )
            if not force and not self.should_attempt_auto_recovery(full_device_id):
                # Auto-recovery dedup said no (dedup window, max attempts,
                # or backoff). Don't schedule — caller (auto-recovery
                # loop) is responsible for trying again next cycle.
                log.debug(
                    "flash_device(%s, force=False): blocked by should_attempt_auto_recovery",
                    device_id,
                )
                return None
            job = FlashJob(
                device_id=full_device_id,
                firmware_path=firmware_path,
                expected_version=expected_version,
                canonical_id=canonical,
                is_auto_recovery=not force,
            )
            self._flash_jobs[canonical] = job
            # Register in the single in-flight set so any other code path
            # that checks before touching firmware sees this device is
            # occupied. Cleared by `_run_flash_job`'s `finally` block
            # under the same lock. The job pins ``canonical_id`` so a
            # mid-flight alias shift in ``_identity_groups`` doesn't
            # cause the finally cleanup to write under a different key.
            self._device_in_flight.add(canonical)
            log.info(
                "flash_device(%s, force=%s): created job %s, firmware=%s, expected_version=%s",
                device_id,
                force,
                job.job_id,
                firmware_path,
                expected_version,
            )
        # Launch the orchestrator OUTSIDE the lock so a long-running flash
        # doesn't block subsequent flash_device queries for the same device
        # (those will see is_active=True and return the in-flight job).
        task = asyncio.create_task(self._run_flash_job(job), name=f"flash-job-{job.job_id}")
        self._background_tasks.add(task)
        task.add_done_callback(self._background_tasks.discard)
        return job

    async def flash_fleet(
        self,
        device_ids: list[str],
        firmware_path: Path,
        *,
        per_device_timeout: float = 600.0,
        expected_version: str | None = None,
        on_iteration: Callable[[int, int, str, FlashJob | None, str | None], None] | None = None,
    ) -> AsyncIterator[tuple[str, FlashJob | None, str | None]]:
        """Strictly-serial multi-device flash.

        Per ``feedback_flash_safety_policy`` ("one device at a time,
        finish fully before next"): each device's flash reaches a
        terminal state (``COMPLETED`` / ``FAILED`` / ``ABANDONED``)
        before the next device's flash is scheduled. No parallelism
        across devices; that's a structural defense against
        cross-device interference (USB bus reset disrupting siblings,
        BLE radio contention between simultaneous BLE-DFU flashes).

        Yields ``(device_id, job, error)`` for each device as it
        finishes:
          - ``(id, FlashJob, None)`` — flash reached terminal state;
            inspect ``job.state`` for COMPLETED vs FAILED/ABANDONED.
          - ``(id, None, msg)`` — device couldn't be scheduled
            (missing from the manager, wrong platform, etc.).
          - ``(id, job, "timeout")`` — ``wait_until_terminal`` exceeded
            the per-device cap; the orchestrator may still be running
            in the background (its own internal caps will eventually
            terminate it). Surfaces the timeout to the caller so
            progress reporting can show it.

        ``on_iteration(index, total, device_id, job, error)`` is
        invoked synchronously between iterations. The JobManager
        super-job's progress callback uses this to update its visible
        progress + message without needing to consume the generator
        out-of-band.

        Batch-level coordination (done ONCE around the whole loop):
          - Broadcaster paused if running, restored in ``finally``. The
            orchestrator's per-device BLE-DFU branch sees
            ``broadcaster_was_running=False`` and skips its own
            stop/start — saves churn between BLE-DFU iterations.

        Per-iteration coordination (per device, scoped to that flash):
          - Sibling-serial ``hold_reconnect`` 600s for UF2 targets — the
            UF2 USB bus reset disrupts other serial devices on the same
            hub, so we freeze their reconnect timers across the flash.
            Released between iterations so each next-target's siblings
            are computed fresh. (Notably absent: ``pause_discovery`` and
            target ``hold_reconnect`` — both block the orchestrator's
            verify-via-reconnect; see ``project_deploy_flash_cascade_bug``
            and the L3a hardware-test post-mortem.)
          - ``udevadm settle`` + 8 s sleep after every UF2 iteration
            except the last, so the kernel's USB event queue drains
            before the next device's reset.
        """
        broadcaster_was_running = self.broadcaster is not None and self.broadcaster.is_running
        if broadcaster_was_running and self.broadcaster is not None:
            try:
                await self.broadcaster.stop()
            except Exception:
                log.exception(
                    "flash_fleet: broadcaster.stop() raised; refusing to proceed "
                    "under uncertain radio state"
                )
            # Fail-loud — refuse to proceed if the radio is still advertising.
            # Mid-flash radio contention against the BCM43455 is what
            # bricked cart_inner on 2026-05-16, so this is a hard gate.
            if self.broadcaster is not None and self.broadcaster.is_running:
                err_msg = (
                    "broadcaster did not stop cleanly; flash_fleet aborted "
                    "to avoid BLE radio contention"
                )
                log.error("flash_fleet: %s", err_msg)
                for did in device_ids:
                    yield (did, None, err_msg)
                return

        total = len(device_ids)
        try:
            for i, device_id in enumerate(device_ids):
                err: str | None = None
                # Raw lookup (with prefix-match fallback) — NOT canonical
                # resolution. When the same physical device has multiple
                # entries (e.g., USB SN + BLE app addr), the caller's
                # choice of identity selects which entry's transport we
                # flash through. Canonical resolution would collapse them
                # and might pick the wrong transport (e.g., flashing via
                # the stale USB entry when the BLE entry is the one in
                # DFU_RECOVERY). The orchestrator's `flash_device`
                # canonical-resolves on its own for the in-flight dedup,
                # which is the right place for that.
                device = self.get_device(device_id)
                if device is None:
                    err = f"device {device_id} not found in fleet"
                    log.warning("flash_fleet[%d/%d]: %s", i + 1, total, err)
                    if on_iteration is not None:
                        on_iteration(i, total, device_id, None, err)
                    yield (device_id, None, err)
                    continue

                # Per-iteration sibling holds (UF2 only — BLE targets don't
                # USB-bus-reset, so they don't disrupt serial siblings).
                is_serial_target = device.transport.transport_type == "serial"
                sibling_ids: list[str] = []
                if is_serial_target:
                    for sibling in self.get_all_devices():
                        if sibling.id != device.id and sibling.transport.transport_type == "serial":
                            self.hold_reconnect(sibling.id, 600)
                            sibling_ids.append(sibling.id)

                job: FlashJob | None = None
                try:
                    # Use ``device.id`` (the full identity stored in
                    # ``self._devices``) rather than the caller-supplied
                    # ``device_id`` — ``get_device`` accepts a prefix, but
                    # downstream lookups inside ``_run_flash_job`` /
                    # ``_run_uf2_flash`` / ``_run_ble_dfu_flash`` use
                    # ``job.device_id`` to read ``self._devices``, which
                    # requires the FULL key.
                    job = await self.flash_device(
                        device.id,
                        firmware_path,
                        expected_version=expected_version,
                    )
                    terminal = await job.wait_until_terminal(timeout=per_device_timeout)
                    if not terminal:
                        err = "timeout"
                        log.error(
                            "flash_fleet[%d/%d] %s: wait_until_terminal hit %ss cap",
                            i + 1,
                            total,
                            device_id[:12],
                            per_device_timeout,
                        )
                except Exception as exc:
                    err = f"flash_device/wait raised: {exc}"
                    log.exception("flash_fleet[%d/%d] %s: %s", i + 1, total, device_id[:12], err)
                finally:
                    for sid in sibling_ids:
                        self.resume_reconnect(sid)

                if on_iteration is not None:
                    on_iteration(i, total, device_id, job, err)
                yield (device_id, job, err)

                # Inter-iteration USB settle (UF2 only — BLE doesn't need
                # USB-bus stabilization). Skip after the last iteration.
                if is_serial_target and i < total - 1:
                    await asyncio.sleep(8)
                    try:
                        proc = await asyncio.create_subprocess_exec(
                            "udevadm",
                            "settle",
                            "--timeout=10",
                            stdout=asyncio.subprocess.DEVNULL,
                            stderr=asyncio.subprocess.DEVNULL,
                        )
                        await proc.wait()
                        # rc=1 means the udev queue was still non-empty after
                        # 10s — the host-USB-jam pattern from 2026-05-01.
                        # Don't fail the deploy (the next device's flash
                        # retries enumeration independently), but log
                        # loudly so the operator sees the early warning.
                        if proc.returncode != 0:
                            log.warning(
                                "flash_fleet: udevadm settle exited rc=%d after 10s — "
                                "USB event queue still busy; next device's flash may "
                                "hit a _wait_for_uf2_drive timeout",
                                proc.returncode,
                            )
                    except (FileNotFoundError, OSError) as exc:
                        log.warning("flash_fleet: udevadm settle skipped: %s", exc)
        finally:
            if broadcaster_was_running and self.broadcaster is not None:
                try:
                    await self.broadcaster.start()
                except Exception:
                    log.exception(
                        "flash_fleet: failed to restart broadcaster — fleet "
                        "commands will not reach BLE devices until manual restart"
                    )

    async def _run_flash_job(self, job: FlashJob) -> None:
        """Drive a ``FlashJob`` through its state machine.

        This is THE orchestrator entry point — every flash in the system
        runs through it, and the ContextVar guard
        (``firmware/_guard.py``) makes that invariant structural.
        Dispatches to ``_run_uf2_flash`` or ``_run_ble_dfu_flash`` based
        on transport selection; both branches transition the job through
        WRITING → VERIFYING → COMPLETED (or FAILED on error).

        Flow (UF2):
          PENDING → SELECTING_TRANSPORT → WRITING (subprocess writes
          UF2 to MSC) → VERIFYING (``run_verify`` polls device signals)
          → COMPLETED. The verify phase NEVER auto-fails on wall-clock;
          orchestrator gives it ~5 min of polling and surfaces anomalies
          if signals don't progress.

        Flow (BLE-DFU):
          PENDING → SELECTING_TRANSPORT → WRITING (``_ble_dfu_write_impl``
          runs the Nordic Legacy DFU protocol) → VERIFYING → COMPLETED.

        Always records ``_recent_flash_attempts`` and updates
        ``_recovery_retry_state`` on terminal transition, regardless of
        success/failure — so ``flash_device(force=False)`` dedup catches
        "I just tried this device" and max-attempts/backoff fire correctly
        for the next auto-recovery cycle.
        """
        # Enter the orchestrator context. Every guarded write impl
        # (`_uf2_write_impl`, `_uf2_write_impl_for_job`,
        # `_ble_dfu_write_impl`) checks the ContextVar at entry and
        # raises ``OutsideFlashJobContextError`` if it's not set.
        # Setting it here gates ALL paths reachable from this function
        # — the lockdown's "single entry point" invariant. Reset in
        # the finally block so cross-task contamination is impossible.
        _orch_token = enter_orchestrator_context()
        try:
            job.transition(FlashJobState.SELECTING_TRANSPORT)
            probe = self._build_transport_probe(job.device_id)
            try:
                transport = select_transport(probe)
            except NoReachableTransport as exc:
                job.set_error(str(exc))
                job.transition(FlashJobState.FAILED)
                return
            job.set_transport(transport)

            if transport is FlashTransport.UF2:
                await self._run_uf2_flash(job)
            elif transport is FlashTransport.BLE_DFU:
                await self._run_ble_dfu_flash(job)
            else:
                # Exhaustiveness guard — if FlashTransport gains a variant,
                # we want a loud error, not a silent fallthrough.
                job.set_error(f"unknown transport: {transport!r}")
                job.transition(FlashJobState.FAILED)
        except asyncio.CancelledError:
            # Operator-initiated cancel propagated through. Mark abandoned.
            if not job.is_terminal:
                job.transition(FlashJobState.ABANDONED)
            raise
        except Exception as exc:
            log.exception("flash job %s crashed: %s", job.job_id, exc)
            job.set_error(f"orchestrator crashed: {exc}")
            if not job.is_terminal:
                job.transition(FlashJobState.FAILED)
        finally:
            # Stamp dedup timestamp + clear in-flight + update retry
            # counter on every terminal exit. Held under the per-device
            # lock (keyed by canonical ID) so a concurrent
            # ``should_attempt_auto_recovery`` call sees a consistent
            # (jobs + timestamps + in-flight + retry-counter) snapshot.
            # Use ``job.canonical_id`` (pinned at creation) rather than
            # ``resolve_canonical(job.device_id)`` so a mid-flight alias
            # shift can't cause us to clean up under a different key.
            # Tests that construct ``FlashJob`` directly leave
            # ``canonical_id=None``; the fallback re-resolves which is
            # the same code path as before and works fine for them.
            canonical = (
                job.canonical_id
                if job.canonical_id is not None
                else self.resolve_canonical(job.device_id)
            )
            lock = self._get_flash_job_lock(canonical)
            async with lock:
                self._recent_flash_attempts[canonical] = _time.time()
                self._device_in_flight.discard(canonical)
                # Update the auto-recovery retry counter so the next
                # ``flash_device(force=False)`` for this device honors
                # max-attempts and backoff. Behavior:
                #   - COMPLETED (any caller): pop the counter. A
                #     successful flash means the device is healthy —
                #     even an operator-driven success clears prior
                #     auto-recovery failures so the next cycle can
                #     retry if anything goes wrong later.
                #   - FAILED/ABANDONED with is_auto_recovery=True:
                #     bump the counter. Auto-recovery making progress
                #     toward its 3-strike budget.
                #   - FAILED/ABANDONED with is_auto_recovery=False
                #     (operator-driven): leave the counter alone.
                #     Operator failures are the operator's concern;
                #     they should not eat into auto-recovery's budget
                #     for the same device.
                if job.state is FlashJobState.COMPLETED:
                    self._recovery_retry_state.pop(canonical, None)
                elif job.is_auto_recovery and job.state in (
                    FlashJobState.FAILED,
                    FlashJobState.ABANDONED,
                ):
                    entry = self._recovery_retry_state.get(canonical) or _RecoveryRetryEntry()
                    entry.fails += 1
                    entry.last_failure_at = _time.time()
                    # Reset the "gave up" log latch when fails crosses
                    # MAX_AUTO_RECOVERY_ATTEMPTS for the first time —
                    # next auto-recovery cycle will log the giveup once.
                    if entry.fails == MAX_AUTO_RECOVERY_ATTEMPTS:
                        entry.gave_up_logged = False
                    self._recovery_retry_state[canonical] = entry
                # L4: append to the persistent audit log so the dedup
                # window survives a server restart. INSIDE the lock so
                # any concurrent reader sees the in-memory
                # _recent_flash_attempts update and the on-disk entry
                # as a single consistent snapshot — and so a crash
                # between the in-memory update and the on-disk write
                # is impossible (defeats the whole point of L4 for
                # this attempt). Sync I/O held under the lock is
                # ~1 ms; the flash itself is seconds-to-minutes, so
                # the lock-hold cost is negligible. The write
                # tolerates OSError internally (logs WARN), so a
                # disk failure doesn't propagate out of the finally.
                self._write_flash_audit_entry(job, canonical)
            # Reset the orchestrator-context-var. Token-based reset
            # restores the prior value (default False, or — if some
            # future code composes orchestrator entries — the outer
            # True). Done LAST so any error during dedup-table cleanup
            # doesn't leak the context-var into a subsequent task.
            reset_orchestrator_context(_orch_token)

    async def _run_uf2_flash(self, job: FlashJob) -> None:
        """UF2 branch of ``_run_flash_job``.

        The split from ``_run_ble_dfu_flash`` exists for testability —
        each transport gets its own method so monkeypatching one branch
        doesn't drag the other along.
        """
        device = self._devices.get(job.device_id)
        if device is None:
            job.set_error(f"device {job.device_id} disappeared between selection and write")
            job.transition(FlashJobState.FAILED)
            return

        # UF2 is USB-only; if we got here with a non-serial transport, the
        # transport-selector code in flash_job.select_transport has a bug
        # and we want it loud, not a silent fall-back to a stale
        # ``device.port`` attribute. ``Transport.address`` is on the
        # abstract base class — every concrete transport has it — so a
        # defensive ``hasattr()`` check on top of this guard would just
        # mask the bug behind a silent substitution if the invariant
        # ever broke.
        if device.transport.transport_type != "serial":
            job.set_error(
                f"UF2 flash requires a serial transport, got "
                f"{device.transport.transport_type!r} (transport selector bug)"
            )
            job.transition(FlashJobState.FAILED)
            return

        # Build the SignalHistory + _FleetVerifySignals BEFORE the flash so
        # that the verify-signals snapshot of the device's USB devnum is
        # the PRE-flash baseline. The flash workflow includes a USB
        # reset and a BL → app reboot, both of which renumber the
        # device; if signals were constructed post-flash, the baseline
        # would already reflect the post-reboot devnum and the verify
        # state machine would stall in AWAITING_REBOOT forever (the
        # 0.8.0-4 BL doesn't help — even when the device IS happily
        # booted, the verify can't tell because the devnum it remembers
        # IS the post-boot one). Verified live 2026-05-18 against FPS
        # Sweep — pre-fix: stuck in AWAITING_REBOOT for 5 min; post-fix:
        # detects re-enum immediately after BL exit. `write_completed_at`
        # is filled in after _uf2_write_impl_for_job lands so the timestamp
        # corresponds to bytes-on-flash rather than to verify-start.
        history = SignalHistory(
            write_completed_at=_time.time(),  # provisional, updated below
            expected_version=job.expected_version,
            # `previous_version` is what the device reported at discovery
            # handshake (`json info` populates `device.version`). The
            # `detect_stale_firmware` detector compares the post-flash
            # version against this — when the new firmware boots but
            # `json info` still reports the old build string, that's the
            # "BL accepted UF2 but didn't update the valid-app marker"
            # failure mode we hit on 2026-05-17 with bootloader 0.8.0-4.
            # Without this wiring the field stays None and the detector
            # silently never fires.
            previous_version=device.version,
        )
        signals = _FleetVerifySignals(fleet=self, device_id=job.device_id, history=history)

        # WRITING: subprocess + progress callbacks.
        job.transition(FlashJobState.WRITING)
        write_ok = await _uf2_write_impl_for_job(
            job,
            serial_port=device.transport.address,
            firmware_path=str(job.firmware_path),
            transport=device.transport,
        )
        if not write_ok:
            # ``_uf2_write_impl_for_job`` already set ``job.error``.
            job.transition(FlashJobState.FAILED)
            return

        # VERIFYING: progressive sub-states + anomaly detection.
        # Sync write_completed_at into the history now that we have it.
        history.write_completed_at = job.write_completed_at or _time.time()
        job.transition(FlashJobState.VERIFYING)
        # Wrap run_verify in a soft 5-minute total cap. The verify state
        # machine itself never fails on wall-clock; this is the
        # orchestrator's safety net for "operator isn't watching, give
        # up eventually." Per design, the failure surfaces as anomalies
        # in the job, not as a hard FAILED unless we genuinely have no
        # other signal.
        try:
            await asyncio.wait_for(
                self._verify_with_anomaly_checks(job, signals, history),
                timeout=300.0,
            )
            job.transition(FlashJobState.COMPLETED)
        except TimeoutError:
            # Verify never converged. Surface as FAILED with the anomaly
            # set the detectors built up.
            check_anomalies(job, history)
            job.set_error("verify did not converge within 5 minutes")
            job.transition(FlashJobState.FAILED)

    async def _run_ble_dfu_flash(self, job: FlashJob) -> None:
        """BLE-DFU branch of ``_run_flash_job``.

        Three reachability cases, distinguished by ``device.state``:

        * ``DFU_RECOVERY`` — the device is already in its bootloader
          advertising the AdaDFU service. Skip bootloader entry; go
          straight to the DFU transfer.

        * App-mode over **BLE** — device is healthy and we're talking
          over NUS. Issue ``bootloader ble`` via NUS, give the bootloader
          ~3 s to come up + start advertising, then DFU.

        * App-mode over **serial** — device is healthy and we're talking
          over USB-CDC. Issue ``bootloader ble`` via the serial console,
          ~2 s wait. (Only reachable if ``select_transport`` chose
          BLE_DFU even with USB present — which it doesn't today since
          USB > BLE in the policy. Kept for completeness / future routes.)

        Protections wrapped around the call (per FLASH_LOCKDOWN_AUDIT.md):
        ``pause_discovery`` / ``hold_reconnect`` / ``broadcaster.stop()``
        + verification guard + restart in ``finally``. BCM43455's single
        radio cannot register an advertisement while we hold the GATT
        central channel that ``_ble_dfu_write_impl`` needs — flashing
        under radio contention is what bricked cart_inner on 2026-05-16,
        so the broadcaster check is fail-loud (refuses to enter DFU if
        the broadcaster didn't stop cleanly).

        The ``_ble_dfu_write_impl`` call retains its built-in post-DFU
        verify (scan + NUS+RSSI fallback for random-static-address-
        change). The plan is to migrate that into a BLE-aware
        ``run_verify`` branch later; for now, success of the impl's
        verify maps directly to ``FlashJobState.COMPLETED``.
        """
        from ..firmware.compile import ensure_dfu_zip

        device = self._devices.get(job.device_id)
        if device is None:
            job.set_error(f"device {job.device_id} disappeared between selection and write")
            job.transition(FlashJobState.FAILED)
            return

        if not device.ble_address:
            job.set_error(
                f"BLE-DFU flash requires a known BLE address, but device "
                f"{job.device_id[:12]} has none"
            )
            job.transition(FlashJobState.FAILED)
            return

        # Determine bootloader-entry mechanism. None means "already in
        # bootloader" — the DFU_RECOVERY fast path.
        enter_via_serial = None
        enter_via_ble = None
        if device.state is not DeviceState.DFU_RECOVERY:
            if device.transport.transport_type == "serial":
                # Bind the protocol/transport at closure-build time so a
                # late device-table mutation can't redirect the
                # bootloader-entry command to a different device.
                _protocol = device.protocol

                async def _enter_via_serial(cmd: str) -> None:
                    await _protocol.send_command(cmd)
                    await asyncio.sleep(2)

                enter_via_serial = _enter_via_serial
            elif device.transport.transport_type == "ble":
                # PRESENT BLE devices don't hold a persistent GATT
                # connection; we bring up NUS just-in-time to issue the
                # command, then drop it before _ble_dfu_write_impl opens
                # its own connection to the bootloader address. Idempotent
                # connect (no-op if already connected) handles the rare
                # path where something previously left a CONNECTED
                # transport in place.
                _ble_transport = device.transport

                async def _enter_via_ble(cmd: str) -> None:
                    if not _ble_transport.is_connected:
                        await _ble_transport.connect()
                    try:
                        await _ble_transport.write_line(cmd)
                        await asyncio.sleep(0.5)
                    finally:
                        # Always drop the connection — a leaked GATT
                        # channel blocks the BCM43455 from re-registering
                        # for the bootloader connection that
                        # _ble_dfu_write_impl is about to make.
                        with contextlib.suppress(Exception):
                            await _ble_transport.disconnect()

                enter_via_ble = _enter_via_ble
            else:
                job.set_error(
                    f"BLE-DFU flash needs serial/ble transport for bootloader "
                    f"entry, got {device.transport.transport_type!r}"
                )
                job.transition(FlashJobState.FAILED)
                return

        # ── Radio-contention prevention ──────────────────────────────────
        # BCM43455 single-radio constraint: can't simultaneously
        # advertise (broadcaster) and act as GATT central (DFU client).
        # Stop the broadcaster before we touch BLE; refuse to proceed if
        # it didn't actually stop. This mirrors the legacy fleet-flash
        # and auto-recovery branches. Documented in
        # `docs/FLASH_LOCKDOWN_AUDIT.md`.
        broadcaster_was_running = self.broadcaster is not None and self.broadcaster.is_running
        if broadcaster_was_running and self.broadcaster is not None:
            try:
                await self.broadcaster.stop()
            except Exception:
                log.exception(
                    "flash job %s: broadcaster.stop() raised; refusing to "
                    "enter BLE DFU under uncertain radio state",
                    job.job_id,
                )
            if self.broadcaster is not None and self.broadcaster.is_running:
                job.set_error(
                    "broadcaster did not stop cleanly — refusing to enter "
                    "BLE DFU under radio contention"
                )
                job.transition(FlashJobState.FAILED)
                return

        self.pause_discovery()
        # Long hold — BLE DFU at MTU=20 runs ~5-8 min on a 540 KB image,
        # plus retries can extend that. 600 s matches the legacy
        # auto-recovery / route values.
        self.hold_reconnect(job.device_id, 600)

        try:
            # Build the DFU zip on a worker thread — adafruit-nrfutil
            # genpkg shells out and we don't want it blocking the loop.
            try:
                dfu_zip = await asyncio.to_thread(ensure_dfu_zip, str(job.firmware_path))
            except Exception as exc:
                job.set_error(f"DFU zip build failed: {exc}")
                job.transition(FlashJobState.FAILED)
                return

            job.transition(FlashJobState.WRITING)

            # Snapshot the firmware size up-front so the progress
            # callback can convert ``_ble_dfu_write_impl``'s percent
            # progress into an absolute bytes_written / bytes_total
            # on the FlashJob (per PR #143 Gemini #9). The impl yields
            # pct via its existing ``progress_callback`` signature
            # (phase, msg, pct); we ignore phase/msg and synthesize
            # bytes from pct. If the firmware file is unreadable
            # (rare, would have already failed in zip-build above),
            # fall back to a 0-byte total so the callback no-ops
            # rather than crashing.
            try:
                _fw_size = job.firmware_path.stat().st_size
            except OSError:
                _fw_size = 0

            def _ble_progress(_phase: str, _msg: str, pct: int | None = None) -> None:
                if pct is None or _fw_size <= 0:
                    return
                # Clamp to 0..100 — the impl's existing pct mapping
                # spans phases (35..99) so values can briefly hit
                # 99 then drop on a retry. Don't let bytes_written
                # go above bytes_total.
                clamped = max(0, min(100, pct))
                bytes_written = (_fw_size * clamped) // 100
                try:
                    job.record_progress(bytes_written, _fw_size)
                except Exception as exc:
                    # Progress reporting failure shouldn't derail the
                    # flash; log once at DEBUG and continue.
                    log.debug(
                        "_run_ble_dfu_flash %s: record_progress raised %s; "
                        "continuing without progress updates",
                        job.job_id,
                        exc,
                    )

            try:
                # Call via the module attribute (not a from-imported name)
                # so ``monkeypatch.setattr(ble_dfu_mod, "_ble_dfu_write_impl",
                # ...)`` in tests reaches this call site. A direct
                # ``from .ble_dfu import _ble_dfu_write_impl`` at module top
                # would capture the original function reference at import
                # time, bypassing later patches and silently running real
                # BLE code under test.
                result = await asyncio.wait_for(
                    _ble_dfu_mod._ble_dfu_write_impl(
                        app_ble_address=device.ble_address,
                        dfu_zip_path=dfu_zip,
                        enter_bootloader_via_serial=enter_via_serial,
                        enter_bootloader_via_ble=enter_via_ble,
                        progress_callback=_ble_progress,
                    ),
                    # 600 s mirrors the legacy fleet-flash + auto-recovery
                    # wall-clock.
                    timeout=600.0,
                )
            except TimeoutError:
                job.set_error("BLE DFU exceeded 600s wall-clock timeout")
                job.transition(FlashJobState.FAILED)
                return

            if result.get("status") != "ok":
                job.set_error(f"BLE DFU failed: {result.get('message', 'unknown error')}")
                job.transition(FlashJobState.FAILED)
                return

            # ``_ble_dfu_write_impl`` returned ok — its own post-DFU
            # verify already scanned for the rebooted device (with the
            # random-static-address-change fallback). Map directly to
            # COMPLETED. The lockdown plan calls for migrating this
            # verify into a BLE-aware ``run_verify`` branch later;
            # until that lands, the legacy verify is what we trust.
            job.transition(FlashJobState.VERIFYING)
            job.transition(FlashJobState.COMPLETED)
        finally:
            self.resume_discovery()
            self.resume_reconnect(job.device_id)
            if broadcaster_was_running and self.broadcaster is not None:
                try:
                    await self.broadcaster.start()
                except Exception:
                    log.exception(
                        "flash job %s: failed to restart broadcaster after "
                        "BLE DFU — fleet commands will not reach BLE devices "
                        "until manual restart",
                        job.job_id,
                    )

    async def _verify_with_anomaly_checks(
        self,
        job: FlashJob,
        signals: _FleetVerifySignals,
        history: SignalHistory,
    ) -> None:
        """Run ``run_verify`` concurrently with a periodic anomaly sweep.

        The anomaly sweep just calls ``check_anomalies(job, history)``
        every couple of seconds. The history is populated by the signals
        wrapper, which appends timestamps as it observes events.
        """

        async def _anomaly_loop() -> None:
            while True:
                await asyncio.sleep(2.0)
                check_anomalies(job, history)

        # signals.history is wired in via the constructor (see
        # _FleetVerifySignals docstring); no need to assign here.
        anomaly_task = asyncio.create_task(_anomaly_loop(), name=f"anomaly-{job.job_id}")
        try:
            await run_verify(job, signals)
        finally:
            anomaly_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await anomaly_task

    def _build_transport_probe(self, device_id: str) -> TransportProbe:
        """Snapshot which interfaces reach this device *right now*.

        Three signals:
          - ``has_usb_app``: true iff the device has a CONNECTED serial
            transport (app-mode CDC handshake is responsive).
          - ``has_ble_dfu_advert``: true iff the device is in
            ``DFU_RECOVERY`` state (the discovery loop sets this when
            it sees an ``AdaDFU`` BLE advertisement at the bootloader
            address — see ``transport/discovery.py`` and the
            ``_promote_dfu_advert_to_device`` logic in this module).
            DFU_RECOVERY implies the device is parked in its bootloader
            with the OTA-DFU service exposed.
          - ``has_ble_app``: true iff the device is in PRESENT state
            with a BLE transport and a known address (NUS-reachable
            for the ``bootloader ble`` command). The orchestrator's
            ``_run_ble_dfu_flash`` issues that command over NUS to send
            the device into the bootloader's AdaDFU service before the
            DFU transfer.
        """
        device = self._devices.get(device_id)
        has_usb_app = (
            device is not None
            and device.transport.transport_type == "serial"
            and device.transport.is_connected
        )
        has_ble_dfu_advert = (
            device is not None
            and device.state is DeviceState.DFU_RECOVERY
            and bool(device.ble_address)
        )
        has_ble_app = (
            device is not None
            and device.transport.transport_type == "ble"
            and device.state is DeviceState.PRESENT
            and bool(device.ble_address)
        )
        return TransportProbe(
            has_usb_app=has_usb_app,
            has_ble_dfu_advert=has_ble_dfu_advert,
            has_ble_app=has_ble_app,
        )

    async def start(self) -> None:
        """Start background loop for device discovery and management.

        Serial device connections happen in the background loop. BLE
        devices do NOT hold persistent connections — fleet commands go
        out via :attr:`broadcaster` (manufacturer-data advertising). See
        :class:`FleetBroadcaster` for the rationale.
        """
        self._running = True
        # Load persisted device-identity aliases. Restoring them BEFORE
        # the recovery-firmware whitelist is loaded matters because the
        # whitelist match in ``_auto_recover_dfu_devices`` uses
        # canonical-resolved keys: a SN-only whitelist needs the
        # SN ↔ BLE-addr alias to match a DFU-state device's BLE form.
        # Without this, the canonical resolver is a no-op at startup
        # and the whitelist match works only after discovery has seen
        # the device on both transports (a window of 1-2 cycles).
        self._load_identity_aliases()
        # Arm DFU auto-recovery from persisted state if a previous deploy
        # left a firmware path AND an explicit device whitelist. Critical
        # for unattended boots: a device on the whitelist stuck in DFU
        # bootloader at power-on recovers itself without operator
        # intervention. Devices not on the whitelist are left alone.
        persisted = self._load_recovery_firmware()
        if persisted:
            firmware_path, device_ids = persisted
            self._recovery_firmware_path = firmware_path
            self._recovery_device_ids = device_ids
            log.info(
                "DFU auto-recovery armed from persisted state: %s (devices: %s)",
                firmware_path,
                ", ".join(sorted(device_ids)) or "<none>",
            )
        # L4: rebuild the dedup window from the persistent audit log so
        # a server restart doesn't erase "I just flashed this device" —
        # otherwise auto-recovery's first cycle after restart could
        # re-fire on a device that just failed pre-restart. Reads only
        # the entries inside FLASH_DEDUP_WINDOW_S of now; older entries
        # are skipped.
        self._load_recent_flash_attempts_from_audit_log()
        # Bring up the BLE broadcaster. Single-radio adapters (BCM43455 on
        # Pi 4) refuse advertising while we hold GATT central connections,
        # so the broadcaster MUST come up before any per-device connect
        # attempt — and we MUST NOT auto-connect to BLE devices during
        # normal operation.
        if self._enable_ble:
            try:
                self.broadcaster = FleetBroadcaster()
                await self.broadcaster.start()
            except Exception:
                log.exception(
                    "Fleet broadcaster failed to start — fleet commands will not be delivered over BLE"
                )
                self.broadcaster = None
        # Start the independent watchdog pinger FIRST so it begins pinging
        # before anything else can stall.
        self._watchdog_task = asyncio.create_task(self._watchdog_pinger())
        self._discovery_task = asyncio.create_task(self._background_loop())
        log.info("Fleet manager started")

    async def stop(self) -> None:
        """Disconnect all devices and stop background tasks."""
        self._running = False
        if self._watchdog_task:
            self._watchdog_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._watchdog_task
            self._watchdog_task = None
        if self._discovery_task:
            self._discovery_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._discovery_task
        for device in self._devices.values():
            await device.disconnect()
        self._devices.clear()
        self._device_discovery.clear()
        if self.broadcaster is not None:
            with contextlib.suppress(Exception):
                await self.broadcaster.stop()
            self.broadcaster = None
        log.info("Fleet manager stopped")

    def hold_reconnect(self, device_id: str, seconds: float) -> None:
        """Prevent auto-reconnect for a device for the given duration."""
        self._reconnect_blackout[device_id] = _time.monotonic() + seconds

    def resume_reconnect(self, device_id: str) -> None:
        """Clear reconnect blackout for a device, allowing auto-reconnect."""
        self._reconnect_blackout.pop(device_id, None)

    def pause_discovery(self) -> None:
        """Pause background discovery (e.g., during BLE DFU scan)."""
        self._discovery_pause_count += 1
        log.info("Discovery paused (depth=%d)", self._discovery_pause_count)

    def resume_discovery(self) -> None:
        """Resume background discovery after pause."""
        self._discovery_pause_count = max(0, self._discovery_pause_count - 1)
        log.info("Discovery resumed (depth=%d)", self._discovery_pause_count)

    async def release_device(self, device_id: str, hold_seconds: int | None = None) -> bool:
        """Temporarily release a device (e.g., for firmware flashing).

        If hold_seconds is set, the device won't be auto-reconnected for
        that duration (in-memory blackout).
        """
        device = self.get_device(device_id)
        if not device:
            return False
        await device.disconnect()
        if hold_seconds is not None:
            # Use the full device ID (not the potentially-shortened API param)
            # so the blackout check in _reconnect_disconnected matches.
            self.hold_reconnect(device.id, hold_seconds)
        log.info("Released device %s on %s (hold=%ss)", device.id[:12], device.port, hold_seconds)
        return True

    async def remove_device(self, device_id: str) -> None:
        """Permanently remove a device from the fleet.

        Disconnects the device, removes it from all tracking structures
        (including DFU state and locks), and adds any BLE address to
        the dedup exclusion set to prevent immediate rediscovery. The
        device will be re-added if it reappears on USB (serial devices
        are always re-discovered).

        Safe to call while the background loop is running — the
        _devices.pop() is atomic (no await between read and delete).
        Subsequent cleanup pops are on secondary dicts and tolerate
        the device already being absent.
        """
        device = self._devices.pop(device_id, None)
        if device:
            with contextlib.suppress(Exception):
                await device.disconnect()
            # Exclude BLE address from rediscovery (serial devices
            # are re-discovered by USB serial number regardless)
            if device.transport.transport_type == "ble":
                self._dedup_excluded.add(device_id)
        self._device_discovery.pop(device_id, None)
        self._reconnect_failures.pop(device_id, None)
        self._reconnect_blackout.pop(device_id, None)
        self._backoff_skip.pop(device_id, None)
        # L3c: drop the device's retry state too. Keyed by canonical
        # because `_run_flash_job`'s finally writes it under canonical.
        self._recovery_retry_state.pop(self.resolve_canonical(device_id), None)
        log.info("Removed device %s from fleet", device_id[:12])

    async def reconnect_device(self, device_id: str) -> bool:
        """Reconnect a previously released device."""
        device = self.get_device(device_id)
        if not device:
            return False
        if device.state == DeviceState.CONNECTED:
            return True
        # Use full device ID for internal lookups
        full_id = device.id
        self._reconnect_failures.pop(full_id, None)  # Reset backoff
        self._reconnect_blackout.pop(full_id, None)  # Clear hold

        disc = self._device_discovery.get(full_id)
        if not disc:
            log.error("No discovery info for device %s", full_id[:12])
            return False

        try:
            transport = _create_transport(disc)
            device.transport = transport
            device.protocol = DeviceProtocol(transport)
            device.protocol.on_stream_line(device._route_stream_line)
            device.protocol.on_raw_line(device._on_raw_line)
            await device.connect()
            return True
        except Exception as e:
            log.error("Reconnect failed for %s: %s", device_id[:12], e)
            return False

    async def send_to_all(self, command: str) -> dict[str, str]:
        """Send a command to all known devices and return per-device status.

        Every known device appears in the result dict. Devices not in
        CONNECTED state get a `"skipped: state=<state>"` entry rather than
        being silently filtered out — callers (deploy.sh, validation jobs)
        rely on full per-device visibility to fail loud when a fleet-wide
        command misses some subset of devices. Pre-2026-05-01 the filtered
        devices were silently dropped, which made deploy.sh's
        "Done" line a lie when one device was re-enumerating during the call.
        """
        results: dict[str, str] = {}
        tasks: list[tuple[str, asyncio.Task[str]]] = []
        for device in self._devices.values():
            if device.state == DeviceState.CONNECTED:
                task = asyncio.create_task(device.protocol.send_command(command))
                tasks.append((device.id, task))
            else:
                results[device.id] = f"skipped: state={device.state.value}"
        for device_id, task in tasks:
            try:
                results[device_id] = await task
            except Exception as e:
                results[device_id] = f"error: {e}"
        return results

    async def discover_now(self) -> None:
        """Run discovery + reconnect immediately (called by API endpoint).

        Debounced: refuses if last discovery was < 5s ago to prevent
        overlapping BleakScanner scans from rapid API calls.
        """
        now = _time.monotonic()
        last = getattr(self, "_last_discovery_time", 0.0)
        if now - last < 5.0:
            log.debug("discover_now() debounced (%.1fs since last)", now - last)
            return
        self._last_discovery_time = now
        await self._discover_and_connect()
        await self._reconnect_disconnected()

    def add_wifi_host(self, host: str, port: int = 3333, device_id: str | None = None) -> None:
        """Add a WiFi device to the discovery list at runtime."""
        entry = {"host": host, "port": port}
        if device_id:
            entry["device_id"] = device_id
        self._wifi_hosts.append(entry)

    # ── Internal ──

    async def _discover_and_connect(self) -> None:
        """Discover devices across transports.

        Serial devices are connected and held (high-bandwidth telemetry use
        cases still require a persistent link). BLE devices are NOT
        connected — they're tracked as "present" entries via passive scan
        only. Commands to BLE devices go out via the broadcaster, never
        per-connection. See class docstring for the architectural rationale.
        """
        self._refresh_dedup_exclusions()
        discovered = await discover_all(
            serial_scan=self._enable_serial,
            ble_scan=self._enable_ble,
            ble_timeout=self._ble_timeout,
            wifi_hosts=self._wifi_hosts,
        )
        known_ids = set(self._devices.keys())

        for disc in discovered:
            device_id = disc.device_id

            # Handle devices stuck in BLE DFU bootloader (SafeBoot crash recovery).
            # These can't be connected normally — surface their state for the operator.
            if disc.transport_type == "ble_dfu":
                self._handle_dfu_recovery_device(disc)
                continue

            # BLE app-mode devices: track presence, do NOT connect. The BCM43455
            # adapter can't advertise (broadcast channel) while we hold central
            # GATT connections; broadcaster takes priority. We still create
            # a Device entry so /api/devices, flash routes, and the rest of
            # the management surface have something to point at — but the
            # BleTransport stays unconnected unless a per-device operation
            # (flash, etc.) brings it up just-in-time.
            if disc.transport_type == "ble":
                if device_id in known_ids:
                    existing = self._devices[device_id]
                    existing.last_seen = _time.monotonic()
                    if disc.rssi is not None:
                        existing.rssi = disc.rssi
                    if disc.description and not existing.device_name:
                        existing.device_name = disc.description
                    # The device is advertising NUS again, so it's back in
                    # app mode. Reset transient post-op states (after a
                    # flash leaves state=DISCONNECTED; after a previous
                    # DFU_RECOVERY where the device has now recovered).
                    # PRESENT is the steady-state for any BLE peer we can
                    # see; flashing again needs the state check to pass.
                    if existing.state in (
                        DeviceState.DISCONNECTED,
                        DeviceState.ERROR,
                        DeviceState.DFU_RECOVERY,
                    ):
                        existing.state = DeviceState.PRESENT
                    continue
                if device_id in self._dedup_excluded:
                    continue
                try:
                    transport = _create_transport(disc)
                    device = Device(
                        device_id=device_id,
                        port=disc.address,
                        platform=disc.platform,
                        transport=transport,
                    )
                    device.state = DeviceState.PRESENT
                    device.last_seen = _time.monotonic()
                    device.device_name = disc.description
                    if disc.rssi is not None:
                        device.rssi = disc.rssi
                    if disc.address:
                        device.ble_address = disc.address
                    self._devices[device_id] = device
                    self._device_discovery[device_id] = disc
                    log.info(
                        "BLE device present: %s (%s, RSSI=%s)",
                        device_id,
                        disc.description or "unnamed",
                        disc.rssi,
                    )
                except Exception:
                    log.exception("Failed to track BLE device %s", device_id[:12])
                continue

            if device_id in known_ids:
                existing = self._devices[device_id]
                # Update address if it changed (USB re-enumeration)
                if existing.port != disc.address:
                    log.info(
                        "Device %s address changed: %s → %s",
                        device_id[:12],
                        existing.port,
                        disc.address,
                    )
                    existing.port = disc.address
                # The device is on the USB bus again. Reset transient
                # post-flash states so the reconnect loop picks it up
                # next cycle (DFU_RECOVERY: returned from BLE-DFU
                # bootloader; DISCONNECTED/ERROR: prior connect dropped).
                # Mirror the BLE branch's analogous transition above.
                # Without this, a device that recovered from BLE-DFU
                # stays in dfu_recovery state forever even though it's
                # back on serial — exactly the bug observed live after
                # PR #143's BLE-DFU hardware test.
                if existing.state in (
                    DeviceState.DFU_RECOVERY,
                    DeviceState.DISCONNECTED,
                    DeviceState.ERROR,
                ):
                    prior_state = existing.state.value
                    # Rebuild the SerialTransport against the freshly-
                    # discovered port — the old transport might be bound
                    # to a stale device path if USB renumbered during
                    # the DFU cycle. ``_reconnect_disconnected`` reads
                    # ``device.transport`` on its next pass to call
                    # ``connect()``, so the transport must be current.
                    try:
                        existing.transport = _create_transport(disc)
                    except Exception:
                        log.exception(
                            "Failed to rebuild transport for %s on re-discovery",
                            device_id[:12],
                        )
                    existing.state = DeviceState.DISCONNECTED
                    # Refresh the cached discovery snapshot so the
                    # reconnect loop sees the current transport_type
                    # (was 'ble_dfu' if we'd promoted to DFU recovery).
                    self._device_discovery[device_id] = disc
                    # Reset the reconnect-backoff counter so the first
                    # post-recovery reconnect attempt fires immediately
                    # rather than waiting through whatever backoff the
                    # prior failures accumulated.
                    self._reconnect_failures.pop(device_id, None)
                    self._reconnect_blackout.pop(device_id, None)
                    log.info(
                        "Device %s re-appeared on serial after %s; "
                        "transitioning to DISCONNECTED for reconnect",
                        device_id[:12],
                        prior_state,
                    )
                continue

            # Skip devices previously deduped (e.g. shared BLE+serial identity).
            if device_id in self._dedup_excluded:
                continue

            # New serial device — connect.
            try:
                transport = _create_transport(disc)
                device = Device(
                    device_id=device_id,
                    port=disc.address,
                    platform=disc.platform,
                    transport=transport,
                )
                if disc.rssi is not None:
                    device.rssi = disc.rssi
                self._devices[device_id] = device
                self._device_discovery[device_id] = disc
                await device.connect()
            except Exception as e:
                log.error(
                    "Failed to connect %s %s on %s: %s",
                    disc.transport_type,
                    device_id[:12],
                    disc.address,
                    e,
                )

        # Deduplicate: if a device is connected via both serial and BLE/WiFi,
        # prefer serial (faster, more reliable) and disconnect the wireless one.
        await self._deduplicate_transports()

    async def broadcast(self, command: str) -> dict[str, str]:
        """Send a command to every device in range.

        Two channels in parallel:
          1. BLE broadcaster — fires off a manufacturer-data advertisement
             that all listening blinky devices decode. No per-device state.
          2. Serial devices — write the same command line down each USB
             serial transport (the small set we still hold persistent
             connections for).

        Returns a per-source result map for the UI:
          - "broadcast": status of the BLE advert
          - "<serial-id>": per-serial-device response or skipped: state=…
        """
        results: dict[str, str] = {}

        # Run BLE broadcast (3s on-air hold) concurrently with serial
        # fan-out (sub-100ms each). Previously the serial devices waited
        # for the broadcaster's full 3-second window before getting their
        # command — pointless serialization. PR #140 review.
        async def _do_broadcast() -> None:
            if self.broadcaster is not None and self.broadcaster.is_running:
                try:
                    await self.broadcaster.broadcast_command(command)
                    results["broadcast"] = "OK"
                except Exception as exc:
                    log.exception("Broadcast %r failed", command)
                    results["broadcast"] = f"error: {exc}"
            elif self._enable_ble:
                results["broadcast"] = "error: broadcaster not running"

        async def _do_serial_fanout() -> None:
            # Fan out to any non-BLE devices we still hold a connection to —
            # serial (cart-side devices), test mocks, future transports.
            # BLE is broadcaster-only (no per-device connection state to
            # message).
            ids: list[str] = []
            tasks: list[asyncio.Task[str]] = []
            for device_id, device in list(self._devices.items()):
                if device.transport.transport_type == "ble":
                    continue
                if device.state != DeviceState.CONNECTED:
                    results[device_id] = f"skipped: state={device.state.value}"
                    continue
                ids.append(device_id)
                tasks.append(asyncio.create_task(device.protocol.send_command(command)))
            # Gather concurrently with return_exceptions so a single slow
            # or failing device doesn't block collecting results for the
            # others (PR #140 review — previously awaited sequentially,
            # which serialized failure timeouts).
            if tasks:
                outcomes = await asyncio.gather(*tasks, return_exceptions=True)
                for device_id, outcome in zip(ids, outcomes, strict=True):
                    if isinstance(outcome, BaseException):
                        results[device_id] = f"error: {outcome}"
                    else:
                        results[device_id] = outcome

        await asyncio.gather(_do_broadcast(), _do_serial_fanout())
        return results

    async def _deduplicate_transports(self) -> None:
        """Remove duplicate connections to the same physical device.

        If a device is connected via serial AND BLE/WiFi, keep the serial
        connection (faster, more reliable) and disconnect the wireless one.
        Detection: hardware_sn ONLY (unique per chip). The previous
        device_type:device_name fallback was removed because it false-positives
        on different physical devices with the same configuration.
        When removing a wireless duplicate, preserve its BLE address on the
        kept serial device (enables BLE DFU fallback), and add the wireless
        address to the exclusion set to prevent rediscovery thrashing.
        """
        # Build a map of device identity → (device_id, transport_type)
        identity_map: dict[str, list[tuple[str, str]]] = {}
        for did, dev in self._devices.items():
            if dev.state != DeviceState.CONNECTED:
                continue
            # Only dedup by hardware_sn (unique per chip).
            # device_type:device_name causes false positives when multiple
            # physical devices share the same config.
            if dev.hardware_sn:
                identity = f"sn:{dev.hardware_sn}"
            else:
                continue
            entry = (did, dev.transport.transport_type)
            identity_map.setdefault(identity, []).append(entry)

        # Find duplicates and remove wireless ones
        to_remove: list[str] = []
        for identity, entries in identity_map.items():
            if len(entries) <= 1:
                continue
            serial_entries = [e for e in entries if e[1] == "serial"]
            wireless_entries = [e for e in entries if e[1] != "serial"]
            if serial_entries and wireless_entries:
                serial_did = serial_entries[0][0]
                serial_dev = self._devices[serial_did]
                for did, ttype in wireless_entries:
                    wireless_dev = self._devices[did]
                    # Preserve BLE address on the serial device for DFU fallback
                    if not serial_dev.ble_address and wireless_dev.ble_address:
                        serial_dev.ble_address = wireless_dev.ble_address
                        log.info(
                            "Dedup: preserved BLE address %s on serial device %s",
                            wireless_dev.ble_address,
                            serial_did[:12],
                        )
                    # PR #143 Copilot #2: register the SN ↔ BLE alias
                    # NOW (the canonical-resolver was a no-op in production
                    # before this — `register_identity_alias` had no
                    # caller outside tests). Wiring it here means the
                    # auto-recovery whitelist match works across
                    # transports: an SN-only whitelist matches a
                    # DFU-recovery device that's advertising under
                    # its BLE bootloader address (app_addr ± 1), and
                    # vice versa. The bootloader address (app_addr + 1
                    # on Nordic) is also added so a DFU-mode discovery
                    # under the bootloader form resolves correctly.
                    aliases: list[str] = [serial_did]
                    if wireless_dev.ble_address:
                        aliases.append(wireless_dev.ble_address)
                        # Bootloader addr = app_addr ± 1 (last octet).
                        # We don't know which sign Nordic chose for this
                        # particular device, so register both and let
                        # the alias map cover the bootloader form too.
                        try:
                            parts = wireless_dev.ble_address.split(":")
                            last = int(parts[-1], 16)
                            for offset in (1, -1):
                                octet = (last + offset) & 0xFF
                                parts_alt = [*parts[:-1], f"{octet:02X}"]
                                aliases.append(":".join(parts_alt))
                        except (ValueError, IndexError):
                            pass
                    canonical = self.register_identity_alias(*aliases)
                    self._persist_identity_aliases()
                    log.info(
                        "Dedup: aliased %s to canonical %s (covers %d forms)",
                        identity,
                        canonical[:12],
                        len(aliases),
                    )
                    log.info(
                        "Dedup: %s (%s) is duplicate of serial device, disconnecting %s",
                        identity,
                        did[:12],
                        ttype,
                    )
                    to_remove.append(did)

        for did in to_remove:
            removed_dev = self._devices.get(did)
            if removed_dev:
                await removed_dev.disconnect()
                # Add to exclusion set so discovery doesn't re-add it.
                # This prevents the thrashing loop where a deduped BLE device
                # is immediately rediscovered and reconnected every cycle.
                self._dedup_excluded.add(did)
                del self._devices[did]
                self._device_discovery.pop(did, None)

    def _handle_dfu_recovery_device(self, disc: DiscoveredDevice) -> None:
        """Handle a device discovered in BLE DFU bootloader mode.

        Matches it to an existing device by BLE address (bootloader addr = app
        addr + 1) and sets its state to DFU_RECOVERY. If no match is found,
        creates a placeholder device entry so it appears in the API.

        Uses the app-mode address as device_id so the same physical device
        keeps a stable identity after recovery.
        """
        app_addr = disc.extra.get("app_address", "")
        boot_addr = disc.address

        # Try to match to an existing device by BLE address
        matched_dev = None
        for dev in self._devices.values():
            if app_addr and dev.ble_address and dev.ble_address.upper() == app_addr.upper():
                matched_dev = dev
                break

        if matched_dev is not None:
            if matched_dev.state != DeviceState.DFU_RECOVERY:
                log.warning(
                    "Device %s (%s) is in DFU bootloader — SafeBoot crash recovery. "
                    "Push firmware via POST /api/devices/%s/flash to recover.",
                    matched_dev.id[:12],
                    matched_dev.device_name or "unknown",
                    matched_dev.id,
                )
                matched_dev.state = DeviceState.DFU_RECOVERY
        else:
            # No match — create a placeholder entry using app_addr as stable ID
            placeholder_id = disc.device_id  # app_addr (stable across boot/app mode)
            if placeholder_id not in self._devices:
                from ..transport.ble_transport import BleTransport

                placeholder = Device(
                    device_id=placeholder_id,
                    port=boot_addr,
                    platform="nrf52840",
                    transport=BleTransport(boot_addr),
                )
                placeholder.state = DeviceState.DFU_RECOVERY
                placeholder.ble_address = app_addr
                self._devices[placeholder_id] = placeholder
                self._device_discovery[placeholder_id] = disc
                log.warning(
                    "Unknown device in DFU bootloader at %s (app addr: %s). "
                    "Push firmware to recover.",
                    boot_addr,
                    app_addr,
                )

    def _refresh_dedup_exclusions(self) -> None:
        """Clear dedup exclusions only for BLE devices whose serial counterpart
        has been REMOVED from the fleet entirely (not just disconnected).

        Previously cleared all exclusions when any serial device disconnected,
        causing a thrashing loop where BLE devices were rediscovered, connected,
        deduped, and disconnected every discovery cycle.

        Now only clears exclusions when the serial device no longer exists in
        self._devices at all — meaning it was removed (failure limit exceeded
        or manually released), not just temporarily disconnected during flash.
        """
        if not self._dedup_excluded:
            return
        # Only clear exclusions if NO serial devices exist at all.
        # Individual serial disconnects (e.g., during flash) should NOT
        # clear exclusions — the serial device will reconnect.
        has_any_serial = any(
            d.transport and d.transport.transport_type == "serial" for d in self._devices.values()
        )
        if not has_any_serial:
            log.info(
                "No serial devices in fleet — clearing %d dedup exclusions",
                len(self._dedup_excluded),
            )
            self._dedup_excluded.clear()

    async def _reconnect_disconnected(self) -> None:
        """Try to reconnect any devices in error/disconnected state.

        Uses exponential backoff: after each failure, waits longer before
        retrying (10s, 20s, 40s, 80s, 160s, capped at 5 min). Resets on
        successful connect or when device is released/reconnected manually.
        After 20 consecutive failures, removes the device from the fleet.
        """
        to_remove_after: list[str] = []

        # Cache serial discovery once for all reconnect attempts (avoid per-device USB scan)
        from ..transport.discovery import discover_serial_devices

        fresh_serial_ports: dict[str, str] = {}  # device_id → address
        try:
            for fresh in discover_serial_devices():
                fresh_serial_ports[fresh.device_id] = fresh.address
        except Exception:
            pass  # Discovery failure is non-fatal

        # Snapshot to avoid RuntimeError if remove_device() mutates dict
        # concurrently (e.g., from API endpoint during an await point).
        for device_id, device in list(self._devices.items()):
            if device.state not in (DeviceState.DISCONNECTED, DeviceState.ERROR):
                continue

            # Check in-memory blackout (set by release_device with hold_seconds)
            blackout = self._reconnect_blackout.get(device_id)
            if blackout:
                if _time.monotonic() < blackout:
                    continue
                del self._reconnect_blackout[device_id]

            disc = self._device_discovery.get(device_id)
            if not disc:
                continue

            # Skip DFU bootloader placeholders — they can't be reconnected
            # normally (ble_dfu transport is not a connectable transport type).
            if disc.transport_type == "ble_dfu":
                continue

            # Exponential backoff for repeated failures.
            # After 20 consecutive failures, remove from fleet entirely.
            fail_count = self._reconnect_failures.get(device_id, 0)
            if fail_count >= 20:
                log.info(
                    "Giving up on %s after %d failures — removing from fleet",
                    device_id[:12],
                    fail_count,
                )
                to_remove_after.append(device_id)
                continue
            # Serial devices with 3+ consecutive failures: stop reconnect
            # attempts. Repeated serial open/close on a device with broken
            # CDC makes things worse (DTR toggling, USB re-enumeration
            # storms). The device may still be reachable via BLE. Increment
            # fail_count each cycle so the device still reaches the 20-failure
            # removal threshold and doesn't stay as a zombie indefinitely.
            if disc.transport_type == "serial" and fail_count >= 3:
                if fail_count == 3:
                    log.warning(
                        "Serial device %s failed 3 times — stopping serial retries. "
                        "Device may be reachable via BLE.",
                        device_id[:12],
                    )
                self._reconnect_failures[device_id] = fail_count + 1
                continue

            if fail_count > 0:
                # Backoff: skip N-1 discovery cycles (10s each) per failure,
                # doubling each time, capped at 30 cycles (5 min)
                skip_cycles = min(2 ** (fail_count - 1), 30)
                counter = self._backoff_skip.get(device_id, 0)
                if counter < skip_cycles:
                    self._backoff_skip[device_id] = counter + 1
                    continue
                self._backoff_skip[device_id] = 0

            try:
                # Refresh port path from cached discovery (avoid per-device USB scan)
                if disc.transport_type == "serial" and disc.device_id in fresh_serial_ports:
                    new_addr = fresh_serial_ports[disc.device_id]
                    if new_addr != disc.address:
                        log.info(
                            "Port changed for %s: %s -> %s",
                            device_id[:12],
                            disc.address,
                            new_addr,
                        )
                        disc.address = new_addr

                transport = _create_transport(disc)
                device.transport = transport
                device.protocol = DeviceProtocol(transport)
                device.protocol.on_stream_line(device._route_stream_line)
                device.protocol.on_raw_line(device._on_raw_line)
                await device.connect()
                # Success — reset failure counter
                self._reconnect_failures.pop(device_id, None)
            except Exception as e:
                self._reconnect_failures[device_id] = fail_count + 1
                backoff_s = min(2**fail_count, 30) * DISCOVERY_INTERVAL_S
                log.warning(
                    "Reconnect failed for %s (attempt %d, next in ~%ds): %s",
                    device_id[:12],
                    fail_count + 1,
                    backoff_s,
                    e,
                )

        # Remove devices that exceeded the failure limit
        for did in to_remove_after:
            dev = self._devices.pop(did, None)
            self._device_discovery.pop(did, None)
            self._reconnect_failures.pop(did, None)
            self._backoff_skip.pop(did, None)
            if dev:
                # Best-effort cleanup: ensure underlying transport is closed
                with contextlib.suppress(Exception):
                    await dev.disconnect()
                log.info("Removed unreachable device %s from fleet", did[:12])

    def _check_serial_threads(self) -> None:
        """Detect dead serial reader threads.

        If a serial reader thread exited without firing the disconnect callback
        (e.g., uncaught exception, or thread killed by OS), the device appears
        connected but is actually dead. Force disconnect so reconnect can pick it up.
        """
        for device in self._devices.values():
            if device.state != DeviceState.CONNECTED:
                continue
            if not isinstance(device.transport, SerialTransport):
                continue
            if not device.transport.is_reader_alive():
                log.warning(
                    "Serial reader thread dead for %s (%s) — forcing disconnect",
                    device.id[:12],
                    device.port,
                )
                # Full disconnect: closes port, cleans up transport state,
                # sets device state to DISCONNECTED so reconnect picks it up.
                task = asyncio.create_task(device.disconnect())
                self._background_tasks.add(task)

                def _on_disconnect_done(t: asyncio.Task[None]) -> None:
                    self._background_tasks.discard(t)
                    if not t.cancelled() and (exc := t.exception()):
                        log.warning("disconnect error: %s", exc)

                task.add_done_callback(_on_disconnect_done)

    async def _check_liveness(self) -> None:
        """Ping devices that haven't communicated recently.

        Detects stale connections on both BLE and serial transports.
        BLE connections can silently drop; serial connections can become
        unresponsive if the device resets or enters bootloader. A lightweight
        `json info` command verifies the connection is alive and refreshes
        device metadata.
        """
        now = _time.monotonic()
        # 20s threshold guarantees ~30s worst-case detection (check runs every 3rd
        # cycle at 10s intervals, so up to 30s between checks)
        stale_threshold = 20.0

        tasks = []
        for device in list(self._devices.values()):
            if device.state != DeviceState.CONNECTED:
                continue
            if device.last_seen and (now - device.last_seen) < stale_threshold:
                continue
            tasks.append(self._ping_device(device))

        if tasks:
            await asyncio.gather(*tasks)

    async def _reap_stale_devices(self) -> None:
        """Evict devices that have been disconnected/errored for too long.

        Removes from the in-memory fleet so /api/devices stops returning a
        dead slot. The device will be re-discovered on the next scan if it
        comes back. CONNECTED/CONNECTING devices are exempt (liveness handles
        those). DFU_RECOVERY is exempt — those are stuck in bootloader and
        re-discovery would just re-stick them in a flap loop; operator must
        intervene via /api/devices/{id}/flash or DELETE.

        PRESENT devices are now ALSO reapable. PR #140 review: BLE devices
        in PRESENT state that go out of range stop being seen by discovery
        (last_seen stops updating), but the slot stays in the fleet forever
        — accumulating ghost entries over time. Discovery refreshes
        last_seen on every scan, so PRESENT + stale last_seen really does
        mean "device disappeared." Same REAP_THRESHOLD_S applies.
        """
        now = _time.monotonic()
        reapable = {DeviceState.DISCONNECTED, DeviceState.ERROR, DeviceState.PRESENT}
        victims: list[str] = []
        for device in list(self._devices.values()):
            if device.state not in reapable:
                continue
            if device.last_seen is None:
                continue  # never connected — leave alone, discovery is still trying
            if (now - device.last_seen) < REAP_THRESHOLD_S:
                continue
            victims.append(device.id)

        for device_id in victims:
            device_opt = self._devices.get(device_id)
            ago = (now - device_opt.last_seen) if device_opt and device_opt.last_seen else None
            log.warning(
                "Reaping stale device %s (state=%s, last_seen=%.0fs ago)",
                device_id[:12],
                device_opt.state.value if device_opt else "?",
                ago if ago is not None else -1,
            )
            await self.remove_device(device_id)

    async def _ping_device(self, device: Device) -> None:
        """Ping a single device; trigger disconnect handling on failure.

        For BLE devices, also updates RSSI from the BlueZ D-Bus proxy so
        signal strength stays current for fleet monitoring.
        """
        try:
            info = await device.protocol.get_info()
            device.last_seen = _time.monotonic()
            device.apply_info(info)
            # Update RSSI from connected BLE transport
            if hasattr(device.transport, "read_rssi"):
                rssi = await device.transport.read_rssi()
                if rssi is not None:
                    device.rssi = rssi
        except Exception:
            log.warning(
                "Liveness check failed for %s (%s) — triggering disconnect",
                device.id[:12],
                device.transport.transport_type,
            )
            with contextlib.suppress(Exception):
                device._on_transport_disconnect()

    @staticmethod
    def _recovery_state_path() -> Path:
        """Persistent state file for recovery firmware + device whitelist.

        Lives under ``~/.local/share/blinky-server/`` so it survives reboots.
        Previously /tmp, which is tmpfs on Pi — a power cycle erased it and
        any device stuck in DFU bootloader could no longer auto-recover.

        File format: JSON ``{"firmware_path": str, "device_ids": [str, ...]}``.
        An older plain-string format used to live here; it's rejected on load
        because it has no whitelist, and auto-flashing all dfu_recovery
        devices unscoped is unsafe (could clobber a dev unit on the bench).
        """
        from ..paths import data_dir

        return data_dir() / "recovery-firmware.json"

    @staticmethod
    def _identity_aliases_path() -> Path:
        """Persistent storage for ``_identity_groups``.

        Lives alongside ``recovery-firmware.json`` so the
        canonical-key resolver survives a server restart. The pre-PR
        ``_identity_groups`` was in-memory only — meaning that after
        every restart, the whitelist match for a DFU-state device
        relied on discovery seeing the device on BOTH transports to
        rebuild the SN ↔ BLE addr mapping. If the device was already
        in DFU at startup (the recovery scenario), discovery would
        only see the BLE form, the alias couldn't be re-established,
        and an SN-only whitelist wouldn't match — closing exactly
        the gap PR #143 Copilot #2 called out.

        Format: JSON dict mapping alias → canonical. Loaded once at
        ``start()`` and rewritten on every ``register_identity_alias``
        call.
        """
        from ..paths import data_dir

        return data_dir() / "device_aliases.json"

    def _persist_identity_aliases(self) -> None:
        """Append-write ``_identity_groups`` to disk.

        Called from ``register_identity_alias`` after a successful
        update. Failure logs WARN — alias persistence is best-effort,
        and the in-memory state is still correct for the running
        session.
        """
        path = self._identity_aliases_path()
        try:
            path.write_text(json.dumps(dict(self._identity_groups)))
        except OSError as exc:
            log.warning("Failed to persist device-aliases state: %s", exc)

    def _load_identity_aliases(self) -> None:
        """Read ``_identity_groups`` from disk at server start.

        Tolerant of missing / malformed files (logs WARN, leaves the
        in-memory dict empty — equivalent to the pre-PR behavior so
        a corrupt file can't keep the server from starting).
        """
        path = self._identity_aliases_path()
        if not path.is_file():
            return
        try:
            raw = path.read_text()
        except OSError as exc:
            log.warning("Failed to read device-aliases state: %s", exc)
            return
        try:
            data = json.loads(raw)
        except json.JSONDecodeError as exc:
            log.warning("device-aliases state is malformed (%s); ignoring", exc)
            return
        if not isinstance(data, dict) or not all(
            isinstance(k, str) and isinstance(v, str) for k, v in data.items()
        ):
            log.warning("device-aliases state has unexpected shape; ignoring")
            return
        self._identity_groups.update(data)
        if data:
            log.info("Loaded %d device-alias mappings from disk", len(data))

    @staticmethod
    def _flash_audit_log_path() -> Path:
        """Append-only JSONL audit of every ``FlashJob`` terminal transition.

        Lives under ``~/.local/share/blinky-server/`` alongside the
        recovery state. One JSON object per line; each line is a
        complete snapshot of a terminal FlashJob (job_id, device_id,
        canonical_id, firmware_path, state, error, timestamps, etc).

        The file is the persistent backing of ``_recent_flash_attempts``:
        without it, a server restart erases the dedup window and
        auto-recovery on the first cycle after restart could
        re-flash a device that just failed pre-restart. The plan §L4
        calls this out as "an unintended escape hatch for the very
        mistake we're guarding against" (the 2026-05-17 cascade).
        """
        from ..paths import data_dir

        return data_dir() / "flash_attempts.jsonl"

    def _write_flash_audit_entry(self, job: FlashJob, canonical: str) -> None:
        """Append one JSONL line for a terminal ``FlashJob``.

        Called from ``_run_flash_job``'s finally under the per-device
        lock. Failures here MUST NOT propagate (the flash succeeded
        as far as the device cares); they're logged at WARN so the
        operator sees them in the journal. Synchronous file I/O —
        the cost is negligible relative to the flash itself (~100ms
        worst case on a busy filesystem; the flash itself is minutes).
        """
        entry: dict[str, Any] = {
            "job_id": job.job_id,
            "device_id": job.device_id,
            "canonical_id": canonical,
            "is_auto_recovery": job.is_auto_recovery,
            "firmware_path": str(job.firmware_path),
            "expected_version": job.expected_version,
            "transport": job.transport.value if job.transport else None,
            "created_at": job.created_at,
            "started_at": job.started_at,
            "write_completed_at": job.write_completed_at,
            "verified_at": job.verified_at,
            "finished_at": job.finished_at,
            "state": job.state.value,
            "error": job.error,
            "anomalies": list(job.anomalies),
        }
        path = self._flash_audit_log_path()
        try:
            # Append with newline. ``open(append)`` is atomic for a
            # single write() of bytes shorter than PIPE_BUF (4 KB on
            # Linux). Our entries are well under that; concurrent
            # writers from separate processes (we don't have any
            # today, but defensive) would not interleave.
            with path.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(entry) + "\n")
        except OSError as exc:
            log.warning(
                "Failed to append flash-audit entry for job %s: %s — "
                "dedup window won't survive restart for this attempt",
                job.job_id,
                exc,
            )

    def _load_recent_flash_attempts_from_audit_log(self) -> None:
        """Repopulate ``_recent_flash_attempts`` from the JSONL audit log.

        Called at server start. Reads every entry in the file and
        keeps those with a ``finished_at`` within
        ``FLASH_DEDUP_WINDOW_S`` of now. Older entries are skipped
        (they're outside the window so they wouldn't dedup anyway,
        and we don't want stale data lingering in memory).

        Tolerant of partial / corrupt lines: a line that can't be
        parsed as JSON is skipped with a WARN, the rest of the file
        is still consumed. The audit log is append-only by contract,
        so corruption should be exceedingly rare — but a power-cut
        mid-write could in theory leave a truncated final line.

        Without this method, ``_recent_flash_attempts`` starts empty
        at boot — auto-recovery's first cycle could re-fire on a
        device that just failed pre-restart (the cascade pattern the
        plan §L4 closes).
        """
        path = self._flash_audit_log_path()
        if not path.is_file():
            return
        now = _time.time()
        loaded = 0
        skipped_old = 0
        skipped_bad = 0
        try:
            with path.open("r", encoding="utf-8") as fh:
                for line_num, raw in enumerate(fh, start=1):
                    raw = raw.strip()
                    if not raw:
                        continue
                    try:
                        entry = json.loads(raw)
                    except json.JSONDecodeError:
                        log.warning("Flash-audit log line %d unparseable; skipping", line_num)
                        skipped_bad += 1
                        continue
                    canonical = entry.get("canonical_id") or entry.get("device_id")
                    finished_at = entry.get("finished_at")
                    if not isinstance(canonical, str) or not isinstance(finished_at, (int, float)):
                        skipped_bad += 1
                        continue
                    if (now - finished_at) >= self.FLASH_DEDUP_WINDOW_S:
                        skipped_old += 1
                        continue
                    # Take the MAX of the existing in-memory value (if
                    # any) and the audit value — multiple historical
                    # attempts for the same canonical should resolve
                    # to the most-recent one, which is the conservative
                    # choice for dedup.
                    existing = self._recent_flash_attempts.get(canonical)
                    if existing is None or finished_at > existing:
                        self._recent_flash_attempts[canonical] = finished_at
                    loaded += 1
        except OSError as exc:
            log.warning("Failed to read flash-audit log %s: %s", path, exc)
            return
        if loaded or skipped_bad:
            log.info(
                "Flash-audit log: rebuilt dedup window from %d entries "
                "(%d skipped as stale, %d skipped as malformed)",
                loaded,
                skipped_old,
                skipped_bad,
            )

    def set_recovery_firmware(self, firmware_path: str, device_ids: list[str]) -> None:
        """Arm automatic DFU recovery, scoped to an explicit device whitelist.

        When armed, the fleet manager attempts BLE DFU on devices in this
        list that show up in DFU_RECOVERY state. Devices NOT in the list
        are left alone — this is the safety mechanism that keeps a cart
        deploy from auto-flashing a hat being developed separately, etc.

        Persisted to ~/.local/share/blinky-server/ so the arm survives a
        reboot. After a power cycle a stuck device still auto-recovers
        without operator intervention.

        Raises ValueError on:
          * an empty device_ids list (would silently disable recovery while
            looking like it was armed — the "no silent fallback" failure mode)
          * a missing firmware file (fail loud at set time rather than
            getting a no-op-with-warning at recovery time when the operator
            isn't around to notice — PR #140 review)
          * any non-string entry in device_ids (the JSON round-trip would
            otherwise pass through e.g. ``[None]`` which would never match
            a real device ID)
        """
        if not device_ids:
            raise ValueError("set_recovery_firmware requires a non-empty device_ids list")
        if not all(isinstance(d, str) and d for d in device_ids):
            raise ValueError(
                f"set_recovery_firmware: device_ids must all be non-empty strings, "
                f"got {device_ids!r}"
            )
        if not Path(firmware_path).is_file():
            raise ValueError(
                f"set_recovery_firmware: firmware file does not exist: {firmware_path}"
            )
        self._recovery_firmware_path = firmware_path
        self._recovery_device_ids = set(device_ids)
        state = {"firmware_path": firmware_path, "device_ids": sorted(set(device_ids))}
        try:
            self._recovery_state_path().write_text(json.dumps(state))
        except OSError:
            log.warning("Failed to persist recovery-firmware state")
        log.info(
            "Recovery firmware armed: %s (devices: %s)",
            firmware_path,
            ", ".join(sorted(set(device_ids))) or "<none>",
        )

    def _load_recovery_firmware(self) -> tuple[str, set[str]] | None:
        """Load persisted recovery firmware path + device whitelist.

        Returns (firmware_path, device_ids) on success. Returns None if the
        state file is missing, the firmware file no longer exists, the file
        is malformed, OR it's in the legacy plain-string format (which had
        no whitelist — refusing those is safer than auto-flashing the wrong
        device).
        """
        path = self._recovery_state_path()
        try:
            raw = path.read_text().strip()
        except OSError:
            return None
        if not raw:
            return None
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            log.warning(
                "Ignoring legacy recovery-firmware state at %s (plain string; "
                "no device whitelist — refusing to auto-flash unscoped)",
                path,
            )
            return None
        firmware_path = data.get("firmware_path")
        device_ids = data.get("device_ids")
        if not (
            isinstance(firmware_path, str)
            and isinstance(device_ids, list)
            and device_ids
            and all(isinstance(d, str) and d for d in device_ids)
        ):
            log.warning("Ignoring malformed recovery-firmware state at %s", path)
            self._clear_recovery_state_file(path, reason="malformed")
            return None
        if not Path(firmware_path).is_file():
            log.warning("Recovery firmware %s no longer exists; not arming", firmware_path)
            self._clear_recovery_state_file(path, reason="firmware-missing")
            return None
        return firmware_path, set(device_ids)

    @staticmethod
    def _clear_recovery_state_file(path: Path, reason: str) -> None:
        """Remove a stale recovery-state JSON so the rejection log fires
        once, not on every boot. PR #140 review.
        """
        try:
            path.unlink(missing_ok=True)
            log.info("Cleared stale recovery-firmware state (%s)", reason)
        except OSError:
            log.exception("Failed to clear stale recovery-firmware state")

    def _maybe_reload_recovery_whitelist(self) -> None:
        """L3.1: refresh ``_recovery_firmware_path`` / ``_recovery_device_ids``
        from the persisted JSON if the file's mtime has changed.

        Closes the gap where an operator-edited recovery-firmware.json
        wouldn't take effect until a server restart. The stat() call is
        rate-limited to once per ``RECOVERY_WHITELIST_RELOAD_TTL_S`` so
        a high-frequency auto-recovery cycle doesn't hammer the
        filesystem; the JSON re-read only fires when mtime changes.

        File-absent handling (PR #143 Copilot review):

        - ``FileNotFoundError`` (ENOENT): treat as operator intent to
          disarm. The operator deliberately deleted the file; honor
          that by clearing the in-memory ``_recovery_firmware_path``
          and ``_recovery_device_ids``. Logs INFO so the disarm shows
          up in the journal as a deliberate state change.
        - Other ``OSError`` (PermissionError, transient I/O, NFS
          unmount, etc.): keep the in-memory state and log a WARN
          (latched on first occurrence to avoid spam). Distinguishing
          ENOENT from generic I/O avoids silently disarming
          auto-recovery on a transient mount issue while still
          honoring the operator's explicit ``rm`` as a disarm signal.
        - Invalid/malformed JSON: ``_load_recovery_firmware`` returns
          None and ``_clear_recovery_state_file`` removes the bad file;
          we treat that the same as the ENOENT path (clear in-memory
          state) so a corrupted file doesn't leave stale recovery
          armed.
        """
        now = _time.monotonic()
        if now - self._last_whitelist_check_at < RECOVERY_WHITELIST_RELOAD_TTL_S:
            return
        self._last_whitelist_check_at = now
        path = self._recovery_state_path()
        try:
            mtime = path.stat().st_mtime
        except FileNotFoundError:
            # Operator-deliberate delete: honor as disarm.
            if self._recovery_firmware_path is not None or self._recovery_device_ids:
                log.info(
                    "Recovery-firmware state file %s removed; disarming "
                    "auto-recovery (operator intent inferred from rm)",
                    path,
                )
                self._recovery_firmware_path = None
                self._recovery_device_ids = set()
            self._last_whitelist_mtime = None
            self._whitelist_missing_warned = False
            return
        except OSError as exc:
            # Transient I/O — preserve in-memory state, WARN once.
            if self._last_whitelist_mtime is not None and not self._whitelist_missing_warned:
                log.warning(
                    "Recovery-firmware state file %s is unreadable (%s). "
                    "Keeping in-memory whitelist active; if you intended to "
                    "disarm, remove the file with rm (which the loader treats "
                    "as a deliberate disarm) — a generic I/O error is treated "
                    "as transient so a permission flap or NFS unmount doesn't "
                    "silently disarm auto-recovery.",
                    path,
                    exc,
                )
                self._whitelist_missing_warned = True
            return
        # File came back after a missing-warning — reset the latch so a
        # second unreadable case gets logged again.
        self._whitelist_missing_warned = False
        if self._last_whitelist_mtime is not None and mtime == self._last_whitelist_mtime:
            return
        self._last_whitelist_mtime = mtime
        persisted = self._load_recovery_firmware()
        if persisted is None:
            # Malformed file. ``_load_recovery_firmware`` already
            # cleared the file via ``_clear_recovery_state_file`` (so
            # the rejection log doesn't repeat on every cycle). Treat
            # like a deliberate disarm: clear in-memory state so a
            # corrupt JSON doesn't leave the OLD whitelist active.
            if self._recovery_firmware_path is not None or self._recovery_device_ids:
                log.warning(
                    "Recovery-firmware state was malformed; disarming "
                    "auto-recovery in memory to match the cleared file"
                )
                self._recovery_firmware_path = None
                self._recovery_device_ids = set()
            return
        firmware_path, device_ids = persisted
        if firmware_path != self._recovery_firmware_path or device_ids != self._recovery_device_ids:
            log.info(
                "Auto-recovery whitelist refreshed from disk: %s (devices: %s)",
                firmware_path,
                ", ".join(sorted(device_ids)) or "<none>",
            )
            self._recovery_firmware_path = firmware_path
            self._recovery_device_ids = device_ids

    async def _auto_recover_dfu_devices(self) -> None:
        """Thin loop over ``flash_device(force=False)`` for devices
        stuck in DFU bootloader.

        The orchestrator's ``_run_flash_job`` owns the BLE-DFU plumbing
        (pause_discovery, hold_reconnect, broadcaster stop/start, write,
        verify); ``flash_device(force=False)`` owns the max-attempts +
        backoff dedup via ``should_attempt_auto_recovery``; the canonical
        in-flight set is the cross-caller mutex.

        What this method does:

          - Re-read the whitelist from disk if its mtime changed
            (L3.1 — closes the operator-edit-without-restart gap).
          - Filter the fleet to DFU_RECOVERY devices with a BLE address
            that are in the whitelist (canonical-resolved).
          - For each, call ``flash_device(force=False)``:
            * ``None`` returned → blocked by dedup / max-attempts /
              backoff. Log the giveup once if we just hit the cap.
            * ``FlashJob`` returned → wait for terminal. Log result.

        Strictly serial: one device at a time, finish fully before next.
        BLE DFU uses the radio exclusively so concurrent recovery
        attempts would interfere; the in-flight set already enforces
        per-device, but we ALSO want per-cycle for cross-device.
        """
        self._maybe_reload_recovery_whitelist()
        firmware_path = self._recovery_firmware_path
        if not firmware_path or not os.path.isfile(firmware_path):
            return
        # No whitelist = nothing to recover. The whitelist is the safety
        # belt against auto-flashing a device the operator didn't include
        # in this deploy.
        if not self._recovery_device_ids:
            return

        # Canonical-resolved whitelist so an SN-only whitelist still
        # matches a BLE-addr-only DFU device after the alias is registered
        # (the cart_inner 2026-05-16 brick mode).
        whitelist_canonical = {self.resolve_canonical(wid) for wid in self._recovery_device_ids}

        dfu_devices = [
            d
            for d in self._devices.values()
            if d.state == DeviceState.DFU_RECOVERY
            and d.ble_address
            and self.resolve_canonical(d.id) in whitelist_canonical
        ]
        if not dfu_devices:
            return

        for device in dfu_devices:
            canonical = self.resolve_canonical(device.id)

            # Log "GIVING UP" exactly once when we've hit max-attempts.
            # `should_attempt_auto_recovery` will keep returning False
            # for this device until the operator manually flashes it
            # (which clears the retry counter on COMPLETED).
            retry = self._recovery_retry_state.get(canonical)
            if retry is not None and retry.fails >= MAX_AUTO_RECOVERY_ATTEMPTS:
                if not retry.gave_up_logged:
                    log.error(
                        "Auto-recovery GIVING UP for %s (%s) after %d attempts. "
                        "Manual intervention required. State is in-memory only — "
                        "a server restart clears the cap, which is intentional: "
                        "a fresh boot is a deliberate operator action.",
                        device.id[:12],
                        device.device_name or "unknown",
                        retry.fails,
                    )
                    retry.gave_up_logged = True
                continue

            log.info(
                "Auto-recovering DFU device %s (BLE: %s, attempt %d/%d)...",
                device.id[:12],
                device.ble_address,
                (retry.fails if retry else 0) + 1,
                MAX_AUTO_RECOVERY_ATTEMPTS,
            )

            # Single entry point: orchestrator handles transport selection
            # (BLE-DFU for DFU_RECOVERY state), pause_discovery,
            # hold_reconnect, broadcaster.stop, write, verify, and the
            # finally-block update of `_recovery_retry_state`. We just
            # wait for terminal and log the outcome.
            job = await self.flash_device(device.id, Path(firmware_path), force=False)
            if job is None:
                # Blocked by `should_attempt_auto_recovery` (recently
                # attempted, in-flight elsewhere, or inside backoff).
                # Try again next cycle.
                continue

            # 900s outer cap: orchestrator's _run_ble_dfu_flash internally
            # bounds at 600s wait_for around the BLE-DFU impl plus its
            # verify scan. 900s leaves margin for the verify-scan tail.
            terminal = await job.wait_until_terminal(timeout=900.0)
            if not terminal:
                log.error(
                    "Auto-recovery for %s: wait_until_terminal hit 900s cap — "
                    "orchestrator may still be running",
                    device.id[:12],
                )
                continue

            if job.state is FlashJobState.COMPLETED:
                log.info("Auto-recovery succeeded for %s!", device.id[:12])
            else:
                log.warning(
                    "Auto-recovery failed for %s: state=%s error=%s",
                    device.id[:12],
                    job.state.value,
                    job.error or "(none)",
                )

    # Fallback ping cadence when systemd doesn't provide WATCHDOG_USEC
    # (running outside a unit, or WatchdogSec=0). 30s is conservative.
    WATCHDOG_PING_INTERVAL_FALLBACK_S = 30.0

    def _watchdog_ping_interval(self) -> float:
        """Compute the watchdog ping cadence from $WATCHDOG_USEC.

        Per systemd docs, services should ping at half the watchdog
        interval to leave headroom for missed beats. PR #140 review:
        the previous hardcoded 30s broke for tighter WatchdogSec settings
        (e.g., WatchdogSec=15s would miss every other beat).
        """
        wdog = systemd_notify.watchdog_sec()
        if wdog is None:
            return self.WATCHDOG_PING_INTERVAL_FALLBACK_S
        # half-interval per systemd convention
        return max(wdog / 2.0, 1.0)

    async def _watchdog_pinger(self) -> None:
        """Ping systemd's watchdog on a fixed cadence, independent of the
        main background loop.

        Why this is a separate task: long-running operations inside the
        background loop (BLE DFU via auto-recovery, in particular) `await`
        for many minutes. While that await is pending, the main loop never
        returns to its `while self._running` head, so the in-loop watchdog
        ping (full-cycle path AND pause-path) never fires. We pinned this
        live on 2026-05-16 — the first kill was the cause of cart_inner's
        partial-flash brick.

        A separate task pings while the asyncio event loop is alive, which
        is exactly what we want the systemd watchdog to gate on. A hard
        event-loop deadlock still SIGKILLs us — by design.
        """
        interval = self._watchdog_ping_interval()
        log.info("Watchdog pinger started (every %.1fs)", interval)
        try:
            while self._running:
                systemd_notify.watchdog()
                await asyncio.sleep(interval)
        except asyncio.CancelledError:
            pass
        finally:
            log.info("Watchdog pinger stopped")

    async def _background_loop(self) -> None:
        """Periodic discovery, reconnection, liveness checks, and DFU recovery."""
        cycle = 0
        # BLE cleanup on first iteration (moved from start() to avoid blocking API)
        if self._enable_ble:
            try:
                await cleanup_stale_ble_connections()
                await asyncio.sleep(2)
            except Exception:
                log.exception("BLE cleanup failed (non-fatal)")
        while self._running:
            try:
                await asyncio.sleep(DISCOVERY_INTERVAL_S)
                cycle += 1
                if self._discovery_pause_count > 0:
                    # Paused intentionally (BLE DFU in progress, etc). Still
                    # counts as a healthy tick for the watchdog — we ARE
                    # doing work, just elsewhere.
                    #
                    # The in-loop systemd_notify.watchdog() call here is
                    # now defense-in-depth: the independent _watchdog_pinger
                    # task is the primary mechanism that keeps systemd happy
                    # while a long await (BLE DFU auto-recovery) blocks
                    # this loop. Don't remove this call thinking the
                    # pinger covers it — it's belt-and-braces for the
                    # 2026-05-16 cart_inner regression.
                    self._loop_last_ok = _time.monotonic()
                    self._loop_cycles = cycle
                    self._loop_consecutive_errors = 0
                    systemd_notify.watchdog()
                    continue
                await self._discover_and_connect()
                await self._reconnect_disconnected()
                # Detect dead serial reader threads (thread exited but device still
                # appears connected). This catches silent thread deaths.
                self._check_serial_threads()
                # Liveness check every 3rd cycle (~30s) for all transports
                if cycle % 3 == 0:
                    await self._check_liveness()
                # DFU recovery check every 6th cycle (~60s)
                if cycle % 6 == 0:
                    await self._auto_recover_dfu_devices()
                # Reap long-dead non-recoverable devices every 6th cycle (~60s)
                if cycle % REAP_INTERVAL_CYCLES == 0:
                    await self._reap_stale_devices()
                # Mark this cycle as healthy AFTER all work succeeded.
                # The systemd watchdog ping here is defense-in-depth (the
                # independent _watchdog_pinger task is the primary). It's
                # left in deliberately: a long inline await IS our hang
                # scenario, and the pinger covers it. The in-loop call
                # exists so that a loop that completes a full cycle marks
                # _loop_last_ok and pings within the same critical
                # section — keep them adjacent.
                self._loop_last_ok = _time.monotonic()
                self._loop_cycles = cycle
                self._loop_consecutive_errors = 0
                systemd_notify.watchdog()
            except asyncio.CancelledError:
                break
            except Exception:
                self._loop_consecutive_errors += 1
                # log.exception preserves the traceback — log.error("%s", e)
                # would lose it and make post-event diagnosis impossible.
                log.exception(
                    "Background loop error (cycle %d, consecutive errors %d)",
                    cycle,
                    self._loop_consecutive_errors,
                )
                # Backoff to avoid spinning if the failure is persistent
                # (e.g. BlueZ wedged). Cap at 5x normal interval so we still
                # try to recover periodically.
                extra_sleep = min(
                    self._loop_consecutive_errors * DISCOVERY_INTERVAL_S,
                    5 * DISCOVERY_INTERVAL_S,
                )
                with contextlib.suppress(asyncio.CancelledError):
                    await asyncio.sleep(extra_sleep)


# ─────────────────────────────────────────────────────────────────────
# Phase 7: production VerifySignals for the UF2 path.
#
# Plugs the verify state machine into real fleet state. The signals are:
#   - has_re_enumerated_since: snapshot the device's USB device-number
#       at first call, then watch for any change after `since`. The
#       lsusb device number changes on every USB re-enumeration so this
#       is the cleanest signal short of inotify on /dev.
#   - is_serial_connected: check Device.transport.is_connected (the
#       FleetManager's normal reconnect loop maintains this).
#   - get_handshake_info: send `json info` via Device.protocol and
#       parse the response as a dict. None on any I/O error.
# ─────────────────────────────────────────────────────────────────────


class _FleetVerifySignals:
    """Verify-signal source backed by FleetManager + udev sysfs reads.

    Constructed with the ``SignalHistory`` the anomaly sweep will read
    from. Wiring history in via the constructor (rather than as a
    settable attribute the caller is expected to assign after the fact)
    closes a non-obvious invisible-dependency: if a future test or
    caller constructs ``_FleetVerifySignals`` and forgets an external
    ``signals.history = history`` line, the anomaly sweep silently
    sees no data and detectors never fire.
    """

    def __init__(
        self,
        fleet: FleetManager,
        device_id: str,
        history: SignalHistory,
    ) -> None:
        self._fleet = fleet
        self._device_id = device_id
        self.history: SignalHistory = history
        # Capture the USB devnum baseline AT CONSTRUCTION TIME, not at
        # first poll. The flash workflow goes:
        #   construct signals  →  USB reset  →  UF2 write  →  device
        #   reboot (BL → app, devnum changes)  →  run_verify starts polling
        # If the baseline were captured at first poll (the prior bug),
        # it would lock onto the POST-reboot devnum and never see a
        # subsequent change — verify stalls in AWAITING_REBOOT until
        # the 5-min cap. Capturing at __init__ pins the PRE-reboot
        # devnum, so the next compare after the reboot fires
        # re-enumerated=True correctly.
        #
        # Caller contract: construct this signals object BEFORE invoking
        # `_uf2_write_impl_for_job` (or any other code that triggers a USB
        # reset / reboot of the target device).
        self._initial_devnum: int | None = self._read_devnum_for_device()
        self._initial_devnum_captured_at: float = _time.time()
        # One-shot flag: the first exception bubbling out of
        # is_serial_connected gets logged at WARN; subsequent ones at
        # DEBUG. Verify polls every ~250 ms, so logging every miss
        # would spam journalctl during a normal multi-second
        # AWAITING_APP_BOOT window — but a silent failure here stalls
        # the state machine invisibly, which CLAUDE.md explicitly
        # forbids. First-call WARN gives the operator a single
        # discoverable line; the polling loop's existing forward
        # progress (or lack thereof) provides the rest of the signal.
        self._is_connected_warned: bool = False

    async def has_re_enumerated_since(self, device_id: str, since: float) -> bool:
        """True if the USB device number has changed since construction.

        Baseline is captured at __init__ — the caller is responsible for
        constructing the signals object BEFORE the destructive flash so
        the baseline reflects pre-flash state. The ``since`` parameter
        is part of the `VerifySignals` protocol but not consulted here:
        the construction-time snapshot IS our "before the flash" marker,
        and that's what we compare against.

        The device's serial number is the stable identity; we look it
        up in ``/dev/serial/by-id`` and read the device number from
        sysfs. Returns False when the device isn't enumerated right
        now (mid-reboot or stuck in bootloader) — the next poll will
        see the new devnum and fire.
        """
        current_devnum = self._read_devnum_for_device()
        if current_devnum is None:
            # Device not currently enumerated. Could be mid-reboot or
            # stuck in BL — either way, not "we saw it re-enum on the
            # other side" yet. Wait for the next poll.
            return False
        if self._initial_devnum is None:
            # Construction happened while the device wasn't enumerated
            # (rare — would mean we built signals after the USB reset
            # had already taken effect). Any enumeration we see now
            # counts as re-enum. Bump the baseline so we don't fire
            # again on the same enumeration.
            self.history.re_enum_timestamps.append(_time.time())
            self._initial_devnum = current_devnum
            return True
        if current_devnum != self._initial_devnum:
            # Re-enum detected. Bump the baseline so subsequent polls
            # don't keep firing on the same edge.
            self.history.re_enum_timestamps.append(_time.time())
            self._initial_devnum = current_devnum
            return True
        return False

    async def is_serial_connected(self, device_id: str) -> bool:
        device = self._fleet._devices.get(device_id)
        if device is None:
            return False
        try:
            return bool(device.transport.is_connected)
        except Exception as exc:
            # Returning False on an unexpected exception stalls the
            # verify state machine at AWAITING_APP_BOOT — invisible to
            # the operator if not logged. CLAUDE.md forbids the silent
            # form. Compromise: WARN on the first occurrence so it
            # shows up in journalctl without --debug; DEBUG on
            # subsequent ones (verify polls every ~250 ms, so a true
            # WARN every iteration would drown the log). The first
            # line is the discoverable bread crumb; the lack of
            # forward progress on the job is the ongoing signal.
            if not self._is_connected_warned:
                self._is_connected_warned = True
                log.warning(
                    "is_serial_connected(%s) raised %s: %s — "
                    "treating as 'not connected'; subsequent occurrences logged at debug",
                    device_id,
                    type(exc).__name__,
                    exc,
                )
            else:
                log.debug(
                    "is_serial_connected(%s) raised: %s",
                    device_id,
                    exc,
                )
            return False

    async def get_handshake_info(self, device_id: str) -> dict[str, Any] | None:
        device = self._fleet._devices.get(device_id)
        if device is None:
            return None
        try:
            # Match the existing reconnect loop's probe — `json info` is
            # what the SerialConsole responds to with the firmware's
            # full identity blob (version, device config, safeMode flag).
            reply = await device.protocol.send_command("json info")
        except Exception as exc:
            log.debug(
                "get_handshake_info(%s): protocol raised %s",
                device_id,
                exc,
            )
            return None
        if not isinstance(reply, str) or not reply.strip():
            return None
        # The firmware emits JSON; tolerate leading/trailing log lines.
        try:
            payload = json.loads(reply.strip())
        except json.JSONDecodeError:
            # Some replies include a prose preamble — pull out the first
            # `{...}` block as a fallback.
            start = reply.find("{")
            end = reply.rfind("}")
            if start == -1 or end == -1 or end < start:
                return None
            try:
                payload = json.loads(reply[start : end + 1])
            except json.JSONDecodeError:
                return None
        if not isinstance(payload, dict):
            return None
        self.history.handshakes.append((_time.time(), payload))
        return payload

    def _read_devnum_for_device(self) -> int | None:
        """Look up the device's current USB device number via sysfs.

        Maps device_id (serial number, e.g., '062CBD12EB6961C8') to its
        ``/dev/serial/by-id/usb-Seeed_XIAO_nRF52840_Sense_<sn>-if00``
        symlink → realpath → ``/sys/class/tty/<name>/device/...``
        chain → ``devnum`` file. Returns None if the device isn't
        currently enumerated (which is itself a signal: still in
        bootloader, or unplugged).
        """
        try:
            by_id = Path("/dev/serial/by-id")
            if not by_id.exists():
                return None
            for entry in by_id.iterdir():
                if self._device_id not in entry.name:
                    continue
                tty_path = entry.resolve()  # e.g. /dev/ttyACM1
                tty_name = tty_path.name  # 'ttyACM1'
                # /sys/class/tty/ttyACM1/device → ../../../1-1.3:1.0
                sys_dev = Path(f"/sys/class/tty/{tty_name}/device")
                if not sys_dev.exists():
                    continue
                # Walk up to the parent USB device (the one with devnum).
                usb_dev = sys_dev.resolve().parent
                devnum_file = usb_dev / "devnum"
                if devnum_file.is_file():
                    return int(devnum_file.read_text().strip())
            return None
        except Exception as exc:
            log.debug("_read_devnum_for_device(%s) raised %s", self._device_id, exc)
            return None
