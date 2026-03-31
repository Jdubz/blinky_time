from __future__ import annotations

import asyncio
import contextlib
import logging
import time as _time
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
        # Discovery pause counter — when > 0, background loop skips discovery/reconnect.
        # Used during BLE DFU to avoid BleakScanner conflicts (only one scan at a time).
        self._discovery_pause_count: int = 0
        # BLE addresses excluded from discovery because they were deduped with a
        # serial device. Prevents the thrashing loop where a deduped device is
        # immediately rediscovered and reconnected on every cycle.
        self._dedup_excluded: set[str] = set()

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
        """Discover devices and connect to all of them."""
        self._running = True
        if self._enable_ble:
            await cleanup_stale_ble_connections()
            await asyncio.sleep(2)  # Let devices re-advertise after disconnect
        await self._discover_and_connect()
        self._discovery_task = asyncio.create_task(self._background_loop())
        connected = sum(1 for d in self._devices.values() if d.state == DeviceState.CONNECTED)
        log.info("Fleet manager started: %d devices connected", connected)

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
        that duration (in-memory blackout). The caller should also acquire
        a serial lock file for cross-process coordination.
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
        """Run discovery + reconnect immediately (called by API endpoint)."""
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

            # Skip locked serial ports (e.g., firmware flashing in progress)
            if disc.transport_type == "serial":
                from ..transport.serial_lock import is_locked

                locked, _ = is_locked(disc.address)
                if locked:
                    log.debug("Skipping discovery connect for %s — port locked", device_id[:12])
                    continue

            # New device - connect
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
                    "Push firmware via POST /api/devices/%s/ota to recover.",
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
        """Clear dedup exclusions when serial transports are gone or disconnected.

        Must run BEFORE discovery so BLE can take over for lost serial devices.
        Clears when: any serial device is disconnected/error, OR no serial
        devices exist at all (e.g., all unplugged and removed by failure limit).
        """
        if not self._dedup_excluded:
            return
        has_connected_serial = any(
            d.transport.transport_type == "serial" and d.state == DeviceState.CONNECTED
            for d in self._devices.values()
        )
        if not has_connected_serial:
            log.info(
                "No connected serial devices — clearing %d dedup exclusions",
                len(self._dedup_excluded),
            )
            self._dedup_excluded.clear()

    async def _reconnect_disconnected(self) -> None:
        """Try to reconnect any devices in error/disconnected state.

        Uses exponential backoff: after each failure, waits longer before
        retrying (10s, 20s, 40s, 80s, 160s, capped at 5 min). Resets on
        successful connect or when device is released/reconnected manually.
        After 10 consecutive failures, removes the device from the fleet.
        """
        from ..transport.serial_lock import is_locked

        to_remove_after: list[str] = []

        for device_id, device in self._devices.items():
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

            # Exponential backoff for repeated failures (BLE devices that
            # consistently fail waste 20s per attempt on timeout).
            # After 10 consecutive failures, give up entirely — the device
            # is likely not a Blinky or is permanently out of range.
            fail_count = self._reconnect_failures.get(device_id, 0)
            if fail_count >= 10:
                log.info(
                    "Giving up on %s after %d failures — removing from fleet",
                    device_id[:12],
                    fail_count,
                )
                to_remove_after.append(device_id)
                continue
            if fail_count > 0:
                # Backoff: skip N-1 discovery cycles (10s each) per failure,
                # doubling each time, capped at 30 cycles (5 min)
                skip_cycles = min(2 ** (fail_count - 1), 30)
                backoff_key = f"_backoff_skip_{device_id}"
                counter = getattr(self, backoff_key, 0)
                if counter < skip_cycles:
                    setattr(self, backoff_key, counter + 1)
                    continue
                setattr(self, backoff_key, 0)

            # Check serial port lock file (cross-process coordination)
            if disc.transport_type == "serial":
                locked, holder = is_locked(disc.address)
                if locked:
                    log.debug(
                        "Skipping reconnect for %s — port locked by %s",
                        device_id[:12],
                        holder.get("tool") if holder else "unknown",
                    )
                    continue

            try:
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
            if dev:
                # Best-effort cleanup: ensure underlying transport is closed
                with contextlib.suppress(Exception):
                    await dev.disconnect()
                log.info("Removed unreachable device %s from fleet", did[:12])

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

    async def _background_loop(self) -> None:
        """Periodic discovery, reconnection, and BLE liveness checks."""
        cycle = 0
        while self._running:
            try:
                await asyncio.sleep(DISCOVERY_INTERVAL_S)
                cycle += 1
                if self._discovery_pause_count > 0:
                    continue  # Skip discovery while BLE DFU or other ops in progress
                await self._discover_and_connect()
                await self._reconnect_disconnected()
                # Liveness check every 3rd cycle (~30s) for all transports
                if cycle % 3 == 0:
                    await self._check_liveness()
            except asyncio.CancelledError:
                break
            except Exception as e:
                log.error("Background loop error: %s", e)
