from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import os
import time as _time
from pathlib import Path
from typing import Any

from .. import systemd_notify
from ..ble.advertiser import FleetBroadcaster
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
        self._dfu_recovery_state: dict[str, dict[str, int]] = {}
        # Per-device set tracking which devices have active DFU recovery.
        # Prevents the background loop from starting a second recovery on a
        # device that's already being recovered (e.g., by a manual flash).
        self._dfu_recovery_active: set[str] = set()
        # Per-device DFU locks: prevent auto-recovery and manual flash from
        # running concurrently on the same device. Lock is acquired before any
        # DFU operation and held for its entire duration.
        self._dfu_locks: dict[str, asyncio.Lock] = {}
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
        # Verified necessary 2026-05-16: ping inside the loop's pause
        # path doesn't fire when auto-recovery awaits upload_ble_dfu
        # INSIDE the loop iteration — the loop never returns to the
        # top of while until DFU completes.
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

    async def start(self) -> None:
        """Start background loop for device discovery and management.

        Serial device connections happen in the background loop. BLE
        devices do NOT hold persistent connections — fleet commands go
        out via :attr:`broadcaster` (manufacturer-data advertising). See
        :class:`FleetBroadcaster` for the rationale.
        """
        self._running = True
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

    def get_dfu_lock(self, device_id: str) -> asyncio.Lock:
        """Get the per-device DFU lock. Creates one if it doesn't exist."""
        if device_id not in self._dfu_locks:
            self._dfu_locks[device_id] = asyncio.Lock()
        return self._dfu_locks[device_id]

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
        self._dfu_recovery_state.pop(device_id, None)
        self._dfu_locks.pop(device_id, None)
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

    async def _auto_recover_dfu_devices(self) -> None:
        """Automatically push firmware to devices stuck in DFU bootloader.

        Runs periodically. For each DFU_RECOVERY device with a known BLE
        address, attempts BLE DFU using the recovery firmware. Uses exponential
        backoff to avoid flooding BLE with retry attempts.
        """
        if self._dfu_recovery_active:
            # Skip entire cycle if any recovery is in-flight. BLE DFU uses
            # the adapter exclusively (BleakScanner conflicts with concurrent
            # scans), so we serialize recovery attempts even across devices.
            return

        firmware_path = self._recovery_firmware_path
        if not firmware_path:
            persisted = self._load_recovery_firmware()
            if persisted:
                firmware_path, device_ids = persisted
                self._recovery_firmware_path = firmware_path
                self._recovery_device_ids = device_ids
        if not firmware_path:
            return

        if not os.path.isfile(firmware_path):
            return

        # No whitelist = nothing to recover. The whitelist is the safety
        # belt against auto-flashing a device the operator didn't include
        # in this deploy (e.g. a dev unit in DFU on the bench). An armed
        # firmware path without a whitelist is treated as not-armed.
        if not self._recovery_device_ids:
            return

        dfu_devices = [
            d
            for d in self._devices.values()
            if d.state == DeviceState.DFU_RECOVERY
            and d.ble_address
            and d.id in self._recovery_device_ids
        ]
        if not dfu_devices:
            return

        for device in dfu_devices:
            state = self._dfu_recovery_state.setdefault(device.id, {"fails": 0, "backoff": 0})
            fail_count = state["fails"]

            # P4: stop hammering a device that keeps failing. After
            # MAX_AUTO_RECOVERY_ATTEMPTS, the device is presumed to need
            # operator attention (radio interference, firmware bug, hardware
            # issue, etc.). Continuing to attempt risks further damage to a
            # partially-flashed device. Log loudly on the transition so the
            # operator sees it in the journal exactly once per giveup event.
            if fail_count >= MAX_AUTO_RECOVERY_ATTEMPTS:
                if not state.get("gave_up_logged"):
                    log.error(
                        "Auto-recovery GIVING UP for %s (%s) after %d attempts. "
                        "Manual intervention required. State persists across server "
                        "restarts via the dfu_recovery_state dict (in-memory only — "
                        "a restart clears the cap, which is intentional: a fresh boot "
                        "is a deliberate operator action).",
                        device.id[:12],
                        device.device_name or "unknown",
                        fail_count,
                    )
                    state["gave_up_logged"] = True
                continue

            if fail_count > 0:
                # Exponential backoff: 1, 2, 4, 8 min (called every 60s)
                skip_calls = min(2 ** (fail_count - 1), 8)
                if state["backoff"] < skip_calls:
                    state["backoff"] += 1
                    continue
                state["backoff"] = 0

            log.info(
                "Auto-recovering DFU device %s (BLE: %s, attempt %d)...",
                device.id[:12],
                device.ble_address,
                fail_count + 1,
            )

            # Acquire per-device DFU lock — if manual flash is in progress,
            # skip this device rather than blocking the background loop.
            dfu_lock = self.get_dfu_lock(device.id)
            if dfu_lock.locked():
                log.info(
                    "DFU lock held for %s (manual flash in progress?) — skipping auto-recovery",
                    device.id[:12],
                )
                continue

            async with dfu_lock:
                self._dfu_recovery_active.add(device.id)
                self.pause_discovery()
                self.hold_reconnect(device.id, 600)
                # Stop the broadcaster: BLE DFU needs central GATT, and the
                # BCM43455 single radio refuses to register an advertisement
                # while we hold central GATT connections (verified live
                # 2026-05-16 via bluez `Failed (0x03)`). Reverse direction
                # (GATT central while advertising) is not directly observed
                # but the same radio constraint applies — don't risk it.
                broadcaster_was_running = (
                    self.broadcaster is not None and self.broadcaster.is_running
                )
                if broadcaster_was_running and self.broadcaster is not None:
                    try:
                        await self.broadcaster.stop()
                    except Exception:
                        log.exception(
                            "Auto-recovery: broadcaster stop failed; recovery may fail too"
                        )
                    # P3 guard mirroring the flash route: if the broadcaster
                    # didn't actually stop, refuse to enter BLE DFU. Mid-flash
                    # radio contention against the BCM43455 (advertising +
                    # GATT-central) produces "Failed (0x03)" and was the
                    # cart_inner brick mode. PR #140 review — auto-recovery
                    # was missing this guard that the flash routes have.
                    if self.broadcaster is not None and self.broadcaster.is_running:
                        log.error(
                            "Auto-recovery: broadcaster did not stop cleanly for %s — "
                            "refusing to enter BLE DFU; will retry next cycle",
                            device.id[:12],
                        )
                        state["fails"] = fail_count + 1
                        self._dfu_recovery_active.discard(device.id)
                        self.resume_discovery()
                        self.resume_reconnect(device.id)
                        continue
                try:
                    from ..firmware.ble_dfu import upload_ble_dfu
                    from ..firmware.compile import ensure_dfu_zip

                    dfu_zip = await asyncio.to_thread(ensure_dfu_zip, firmware_path)
                    assert device.ble_address is not None  # filtered above
                    # P5: hard 600s timeout. Mirrors the flash-route guard.
                    # Without this, a hung BLE DFU here would run until
                    # something else tears it down — which is exactly the
                    # destructive scenario that bricked cart_inner.
                    result = await asyncio.wait_for(
                        upload_ble_dfu(
                            app_ble_address=device.ble_address,
                            dfu_zip_path=dfu_zip,
                        ),
                        timeout=600.0,
                    )

                    if result.get("status") == "ok":
                        log.info("Auto-recovery succeeded for %s!", device.id[:12])
                        device.state = DeviceState.DISCONNECTED
                        self._dfu_recovery_state.pop(device.id, None)
                    else:
                        state["fails"] = fail_count + 1
                        log.warning(
                            "Auto-recovery failed for %s (attempt %d): %s",
                            device.id[:12],
                            fail_count + 1,
                            result.get("message", ""),
                        )
                except Exception as e:
                    state["fails"] = fail_count + 1
                    log.error("Auto-recovery error for %s: %s", device.id[:12], e)
                finally:
                    self._dfu_recovery_active.discard(device.id)
                    self.resume_discovery()
                    self.resume_reconnect(device.id)
                    if broadcaster_was_running and self.broadcaster is not None:
                        try:
                            await self.broadcaster.start()
                        except Exception:
                            log.exception(
                                "Auto-recovery: failed to restart broadcaster — fleet "
                                "commands will not reach BLE devices until the server "
                                "is restarted"
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
