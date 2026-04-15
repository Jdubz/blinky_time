from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import time as _time
from pathlib import Path
from typing import Any

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
        self._dfu_recovery_state: dict[str, dict[str, int]] = {}
        # Per-device set tracking which devices have active DFU recovery.
        # Prevents the background loop from starting a second recovery on a
        # device that's already being recovered (e.g., by a manual flash).
        self._dfu_recovery_active: set[str] = set()
        # Per-device DFU locks: prevent auto-recovery and manual flash from
        # running concurrently on the same device. Lock is acquired before any
        # DFU operation and held for its entire duration.
        self._dfu_locks: dict[str, asyncio.Lock] = {}

    @property
    def devices(self) -> dict[str, Device]:
        return self._devices

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

        All device connections (serial + BLE) happen in the background loop.
        The API is available immediately — devices connect asynchronously.
        """
        self._running = True
        self._discovery_task = asyncio.create_task(self._background_loop())
        log.info("Fleet manager started (devices connecting in background)")

    async def stop(self) -> None:
        """Disconnect all devices and stop background tasks."""
        self._running = False
        if self._discovery_task:
            self._discovery_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._discovery_task
        for device in self._devices.values():
            await device.disconnect()
        self._devices.clear()
        self._device_discovery.clear()
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
        """Send a command to all connected devices."""
        results: dict[str, str] = {}
        tasks: list[tuple[str, asyncio.Task[str]]] = []
        for device in self._devices.values():
            if device.state == DeviceState.CONNECTED:
                task = asyncio.create_task(device.protocol.send_command(command))
                tasks.append((device.id, task))
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
        """Discover devices across all transports and connect to new ones."""
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

            # Skip BLE devices that were previously deduped with a serial device.
            # Without this, deduped devices get rediscovered immediately and thrash
            # in a connect → dedup → disconnect → rediscover loop.
            if device_id in self._dedup_excluded:
                continue

            # New device — connect.
            # No file-based serial lock check needed: the server is the sole serial
            # consumer. During firmware flashing, upload routes call pause_discovery()
            # + hold_reconnect() to prevent contention with the uf2_upload subprocess.
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
            thread = device.transport._reader_thread
            if thread is not None and not thread.is_alive():
                log.warning(
                    "Serial reader thread dead for %s (%s) — forcing disconnect",
                    device.id[:12],
                    device.port,
                )
                device.state = DeviceState.DISCONNECTED
                device.transport._connected = False

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

    # Persistent state file for recovery firmware path (survives server restarts)
    _RECOVERY_FIRMWARE_STATE = Path("/tmp/blinky-recovery-firmware.path")

    def set_recovery_firmware(self, firmware_path: str) -> None:
        """Set the firmware path used for automatic DFU recovery.

        When set, the fleet manager will automatically attempt BLE DFU on
        devices discovered in DFU_RECOVERY state (stuck in bootloader after
        a failed update). This eliminates the need for operator intervention.

        Persisted to /tmp so it survives server restarts (but not reboots,
        which is fine — a reboot means the firmware file in /tmp is gone too).
        """
        self._recovery_firmware_path = firmware_path
        with contextlib.suppress(OSError):
            self._RECOVERY_FIRMWARE_STATE.write_text(firmware_path)
        log.info("Recovery firmware set: %s", firmware_path)

    def _load_recovery_firmware(self) -> str | None:
        """Load persisted recovery firmware path from state file."""
        try:
            path = self._RECOVERY_FIRMWARE_STATE.read_text().strip()
            if path and Path(path).is_file():
                return path
        except OSError:
            pass
        return None

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
            firmware_path = self._load_recovery_firmware()
            if firmware_path:
                self._recovery_firmware_path = firmware_path
        if not firmware_path:
            return

        if not os.path.isfile(firmware_path):
            return

        dfu_devices = [
            d
            for d in self._devices.values()
            if d.state == DeviceState.DFU_RECOVERY and d.ble_address
        ]
        if not dfu_devices:
            return

        for device in dfu_devices:
            state = self._dfu_recovery_state.setdefault(device.id, {"fails": 0, "backoff": 0})
            fail_count = state["fails"]

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
                try:
                    from ..firmware.ble_dfu import upload_ble_dfu
                    from ..firmware.compile import ensure_dfu_zip

                    dfu_zip = await asyncio.to_thread(ensure_dfu_zip, firmware_path)
                    assert device.ble_address is not None  # filtered above
                    result = await upload_ble_dfu(
                        app_ble_address=device.ble_address,
                        dfu_zip_path=dfu_zip,
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

    async def _background_loop(self) -> None:
        """Periodic discovery, reconnection, liveness checks, and DFU recovery."""
        cycle = 0
        # BLE cleanup on first iteration (moved from start() to avoid blocking API)
        if self._enable_ble:
            try:
                await cleanup_stale_ble_connections()
                await asyncio.sleep(2)
            except Exception as e:
                log.warning("BLE cleanup failed (non-fatal): %s", e)
        while self._running:
            try:
                await asyncio.sleep(DISCOVERY_INTERVAL_S)
                cycle += 1
                if self._discovery_pause_count > 0:
                    continue  # Skip discovery while BLE DFU or other ops in progress
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
            except asyncio.CancelledError:
                break
            except Exception as e:
                log.error("Background loop error: %s", e)
