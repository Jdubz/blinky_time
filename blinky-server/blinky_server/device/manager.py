from __future__ import annotations

import asyncio
import contextlib
import logging

from ..transport.base import Transport
from ..transport.discovery import DiscoveredDevice, discover_all, discover_serial_devices
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
        host = disc.extra["host"]
        port = disc.extra.get("port", 3333)
        return WifiTransport(host, port)
    else:
        raise ValueError(f"Unknown transport type: {disc.transport_type}")


class FleetManager:
    """Manages all connected blinky devices across all transport types."""

    def __init__(
        self,
        enable_ble: bool = True,
        ble_timeout: float = 5.0,
        wifi_hosts: list[dict] | None = None,
    ) -> None:
        self._devices: dict[str, Device] = {}  # keyed by device_id
        self._discovery_task: asyncio.Task[None] | None = None
        self._running = False
        self._enable_ble = enable_ble
        self._ble_timeout = ble_timeout
        self._wifi_hosts = wifi_hosts or []
        # Track discovery metadata for reconnection
        self._device_discovery: dict[str, DiscoveredDevice] = {}

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

    async def release_device(self, device_id: str) -> bool:
        """Temporarily release a device (e.g., for firmware flashing)."""
        device = self.get_device(device_id)
        if not device:
            return False
        await device.disconnect()
        log.info("Released device %s on %s", device_id[:12], device.port)
        return True

    async def reconnect_device(self, device_id: str) -> bool:
        """Reconnect a previously released device."""
        device = self.get_device(device_id)
        if not device:
            return False
        if device.state == DeviceState.CONNECTED:
            return True

        disc = self._device_discovery.get(device_id)
        if not disc:
            log.error("No discovery info for device %s", device_id[:12])
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

    def add_wifi_host(self, host: str, port: int = 3333, device_id: str | None = None) -> None:
        """Add a WiFi device to the discovery list at runtime."""
        entry = {"host": host, "port": port}
        if device_id:
            entry["device_id"] = device_id
        self._wifi_hosts.append(entry)

    # ── Internal ──

    async def _discover_and_connect(self) -> None:
        """Discover devices across all transports and connect to new ones."""
        discovered = await discover_all(
            ble_scan=self._enable_ble,
            ble_timeout=self._ble_timeout,
            wifi_hosts=self._wifi_hosts,
        )
        known_ids = set(self._devices.keys())

        for disc in discovered:
            device_id = disc.device_id

            if device_id in known_ids:
                existing = self._devices[device_id]
                # Update address if it changed (USB re-enumeration)
                if existing.port != disc.address:
                    log.info(
                        "Device %s address changed: %s → %s",
                        device_id[:12], existing.port, disc.address,
                    )
                    existing.port = disc.address
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
                self._devices[device_id] = device
                self._device_discovery[device_id] = disc
                await device.connect()
            except Exception as e:
                log.error(
                    "Failed to connect %s %s on %s: %s",
                    disc.transport_type, device_id[:12], disc.address, e,
                )

        # Deduplicate: if a device is connected via both serial and BLE/WiFi,
        # prefer serial (faster, more reliable) and disconnect the wireless one.
        await self._deduplicate_transports()

    async def _deduplicate_transports(self) -> None:
        """Remove duplicate connections to the same physical device.

        If a device is connected via serial AND BLE/WiFi, keep the serial
        connection (faster, more reliable) and disconnect the wireless one.
        Detection: same device_type + device_name on different transports.
        """
        # Build a map of device identity → (device_id, transport_type)
        identity_map: dict[str, list[tuple[str, str]]] = {}
        for did, dev in self._devices.items():
            if dev.state != DeviceState.CONNECTED or not dev.device_type:
                continue
            # Identity: device_type + device_name (unique per physical device)
            identity = f"{dev.device_type}:{dev.device_name}"
            entry = (did, dev.transport.transport_type)
            identity_map.setdefault(identity, []).append(entry)

        # Find duplicates and remove wireless ones
        to_remove: list[str] = []
        for identity, entries in identity_map.items():
            if len(entries) <= 1:
                continue
            # Check if there's a serial connection
            serial_entries = [e for e in entries if e[1] == "serial"]
            wireless_entries = [e for e in entries if e[1] != "serial"]
            if serial_entries and wireless_entries:
                for did, ttype in wireless_entries:
                    log.info(
                        "Dedup: %s (%s) is duplicate of serial device, disconnecting %s",
                        identity, did[:12], ttype,
                    )
                    to_remove.append(did)

        for did in to_remove:
            dev = self._devices.get(did)
            if dev:
                await dev.disconnect()
                del self._devices[did]
                self._device_discovery.pop(did, None)

    async def _reconnect_disconnected(self) -> None:
        """Try to reconnect any devices in error/disconnected state."""
        for device_id, device in self._devices.items():
            if device.state not in (DeviceState.DISCONNECTED, DeviceState.ERROR):
                continue

            disc = self._device_discovery.get(device_id)
            if not disc:
                continue

            try:
                transport = _create_transport(disc)
                device.transport = transport
                device.protocol = DeviceProtocol(transport)
                device.protocol.on_stream_line(device._route_stream_line)
                device.protocol.on_raw_line(device._on_raw_line)
                await device.connect()
            except Exception as e:
                log.warning("Reconnect failed for %s: %s", device_id[:12], e)

    async def _background_loop(self) -> None:
        """Periodic discovery and reconnection."""
        while self._running:
            try:
                await asyncio.sleep(DISCOVERY_INTERVAL_S)
                await self._discover_and_connect()
                await self._reconnect_disconnected()
            except asyncio.CancelledError:
                break
            except Exception as e:
                log.error("Background loop error: %s", e)
