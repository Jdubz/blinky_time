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
advertising interval — see ``_Advertisement.MinInterval`` /
``MaxInterval`` below for the values we declare (50-100 ms) and the
rationale.
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


# Backoff schedule for RegisterAdvertisement retries (exponential).
# Totals ~7.5 s before the final attempt re-raises. Module-level
# constant rather than per-call so it doesn't re-allocate on every
# `start()` invocation.
_BACKOFFS_S: tuple[float, ...] = (0.5, 1.0, 2.0, 4.0)


# D-Bus error types that indicate a transient "slot still busy" condition
# we should retry on. Anything else (adapter down, permission, missing
# service) will fail identically on every retry — re-raise immediately
# so the operator gets the real diagnostic without waiting through the
# backoff.
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

    # ── Advertising interval (PR #143 follow-up, May 18 research) ───────
    #
    # Units: BlueZ's ``LEAdvertisement1.MinInterval`` and ``MaxInterval``
    # are in MILLISECONDS per its D-Bus API spec. The numbers we return
    # below (50 and 100) are therefore 50 ms and 100 ms.
    #
    # Why we declare these explicitly:
    # The documented BlueZ default for ``LEAdvertisement1.MinInterval``
    # / ``MaxInterval`` is ~1280 ms — at that rate, an entire
    # ``COMMAND_REEMIT_HOLD_MS`` (250 ms) re-emit slot can elapse without
    # a single on-air packet, and a fresh-seq command can be missed
    # entirely. The empirical "~50 ms intervals" observation referenced
    # elsewhere in this module (older comment block in
    # ``FleetBroadcaster.broadcast_command``) was measured on a single
    # adapter / kernel combination; some kernels honour the documented
    # default, others clamp to the request, and a few ignore the property
    # altogether and emit at ~50 ms. Declaring the property is defensive
    # — it removes the kernel-dependent variability and pins us to a
    # known-good range.
    #
    # 20 ms is the BLE spec minimum for legacy non-connectable
    # broadcast. We don't go that low because (a) it eats radio time
    # the BCM43455 also needs for scan + GATT central, and (b) some
    # BlueZ kernels clamp the value silently. 50/100 ms gives us:
    #   - ~10-20 emissions per re-emit slot (we want ≥ 2 per slot to
    #     beat firmware single-slot rxBuffer drops)
    #   - Within nRF52840 firmware scan window: 50 ms interval / 50 ms
    #     window = >= one on-air landing per window every time
    #   - Modest power impact (broadcaster is always plugged in)
    #
    # **Caveat:** declaring the property is no guarantee BlueZ honours
    # it. Some kernel / mgmt-api versions silently clamp the value (see
    # bluez#314, bluez#833). When that happens we fall back to the
    # kernel's own default. Empirical verification on a new adapter is
    # the only ground truth; on the BCM43455 (May 2026) BlueZ honoured
    # the request and emissions landed inside the 50-100 ms window.
    @dbus_property(access=PropertyAccess.READ)
    def MinInterval(self) -> "u":  # type: ignore[name-defined]  # noqa: F821, UP037
        return 50

    @dbus_property(access=PropertyAccess.READ)
    def MaxInterval(self) -> "u":  # type: ignore[name-defined]  # noqa: F821, UP037
        return 100

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
        # Monotonic per-broadcaster command_id. Incremented ONCE per
        # call to ``broadcast_command()``; the same value rides every
        # one of the N re-emits of that logical command. Firmware uses
        # this to identify re-emits as "same logical command, apply
        # once" (BLE_FLEET_RELIABILITY_PLAN item #2). Starts at 0; the
        # first command bumps it to 1 before emit, so the value on the
        # wire is never 0 for a real command.
        self._command_id: int = 0
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
            # for the same message.
            #
            # call_register_advertisement is a dynamically-generated
            # method on the proxy interface (dbus-fast builds it from the
            # introspection XML), so mypy can't see it statically.
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

    # How long to keep each individual emit on-air before replacing it
    # with the next emit (or the no-op terminator). After the full
    # re-emit cycle, the AD payload becomes a no-op packet (type=0x00,
    # which the firmware's BleScanner::update() drops at its packet-type
    # switch). The no-op stays on-air until the next command. Two reasons
    # we don't want the real command lingering:
    #   1. If a device reboots, its scanner comes up and would re-execute
    #      the still-on-air command — a crash loop if that command is the
    #      one that crashed the device in the first place.
    #   2. Devices coming into range later would pick up an old command.
    #
    # Lower bound is the BlueZ advertising interval declared on
    # ``_Advertisement.MaxInterval`` (100 ms) — the slot needs at least
    # one advertising cycle to actually transmit on-air. 250 ms gives
    # ~2-5 advertising cycles per emit (within the 50-100 ms range BlueZ
    # is configured to use), which is enough for the firmware's
    # 100 ms scan interval / 50 ms window to catch each emit at least
    # once in expectation.
    COMMAND_REEMIT_HOLD_MS = 250

    # How many times to re-emit each command with a fresh sequence.
    # Rationale (PR #143 hardware-test on FPS Sweep + carts, May 18):
    # The firmware's BleScanner has a single-slot rxBuffer. If the slot
    # is busy when an advertisement arrives, the packet is silently
    # dropped (firmware diag: ``packets_rx=13`` / ``duped=4892`` /
    # ``dropped=21`` on cart_inner after 6h uptime — 13 unique accepted
    # vs the many hundreds of broadcasts that fired). The firmware
    # dedups by ``(source BD addr, sequence)`` — multiple emits with
    # the SAME seq inside the on-air window count as 1 chance.
    #
    # Re-emitting with FRESH sequences each time gives the firmware N
    # independent chances to catch the same payload. The firmware
    # APPLIES each accepted packet (gen/effect/save/load/set are all
    # idempotent), so applying the same command N times is harmless.
    #
    # 5 x 250ms = 1250ms total air time per command. At the firmware's
    # 100ms scan interval / 50ms window, the chance of all 5 hitting a
    # busy main loop drops to <1% under reasonable load.
    COMMAND_REEMIT_COUNT = 5

    async def broadcast_command(self, command: str) -> None:
        """Broadcast a serial command string to all listening devices.

        Emits the command as a manufacturer-data BLE advertisement
        ``COMMAND_REEMIT_COUNT`` times in succession with FRESH sequence
        numbers per emit, each held on-air for ``COMMAND_REEMIT_HOLD_MS``.
        After the last emit, replaces the advertisement with a no-op
        packet so the air falls silent.

        Re-emit redundancy + idempotency:
        Every re-emit of a single logical command carries the SAME
        ``command_id`` (assigned once at the top of this method) but a
        FRESH ``sequence``. The firmware uses ``sequence`` to dedup
        bus-level retransmissions of identical packets and
        ``command_id`` to short-circuit re-emits of the same logical
        command after the first one is applied. Without command_id the
        firmware would re-execute every fresh-seq emit
        (``COMMAND_REEMIT_COUNT`` times for every fleet command),
        which works for the idempotent gen/effect/save/load/set
        commands we have today but is fragile — any future command
        with side effects would multi-apply. See
        BLE_FLEET_RELIABILITY_PLAN item #2.

        Concurrency: the lock is held only while mutating the payload
        (~1 ms PropertiesChanged emit). The per-emit hold happens
        OUTSIDE the lock so a concurrent ``stop()`` or another
        broadcast can interleave. A concurrent broadcast of a different
        command WILL clobber the in-flight re-emit cycle (this is
        deliberate — the operator's most recent command takes
        priority); the firmware will receive whichever payload was on
        the air during its scan window.
        """
        if self._adv is None:
            raise RuntimeError("FleetBroadcaster not started")

        # Assign the command_id ONCE per logical command, before the
        # re-emit loop. Every re-emit below carries this same value;
        # the firmware uses it to identify them as one logical command.
        # Done outside the lock — the bump itself is atomic and we don't
        # care if a concurrent broadcaster bumps next; sequence-clobber
        # detection inside the loop handles the inter-command race.
        my_command_id = self._next_command_id()

        last_emit_seq = -1
        for emit_idx in range(self.COMMAND_REEMIT_COUNT):
            async with self._lock:
                if self._adv is None:
                    return  # stop() raced us
                my_seq = self._next_sequence()
                packet = _proto.build_command_v2_packet(
                    command, sequence=my_seq, command_id=my_command_id
                )
                self._adv._set_manufacturer_payload(_proto.COMPANY_ID, packet)
                self._last_command = command
                self._broadcasts_sent += 1
                last_emit_seq = my_seq
                log.info(
                    "Broadcast cmd seq=%d cmd_id=%d (emit %d/%d): %r",
                    my_seq,
                    my_command_id,
                    emit_idx + 1,
                    self.COMMAND_REEMIT_COUNT,
                    command,
                )
            # Hold on-air OUTSIDE the lock. Concurrent broadcasts can
            # acquire the lock during this sleep and clobber the payload;
            # the next iteration's lock re-acquire + sequence check
            # detects the clobber and stops re-emitting (let the newer
            # broadcast's re-emit cycle run instead).
            await asyncio.sleep(self.COMMAND_REEMIT_HOLD_MS / 1000.0)
            # If a concurrent broadcast advanced the sequence past our
            # last emit, abandon the rest of OUR re-emit cycle so we
            # don't fight the newer command.
            if self._sequence != last_emit_seq:
                return

        async with self._lock:
            # Drop back to a no-op packet (type=0x00). The firmware's
            # BleScanner routes by packet type and falls through to
            # packetsDropped++ for anything outside SETTINGS/SCENE/COMMAND.
            # Using a fresh sequence ensures the firmware re-processes
            # (and drops) the new packet rather than dedup'ing it against
            # the previous command's (source, seq).
            if self._adv is None:
                return  # stop() raced us
            # Gate on the sequence number captured at the LAST emit:
            # if a concurrent broadcast advanced the sequence past us,
            # let its own no-op terminator run instead of stomping it.
            if self._sequence != last_emit_seq:
                return
            noop_seq = self._next_sequence()
            noop = bytes((_proto.PROTOCOL_VERSION, 0x00, noop_seq, _proto.FRAGMENT_SINGLE))
            self._adv._set_manufacturer_payload(_proto.COMPANY_ID, noop)

    def _next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFF
        return self._sequence

    def _next_command_id(self) -> int:
        # uint16 rolling counter. Wraps after ~65k commands; at, say,
        # 10 cmd/sec that's ~109 minutes — well beyond any realistic
        # in-flight collision window with the firmware's per-source
        # last-id memory (the firmware compares for equality with the
        # PREVIOUS command_id, not membership in a set, so reuse only
        # collides if the very-previous command happens to share the
        # value).
        self._command_id = (self._command_id + 1) & 0xFFFF
        # Skip 0 on wrap — keep the on-wire value strictly non-zero so
        # the firmware can't confuse a real command_id with a default-
        # initialized cell. Cheap defensive guard.
        if self._command_id == 0:
            self._command_id = 1
        return self._command_id

    def status(self) -> dict[str, Any]:
        """Health snapshot for /api/fleet/status."""
        return {
            "running": self.is_running,
            "broadcasts_sent": self._broadcasts_sent,
            "sequence": self._sequence,
            "command_id": self._command_id,
            "last_command": self._last_command,
        }
