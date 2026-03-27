from __future__ import annotations

import asyncio
import contextlib
import logging

from ..transport.discovery import discover_serial_devices
from ..transport.serial_transport import SerialTransport
from .device import Device, DeviceState
from .protocol import DeviceProtocol

log = logging.getLogger(__name__)

DISCOVERY_INTERVAL_S = 10
RECONNECT_INTERVAL_S = 5


class FleetManager:
    """Manages all connected blinky devices across all transport types."""

    def __init__(self) -> None:
        self._devices: dict[str, Device] = {}  # keyed by device_id (serial number)
        self._discovery_task: asyncio.Task[None] | None = None
        self._running = False

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
        log.info(
            "Fleet manager started: %d devices connected",
            sum(1 for d in self._devices.values() if d.state == DeviceState.CONNECTED),
        )

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
        try:
            transport = SerialTransport(device.port)
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

    # ── Internal ──

    async def _discover_and_connect(self) -> None:
        """Discover serial devices and connect to any new ones."""
        discovered = discover_serial_devices()
        known_ids = set(self._devices.keys())

        for disc in discovered:
            device_id = disc.serial_number
            if not device_id:
                device_id = disc.port  # Fallback if no serial number

            if device_id in known_ids:
                # Update port if it changed (USB re-enumeration)
                existing = self._devices[device_id]
                if existing.port != disc.port:
                    log.info(
                        "Device %s port changed: %s → %s",
                        device_id[:12],
                        existing.port,
                        disc.port,
                    )
                    existing.port = disc.port
                continue

            # New device - connect
            transport = SerialTransport(disc.port)
            device = Device(
                device_id=device_id,
                port=disc.port,
                platform=disc.platform,
                transport=transport,
            )
            self._devices[device_id] = device
            try:
                await device.connect()
            except Exception as e:
                log.error(
                    "Failed to connect new device %s on %s: %s",
                    device_id[:12],
                    disc.port,
                    e,
                )

    async def _reconnect_disconnected(self) -> None:
        """Try to reconnect any devices in error/disconnected state."""
        for device in self._devices.values():
            if device.state in (DeviceState.DISCONNECTED, DeviceState.ERROR):
                try:
                    transport = SerialTransport(device.port)
                    device.transport = transport
                    device.protocol = DeviceProtocol(transport)
                    device.protocol.on_stream_line(device._route_stream_line)
                    device.protocol.on_raw_line(device._on_raw_line)
                    await device.connect()
                except Exception:
                    pass  # Will retry next cycle

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
