"""Fleet broadcaster — radio-style command channel over BLE advertising.

Registers an ``org.bluez.LEAdvertisement1`` object with BlueZ and updates
its ``ManufacturerData`` to push commands to every blinky device in range.
No connections, no per-device state, no scanner contention.

The firmware's ``BleScanner`` (passive observer, filters company 0xFFFF)
receives the manufacturer-data AD blob, validates the protocol header,
dedups by ``(source BD addr, sequence)``, and dispatches the payload to
``SerialConsole::handleCommand`` — the same code path serial commands
take. So broadcasting ``"gen fire"`` is exactly equivalent to typing
``gen fire`` into the device's serial console, just delivered to every
listening device at once.

Implementation note: instead of unregister/re-register on every command
(which would briefly stop advertising mid-cycle), we keep the
advertisement registered and emit ``PropertiesChanged`` on
``ManufacturerData``. BlueZ picks up the new payload on its next
advertising interval (~100 ms default).
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
from typing import Any

from dbus_fast import BusType, DBusError, Variant
from dbus_fast.aio import MessageBus
from dbus_fast.constants import PropertyAccess
from dbus_fast.service import ServiceInterface, dbus_property, method

from . import protocol as _proto

log = logging.getLogger(__name__)

BLUEZ_SERVICE = "org.bluez"
DEFAULT_ADAPTER = "/org/bluez/hci0"
ADV_OBJECT_PATH = "/com/blinkytime/fleet/advertisement0"
LE_ADV_MGR_IFACE = "org.bluez.LEAdvertisingManager1"
LE_ADV_IFACE = "org.bluez.LEAdvertisement1"


class _Advertisement(ServiceInterface):
    """D-Bus object implementing org.bluez.LEAdvertisement1.

    BlueZ reads ``ManufacturerData`` and the other annotated properties to
    build the advertising data on the wire. Mutating those properties +
    emitting PropertiesChanged is how we re-aim the broadcast at runtime.
    """

    def __init__(self) -> None:
        super().__init__(LE_ADV_IFACE)
        # Seed with a single-byte non-empty payload so BlueZ has something
        # to put on-air at registration time. The real packet shows up via
        # PropertiesChanged from broadcast_command(). A zero-length value
        # makes some BlueZ builds reject the advertisement with mgmt
        # status 0x03 (Invalid Params).
        self._manufacturer_data: dict[int, bytes] = {
            _proto.COMPANY_ID: bytes([_proto.PROTOCOL_VERSION])
        }
        # On Release(), BlueZ has removed our advertisement (revoked the slot,
        # adapter went down, etc.) Set by the manager so we can resurrect it.
        self._released_event = asyncio.Event()

    @method()
    def Release(self):  # noqa: ANN201, N802 — D-Bus method; annotations are signatures
        """Called by BlueZ when it removes this advertisement."""
        log.warning(
            "BlueZ released our fleet advertisement — slot lost; will re-register"
        )
        # Setting in a sync method called from D-Bus is fine; asyncio.Event
        # is thread-safe enough for the loop-affinity D-Bus uses internally.
        self._released_event.set()

    @dbus_property(access=PropertyAccess.READ)
    def Type(self) -> "s":  # type: ignore[name-defined,valid-type]  # noqa: N802
        # 'broadcast' = we don't accept GATT connects from anyone scanning us.
        # Devices stay passive scanners; nothing connects to nothing.
        return "broadcast"

    @dbus_property(access=PropertyAccess.READ)
    def ManufacturerData(self) -> "a{qv}":  # type: ignore[name-defined,valid-type]  # noqa: N802
        # BlueZ wants a Variant per value — wrap bytes as 'ay'.
        return {k: Variant("ay", v) for k, v in self._manufacturer_data.items()}

    # Internal helper used by FleetBroadcaster to swap the bytes BlueZ
    # serializes on its next interval. Emits PropertiesChanged so BlueZ
    # picks up the new payload without re-registration.
    def _set_manufacturer_payload(self, company_id: int, payload: bytes) -> None:
        self._manufacturer_data = {company_id: payload}
        # ServiceInterface.emit_properties_changed will publish the new value
        # on the object's PropertiesChanged signal. BlueZ subscribes to this.
        self.emit_properties_changed(
            {"ManufacturerData": {company_id: Variant("ay", payload)}}
        )


class FleetBroadcaster:
    """Server-side BLE advertiser. Owns the D-Bus connection + advertisement.

    Lifecycle:
        ``start()``  — connect to the system bus, register the advertisement
                       object with BlueZ. After this, every call to
                       ``broadcast_command()`` re-aims the broadcast.
        ``stop()``   — unregister + drop the bus connection.
    """

    def __init__(
        self,
        adapter_path: str = DEFAULT_ADAPTER,
        object_path: str = ADV_OBJECT_PATH,
    ) -> None:
        self._adapter_path = adapter_path
        self._object_path = object_path
        self._bus: MessageBus | None = None
        self._adv: _Advertisement | None = None
        self._sequence: int = 0
        self._broadcasts_sent: int = 0
        self._last_command: str | None = None
        # Serializes broadcast() calls; rapid back-to-back PropertiesChanged
        # emits can confuse BlueZ if they arrive mid-advertising-cycle.
        self._lock = asyncio.Lock()

    async def start(self) -> None:
        """Connect to BlueZ and register the advertisement slot."""
        if self._bus is not None:
            return
        # Skip when running without a real D-Bus (CI, dev shell): the system
        # bus address is absent and trying to connect would block forever.
        if not os.environ.get("DBUS_SYSTEM_BUS_ADDRESS") and not os.path.exists(
            "/run/dbus/system_bus_socket"
        ):
            raise RuntimeError(
                "FleetBroadcaster requires the system D-Bus; not present in this env"
            )

        bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
        adv = _Advertisement()
        bus.export(self._object_path, adv)

        # Tell BlueZ about the advertisement object. RegisterAdvertisement
        # returns when BlueZ has accepted the slot — failure here means we
        # never get on-air, so raise instead of swallowing.
        introspection = await bus.introspect(BLUEZ_SERVICE, self._adapter_path)
        proxy = bus.get_proxy_object(BLUEZ_SERVICE, self._adapter_path, introspection)
        mgr = proxy.get_interface(LE_ADV_MGR_IFACE)
        await mgr.call_register_advertisement(self._object_path, {})

        self._bus = bus
        self._adv = adv
        log.info("Fleet broadcaster registered with BlueZ (%s)", self._adapter_path)

    async def stop(self) -> None:
        """Unregister the advertisement + tear down the bus connection."""
        if self._bus is None:
            return
        bus = self._bus
        self._bus = None
        adv = self._adv
        self._adv = None
        try:
            introspection = await bus.introspect(BLUEZ_SERVICE, self._adapter_path)
            proxy = bus.get_proxy_object(BLUEZ_SERVICE, self._adapter_path, introspection)
            mgr = proxy.get_interface(LE_ADV_MGR_IFACE)
            with contextlib.suppress(DBusError):
                await mgr.call_unregister_advertisement(self._object_path)
        finally:
            if adv is not None:
                with contextlib.suppress(Exception):
                    bus.unexport(self._object_path)
            bus.disconnect()
            log.info("Fleet broadcaster unregistered")

    @property
    def is_running(self) -> bool:
        return self._bus is not None and self._adv is not None

    @property
    def broadcasts_sent(self) -> int:
        return self._broadcasts_sent

    @property
    def last_command(self) -> str | None:
        return self._last_command

    # How long to keep a real command on-air after broadcast_command(). After
    # this window, we replace the AD payload with a no-op packet (type=0x00,
    # which the firmware's BleScanner::update() drops at its packet-type
    # switch). The no-op stays on-air until the next command. Two reasons
    # we don't want the real command lingering:
    #   1. If a device reboots, its scanner comes up and would re-execute
    #      the still-on-air command — a crash loop if that command is the
    #      one that crashed the device in the first place.
    #   2. Devices coming into range later would pick up an old command.
    # 300 ms is long enough that the firmware's scan window (50 ms at 50%
    # duty) catches at least one full advertisement at BlueZ's ~1 Hz ad
    # interval, with comfortable margin.
    COMMAND_ONAIR_MS = 300

    async def broadcast_command(self, command: str) -> None:
        """Broadcast a serial command string to all listening devices.

        Sets the BLE advertisement's manufacturer-data to a COMMAND packet
        carrying ``command``, holds it on-air for ``COMMAND_ONAIR_MS``,
        then replaces it with a no-op packet so the air falls silent
        (broadcast-wise) until the next call.

        Each call advances the sequence number, so a same-text command
        sent twice still triggers the firmware (different (source, seq)
        tuple) — but holding the same packet on-air for 300 ms with one
        emit is reliable on its own; we don't re-emit during the window.
        Same-source duplicates while the packet is on-air are dropped by
        firmware dedup on (source BD addr, sequence).
        """
        if self._adv is None:
            raise RuntimeError("FleetBroadcaster not started")

        async with self._lock:
            seq = self._next_sequence()
            packet = _proto.build_packet(_proto.PacketType.COMMAND, command, seq)
            self._adv._set_manufacturer_payload(_proto.COMPANY_ID, packet)
            self._last_command = command
            self._broadcasts_sent += 1
            log.info("Broadcast cmd seq=%d: %r", seq, command)

            await asyncio.sleep(self.COMMAND_ONAIR_MS / 1000.0)

            # Drop back to a no-op packet (type=0x00). The firmware's
            # BleScanner routes by packet type and falls through to
            # packetsDropped++ for anything outside SETTINGS/SCENE/COMMAND.
            # Using a fresh sequence ensures the firmware re-processes
            # (and drops) the new packet rather than dedup'ing it against
            # the previous command's (source, seq).
            if self._adv is None:
                return  # stop() raced us
            noop_seq = self._next_sequence()
            noop = bytes((_proto.PROTOCOL_VERSION, 0x00, noop_seq, _proto.FRAGMENT_SINGLE))
            self._adv._set_manufacturer_payload(_proto.COMPANY_ID, noop)

    def _next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFF
        return self._sequence

    def status(self) -> dict[str, Any]:
        """Health snapshot for /api/fleet/status."""
        return {
            "running": self.is_running,
            "broadcasts_sent": self._broadcasts_sent,
            "sequence": self._sequence,
            "last_command": self._last_command,
        }
