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


# D-Bus error types that indicate a transient "slot still busy" condition
# we should retry on. Anything else (adapter down, permission, missing
# service) will fail identically on every retry — re-raise immediately
# so the operator gets the real diagnostic without waiting through the
# backoff. PR 142 review.
_TRANSIENT_REGISTER_ERROR_TYPES = frozenset(
    {
        "org.bluez.Error.Failed",  # generic, what BlueZ returns for the slot-still-held case
        "org.bluez.Error.AlreadyExists",  # slot literally still registered
    }
)


def _is_transient_register_error(exc: DBusError) -> bool:
    """True if ``exc`` looks like a retryable RegisterAdvertisement failure.

    Matches by D-Bus error type (the canonical name like
    ``org.bluez.Error.Failed``) rather than text, so locale or BlueZ
    version changes to the human-readable string don't break us. Older
    dbus-fast versions don't always populate ``.type``; fall back to a
    substring check on the message as a last resort.
    """
    err_type = getattr(exc, "type", "") or ""
    if err_type in _TRANSIENT_REGISTER_ERROR_TYPES:
        return True
    # Fallback for libraries that don't expose .type or use the older
    # generic DBusError wrapping. Keep the substring narrow.
    msg = str(exc).lower()
    return "failed to register advertisement" in msg or "alreadyexists" in msg


class _Advertisement(ServiceInterface):
    """D-Bus object implementing org.bluez.LEAdvertisement1.

    BlueZ reads ``ManufacturerData`` and the other annotated properties to
    build the advertising data on the wire. Mutating those properties +
    emitting PropertiesChanged is how we re-aim the broadcast at runtime.
    """

    def __init__(self, loop: asyncio.AbstractEventLoop) -> None:
        super().__init__(LE_ADV_IFACE)
        # Seed with a single-byte non-empty payload so BlueZ has something
        # to put on-air at registration time. The real packet shows up via
        # PropertiesChanged from broadcast_command(). A zero-length value
        # makes some BlueZ builds reject the advertisement with mgmt
        # status 0x03 (Invalid Params).
        self._manufacturer_data: dict[int, bytes] = {
            _proto.COMPANY_ID: bytes([_proto.PROTOCOL_VERSION])
        }
        # On Release(), BlueZ has removed our advertisement (revoked the
        # slot, adapter went down, etc.) Monitored by FleetBroadcaster's
        # _release_monitor task.
        #
        # Captured loop reference: D-Bus method handlers can fire on a
        # dispatch thread that is NOT the asyncio loop thread (dbus-fast
        # detail). Event.set() is not thread-safe, so we schedule it via
        # call_soon_threadsafe on the captured loop. PR #140 review —
        # this was the silent-failure path that prompted the audit.
        #
        # `loop` is required (not defaulted to asyncio.get_event_loop())
        # because get_event_loop() is deprecated in 3.10+ when there is
        # no running loop. start() always passes the running loop.
        self._loop = loop
        self._released_event = asyncio.Event()

    @method()
    def Release(self):  # type: ignore[no-untyped-def]
        """Called by BlueZ when it removes this advertisement.

        Intentionally no return annotation: dbus-fast reads the return
        annotation as a D-Bus signature string; the absence of an
        annotation means "void return" which is what BlueZ expects. A
        Python ``-> None`` would set ``__annotations__['return'] = None``
        and dbus-fast would TypeError at decorator time.

        Method name capitalization matches the D-Bus interface
        (``org.bluez.LEAdvertisement1.Release``), not Python style.

        Thread safety: dbus-fast may dispatch this on a non-asyncio
        thread, so the Event.set() call MUST go through
        ``call_soon_threadsafe`` on the captured event loop. A direct
        ``set()`` from a foreign thread is a silent-failure race
        (PR #140 review).
        """
        log.warning("BlueZ released our fleet advertisement — slot lost; will re-register")
        self._loop.call_soon_threadsafe(self._released_event.set)

    # The annotations on the two methods below are D-Bus type signatures,
    # NOT Python type annotations — dbus-next reads __annotations__['return']
    # and uses the string verbatim as the D-Bus signature. "s" is a string,
    # "a{qv}" is a dict<uint16, variant>. Ruff (F821/F722/UP037) cannot
    # know these are signatures, so we suppress on each line; mypy
    # (valid-type/name-defined) is similarly told to ignore.
    @dbus_property(access=PropertyAccess.READ)
    def Type(self) -> "s":  # type: ignore[name-defined]  # noqa: F821, UP037
        # 'broadcast' = we don't accept GATT connects from anyone scanning us.
        # Devices stay passive scanners; nothing connects to nothing.
        return "broadcast"

    @dbus_property(access=PropertyAccess.READ)
    def ManufacturerData(self) -> "a{qv}":  # type: ignore[valid-type]  # noqa: F722
        # BlueZ wants a Variant per value — wrap bytes as 'ay'.
        return {k: Variant("ay", v) for k, v in self._manufacturer_data.items()}

    # Internal helper used by FleetBroadcaster to swap the bytes BlueZ
    # serializes on its next interval. Emits PropertiesChanged so BlueZ
    # picks up the new payload without re-registration.
    def _set_manufacturer_payload(self, company_id: int, payload: bytes) -> None:
        self._manufacturer_data = {company_id: payload}
        # ServiceInterface.emit_properties_changed will publish the new value
        # on the object's PropertiesChanged signal. BlueZ subscribes to this.
        self.emit_properties_changed({"ManufacturerData": {company_id: Variant("ay", payload)}})


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
        # Serializes start()/stop() so concurrent calls (e.g. fleet restart
        # racing with auto-recovery) don't double-register the advertisement
        # with BlueZ. PR #140 review (TOCTOU race on _bus is not None).
        self._lifecycle_lock = asyncio.Lock()
        # Watchdog: monitors _adv._released_event so we don't silently
        # broadcast into a slot BlueZ has revoked (adapter bounce, manager
        # reset, slot-limit exceeded). PR #140 review item — fail-loud rule.
        self._release_monitor: asyncio.Task[None] | None = None

    async def start(self) -> None:
        """Connect to BlueZ and register the advertisement slot."""
        async with self._lifecycle_lock:
            if self._bus is not None:
                return
            # Skip when running without a real D-Bus (CI, dev shell): the
            # system bus address is absent and trying to connect would block
            # forever.
            if not os.environ.get("DBUS_SYSTEM_BUS_ADDRESS") and not os.path.exists(
                "/run/dbus/system_bus_socket"
            ):
                raise RuntimeError(
                    "FleetBroadcaster requires the system D-Bus; not present in this env"
                )

            bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
            # Pass the running loop explicitly so Release() (which may
            # fire on a non-asyncio thread) can schedule its Event.set
            # back onto the right loop.
            adv = _Advertisement(loop=asyncio.get_running_loop())
            bus.export(self._object_path, adv)

            # Tell BlueZ about the advertisement object. RegisterAdvertisement
            # returns when BlueZ has accepted the slot — failure here means we
            # never get on-air, so raise instead of swallowing.
            introspection = await bus.introspect(BLUEZ_SERVICE, self._adapter_path)
            proxy = bus.get_proxy_object(BLUEZ_SERVICE, self._adapter_path, introspection)
            mgr = proxy.get_interface(LE_ADV_MGR_IFACE)

            # Retry on TRANSIENT DBusError: when blinky-server restarts
            # back-to-back, BlueZ can still be holding the previous
            # instance's advertisement slot for a brief window after the
            # old client connection dropped. The next register call sees
            # "org.bluez.Error.Failed" or "org.bluez.Error.AlreadyExists"
            # until BlueZ notices the old registration is dead.
            # ~7.5 s of exponential backoff is usually enough.
            #
            # Non-transient errors (adapter down, permission denied, bad
            # path, BlueZ service unknown) will fail identically on every
            # retry — re-raise immediately so the operator sees the real
            # cause in <1 s instead of waiting through 7.5 s of backoff
            # for the same message. PR 142 review: Copilot flagged the
            # over-broad catch + the sentinel-None-as-final-iteration
            # pattern; this rewrite addresses both.
            #
            # call_register_advertisement is a dynamically-generated
            # method on the proxy interface (dbus-fast builds it from the
            # introspection XML), so mypy can't see it statically.
            _BACKOFFS_S = (0.5, 1.0, 2.0, 4.0)  # 4 retries → ~7.5 s total
            for attempt in range(1, len(_BACKOFFS_S) + 2):  # 1..5
                try:
                    await mgr.call_register_advertisement(  # type: ignore[attr-defined]
                        self._object_path, {}
                    )
                    break
                except DBusError as exc:
                    if not _is_transient_register_error(exc):
                        log.error(
                            "RegisterAdvertisement failed with non-transient "
                            "error (%s) — not retrying: %s",
                            getattr(exc, "type", "?"),
                            exc,
                        )
                        raise
                    if attempt > len(_BACKOFFS_S):  # final attempt exhausted
                        log.error(
                            "RegisterAdvertisement failed after %d attempts: %s",
                            attempt,
                            exc,
                        )
                        raise
                    backoff = _BACKOFFS_S[attempt - 1]
                    log.warning(
                        "RegisterAdvertisement attempt %d failed (%s) — retrying in %.1fs",
                        attempt,
                        exc,
                        backoff,
                    )
                    await asyncio.sleep(backoff)

            self._bus = bus
            self._adv = adv
            self._release_monitor = asyncio.create_task(
                self._monitor_release(adv), name="ble-broadcaster-release-monitor"
            )
            log.info("Fleet broadcaster registered with BlueZ (%s)", self._adapter_path)

    async def stop(self) -> None:
        """Unregister the advertisement + tear down the bus connection."""
        async with self._lifecycle_lock:
            release_monitor = self._release_monitor
            self._release_monitor = None
            await self._teardown_locked()

        # Cancel the release-monitor outside the lock so a release-firing
        # monitor that ALSO triggers teardown doesn't end up awaiting
        # itself. Skip cancellation when stop() is called from within the
        # monitor task (current_task == release_monitor).
        if (
            release_monitor is not None
            and not release_monitor.done()
            and release_monitor is not asyncio.current_task()
        ):
            release_monitor.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await release_monitor

    async def _teardown_locked(self) -> None:
        """Inner teardown — caller MUST hold _lifecycle_lock.

        Split out from stop() so the release-monitor can tear the bus
        down without recursively cancelling itself.
        """
        if self._bus is None:
            return
        bus = self._bus
        adv = self._adv

        # Unregister + tear down BEFORE nulling our state. If unregister
        # raises, the slot is still registered in BlueZ and a subsequent
        # start() would fail with AlreadyExists. PR #140 review.
        try:
            introspection = await bus.introspect(BLUEZ_SERVICE, self._adapter_path)
            proxy = bus.get_proxy_object(BLUEZ_SERVICE, self._adapter_path, introspection)
            mgr = proxy.get_interface(LE_ADV_MGR_IFACE)
            with contextlib.suppress(DBusError):
                # Dynamically-generated method on the proxy interface
                # (see note in start() above).
                await mgr.call_unregister_advertisement(self._object_path)  # type: ignore[attr-defined]
        finally:
            if adv is not None:
                with contextlib.suppress(Exception):
                    bus.unexport(self._object_path)
            bus.disconnect()
            self._bus = None
            self._adv = None
            log.info("Fleet broadcaster unregistered")

    async def _monitor_release(self, adv: _Advertisement) -> None:
        """Watch for BlueZ revoking our advertisement slot.

        When BlueZ calls Release() (adapter went down, manager reset,
        advertisement-slot limit exceeded, etc.), our slot is gone but
        `_bus` and `_adv` are still set — `is_running` would lie and
        `broadcast_command()` would emit PropertiesChanged into a dead
        slot, silently dropping fleet commands.

        On release: tear down (so is_running goes False), log at ERROR
        with operator-visible guidance. A future enhancement could
        attempt re-registration here; for now we surface the failure
        loudly so the operator can decide whether to restart the
        service. PR #140 review — closes the silent-failure path.
        """
        try:
            await adv._released_event.wait()
        except asyncio.CancelledError:
            return
        log.error(
            "BlueZ revoked the fleet advertisement slot — fleet commands will "
            "stop reaching devices until restart. Investigate adapter / "
            "advertisement-slot capacity, then restart blinky-server.",
        )
        # Tear down so is_running correctly returns False. Acquire the
        # lifecycle lock directly and call _teardown_locked — calling
        # stop() from here would deadlock on the cancel-and-await of self.
        async with self._lifecycle_lock:
            self._release_monitor = None
            with contextlib.suppress(Exception):
                await self._teardown_locked()

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
    #
    # Sized for reliable single-packet delivery. The firmware scanner runs
    # at 100ms interval / 50ms window (50% duty); BlueZ on hci0 emits at
    # ~50ms intervals empirically (measured 2026-05-17: ~20 retransmits/sec
    # observed on both cart devices). 800ms gives ~16 retransmits per
    # broadcast, of which the scanner can catch ~8 — overwhelmingly enough
    # for one to land.
    #
    # The previous 3000ms value compensated for scene-apply firing 4
    # separate broadcasts back-to-back (gen / effect / huespeed / hueshift)
    # which had a measured ~37% per-packet capture rate. Scene apply is
    # now a single `scene` broadcast (server scene_to_commands +
    # firmware handleSceneCommand, b164), so the long window is no longer
    # needed and was hurting feel: a 4-command scene took ~12s end-to-end.
    # At 800ms the single scene packet lands in <1s.
    #
    # Firmware dedups subsequent same-(source, seq) packets, so reception
    # of multiple retransmits within the window cannot cause re-execution.
    COMMAND_ONAIR_MS = 800

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

        Concurrency model: the lock is held only while mutating the
        advertisement payload (a ~1 ms PropertiesChanged emit). The 3 s
        on-air sleep happens OUTSIDE the lock so a concurrent stop()
        or another broadcast doesn't have to wait the full window
        (PR #140 review). Same-payload re-aim from a second concurrent
        broadcast is fine — the firmware dedups by (source, seq).
        """
        if self._adv is None:
            raise RuntimeError("FleetBroadcaster not started")

        async with self._lock:
            my_seq = self._next_sequence()
            packet = _proto.build_packet(_proto.PacketType.COMMAND, command, my_seq)
            self._adv._set_manufacturer_payload(_proto.COMPANY_ID, packet)
            self._last_command = command
            self._broadcasts_sent += 1
            log.info("Broadcast cmd seq=%d: %r", my_seq, command)

        # Hold on-air OUTSIDE the lock. Concurrent broadcasts will
        # acquire the lock and re-aim the payload; this caller's
        # on-air timer just expires and falls through to the no-op
        # re-arm below.
        await asyncio.sleep(self.COMMAND_ONAIR_MS / 1000.0)

        async with self._lock:
            # Drop back to a no-op packet (type=0x00). The firmware's
            # BleScanner routes by packet type and falls through to
            # packetsDropped++ for anything outside SETTINGS/SCENE/COMMAND.
            # Using a fresh sequence ensures the firmware re-processes
            # (and drops) the new packet rather than dedup'ing it against
            # the previous command's (source, seq).
            if self._adv is None:
                return  # stop() raced us
            # Gate on the sequence number captured at broadcast time, NOT
            # on the command string. Two back-to-back identical commands
            # advance the sequence to different values; comparing strings
            # would let the FIRST wake-up kill the SECOND broadcast's
            # still-live on-air window (PR #140 review — verified would
            # bite scenes that send `set foo X` then `set foo Y`).
            if self._sequence != my_seq:
                return
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
