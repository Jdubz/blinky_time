"""Tests for BLE fleet management features.

Covers: fleet status/discover/settings endpoints, dedup exclusion logic,
failure limit, serial liveness, release hold fix, device info population.
"""

from __future__ import annotations

import pytest
from httpx import AsyncClient

from blinky_server.device.device import Device, DeviceState
from blinky_server.device.manager import FleetManager

from .mock_transport import MockTransport

# ── Helpers ──


def _fast_broadcaster(monkeypatch: pytest.MonkeyPatch) -> None:
    """Zero the broadcaster's per-emit hold so tests finish in ms, not s.

    Patches the CLASS attribute via ``monkeypatch.setattr`` (PR #144
    review item 7). The earlier pattern of mutating the instance
    attribute (``bc.COMMAND_REEMIT_HOLD_MS = 0.0``) worked but was
    fragile: ``broadcast_command`` reads via ``self.`` which today
    resolves to the class attribute (so the instance shadow took
    effect), but a refactor to read via ``FleetBroadcaster.<attr>``
    would silently revert each test to the production 250 ms — adding
    ~7 s per call and quietly breaking the test contract. Patching the
    class survives that refactor, and ``monkeypatch`` undoes the
    change at end-of-test automatically.
    """
    from blinky_server.ble.advertiser import FleetBroadcaster

    monkeypatch.setattr(FleetBroadcaster, "COMMAND_REEMIT_HOLD_MS", 0.0)


async def _make_fleet_with_sn() -> FleetManager:
    """Create a fleet with devices that have hardware_sn set (for dedup tests)."""
    fleet = FleetManager(enable_ble=False, enable_serial=False)

    for i, (did, sn, ttype) in enumerate(
        [
            ("SERIAL_001", "AAAA1111", "serial"),
            ("SERIAL_002", "BBBB2222", "serial"),
        ]
    ):
        info = {
            "version": "1.0.1",
            "sn": sn,
            "ble": f"AA:BB:CC:DD:EE:{i:02X}",
            "device": {
                "id": "long_tube_v1",
                "name": "Long Tube",
                "width": 4,
                "height": 60,
                "leds": 240,
                "configured": True,
            },
        }
        transport = MockTransport(device_info=info, transport_type=ttype)
        device = Device(
            device_id=did,
            port=f"/dev/ttyTEST{i}",
            platform="nrf52840",
            transport=transport,  # type: ignore[arg-type]
        )
        await device.connect()
        fleet._devices[device.id] = device

    return fleet


# ── Fleet Status Endpoint ──


async def test_fleet_status(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/fleet/status")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total"] == 2
    assert data["connected"] == 2
    assert "by_state" in data
    assert "by_transport" in data
    assert data["by_state"]["connected"] == 2
    assert len(data["devices"]) == 2


async def test_fleet_status_shows_disconnected(api_client: AsyncClient) -> None:
    # Release one device
    await api_client.post("/api/devices/MOCK_DEVICE_000/release")

    resp = await api_client.get("/api/fleet/status")
    data = resp.json()
    assert data["connected"] == 1
    assert data["by_state"].get("disconnected", 0) == 1


async def test_fleet_status_device_health(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/fleet/status")
    data = resp.json()
    device = data["devices"][0]
    assert "id" in device
    assert "transport" in device
    assert "state" in device
    assert "rssi" in device
    assert "last_seen_ago" in device
    assert "version" in device


# ── Fleet Discover Endpoint ──


async def test_fleet_discover(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/fleet/discover")
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "ok"
    assert "total" in data
    assert "new_devices" in data
    assert isinstance(data["new_devices"], list)


# ── Fleet Settings Endpoints ──


async def test_fleet_save_settings(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/fleet/settings/save")
    assert resp.status_code == 200
    results = resp.json()
    assert len(results) == 2
    assert all("OK" in v for v in results.values())


async def test_fleet_load_settings(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/fleet/settings/load")
    assert resp.status_code == 200
    results = resp.json()
    assert len(results) == 2
    assert all("OK" in v for v in results.values())


async def test_fleet_restore_defaults(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/fleet/settings/defaults")
    assert resp.status_code == 200
    results = resp.json()
    assert len(results) == 2
    assert all("OK" in v for v in results.values())


# ── Dedup Exclusion Logic ──


async def test_dedup_only_by_hardware_sn() -> None:
    """Devices with same device_type but different hardware_sn should NOT be deduped."""
    fleet = await _make_fleet_with_sn()

    # Add a "BLE" device with DIFFERENT hardware_sn but SAME device_type
    ble_info = {
        "version": "1.0.1",
        "sn": "CCCC3333",  # Different hardware_sn
        "ble": "FF:FF:FF:FF:FF:01",
        "device": {
            "id": "long_tube_v1",  # Same device type
            "name": "Long Tube",
            "width": 4,
            "height": 60,
            "leds": 240,
            "configured": True,
        },
    }
    ble_transport = MockTransport(device_info=ble_info)
    ble_device = Device(
        device_id="BLE_DEVICE_001",
        port="FF:FF:FF:FF:FF:01",
        platform="nrf52840",
        transport=ble_transport,  # type: ignore[arg-type]
    )
    await ble_device.connect()
    fleet._devices[ble_device.id] = ble_device

    # Run dedup — BLE device should NOT be removed (different hardware_sn)
    await fleet._deduplicate_transports()

    assert "BLE_DEVICE_001" in fleet._devices
    assert fleet._devices["BLE_DEVICE_001"].state == DeviceState.CONNECTED

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_dedup_exclusion_set() -> None:
    """Deduped BLE addresses should be tracked in _dedup_excluded."""
    fleet = await _make_fleet_with_sn()

    # Add a BLE device with SAME hardware_sn as serial device
    ble_info = {
        "version": "1.0.1",
        "sn": "AAAA1111",  # Same as SERIAL_001
        "device": {"configured": False, "safeMode": True},
    }
    ble_transport = MockTransport(device_info=ble_info)
    ble_device = Device(
        device_id="BLE_DUP",
        port="AA:BB:CC:DD:EE:FF",
        platform="nrf52840",
        transport=ble_transport,  # type: ignore[arg-type]
    )
    await ble_device.connect()
    fleet._devices[ble_device.id] = ble_device

    # Run dedup
    await fleet._deduplicate_transports()

    # BLE device should be removed and added to exclusion set
    assert "BLE_DUP" not in fleet._devices
    assert "BLE_DUP" in fleet._dedup_excluded

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_dedup_exclusion_persists_when_serial_disconnected() -> None:
    """Exclusions should persist even when serial devices disconnect.

    The old behavior cleared exclusions on serial disconnect, causing a
    BLE thrashing loop (connect → dedup → disconnect → rediscover).
    Exclusions now only clear when serial devices are REMOVED from the
    fleet entirely (failure limit exceeded or manual release).
    """
    fleet = await _make_fleet_with_sn()
    fleet._dedup_excluded.add("BLE_ADDR_001")
    fleet._dedup_excluded.add("BLE_ADDR_002")

    # Disconnect ONE serial device — exclusions should persist
    serial_dev1 = fleet.get_device("SERIAL_001")
    assert serial_dev1 is not None
    await serial_dev1.disconnect()
    fleet._refresh_dedup_exclusions()
    assert len(fleet._dedup_excluded) == 2  # Still excluded

    # Disconnect ALL serial devices — exclusions STILL persist (devices in fleet)
    serial_dev2 = fleet.get_device("SERIAL_002")
    assert serial_dev2 is not None
    await serial_dev2.disconnect()
    fleet._refresh_dedup_exclusions()
    assert len(fleet._dedup_excluded) == 2  # NOT cleared — devices still in fleet

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_dedup_exclusion_persists_when_all_serial_connected() -> None:
    """Exclusions should NOT clear when all serial devices are connected."""
    fleet = await _make_fleet_with_sn()
    fleet._dedup_excluded.add("BLE_ADDR_001")

    # All serial devices are connected — exclusions should persist
    fleet._refresh_dedup_exclusions()

    assert "BLE_ADDR_001" in fleet._dedup_excluded

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_dedup_exclusion_clears_when_no_serial_devices() -> None:
    """Exclusions should clear when no serial devices exist at all."""
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    fleet._dedup_excluded.add("BLE_ADDR_001")

    # No devices at all — should clear exclusions
    fleet._refresh_dedup_exclusions()

    assert len(fleet._dedup_excluded) == 0


# ── Release Hold Fix ──


async def test_release_hold_uses_full_id() -> None:
    """release_device should store blackout under the full device ID, not the API prefix."""
    fleet = await _make_fleet_with_sn()

    # Release using short prefix (like the API would)
    ok = await fleet.release_device("SERIAL_001", hold_seconds=300)
    assert ok

    # Blackout should be stored under full ID
    assert "SERIAL_001" in fleet._reconnect_blackout
    assert "SERIAL" not in fleet._reconnect_blackout  # Not the prefix

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_reconnect_clears_blackout() -> None:
    """reconnect_device should clear the reconnect blackout."""
    fleet = await _make_fleet_with_sn()

    await fleet.release_device("SERIAL_001", hold_seconds=300)
    assert "SERIAL_001" in fleet._reconnect_blackout

    # Reconnect should clear the blackout
    # (reconnect will fail because no discovery info, but blackout should still clear)
    await fleet.reconnect_device("SERIAL_001")
    assert "SERIAL_001" not in fleet._reconnect_blackout

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


# ── Failure Limit ──


async def test_failure_limit_removes_device() -> None:
    """Devices with 20+ consecutive failures should be removed from the fleet."""
    from blinky_server.transport.discovery import DiscoveredDevice

    fleet = await _make_fleet_with_sn()

    # Add a device in error state with 20 failures (limit raised from 10 to 20)
    bad_transport = MockTransport(transport_type="ble")
    bad_device = Device(
        device_id="BAD_BLE_001",
        port="XX:XX:XX:XX:XX:XX",
        platform="unknown",
        transport=bad_transport,  # type: ignore[arg-type]
    )
    bad_device.state = DeviceState.ERROR
    fleet._devices[bad_device.id] = bad_device
    fleet._reconnect_failures["BAD_BLE_001"] = 20
    # Need discovery info for the reconnect path to check failure count
    fleet._device_discovery["BAD_BLE_001"] = DiscoveredDevice(
        device_id="BAD_BLE_001",
        platform="unknown",
        transport_type="ble",
        address="XX:XX:XX:XX:XX:XX",
    )

    # Run reconnect — device should be marked for removal
    await fleet._reconnect_disconnected()

    # Device should be removed
    assert "BAD_BLE_001" not in fleet._devices

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_device_without_discovery_info_skipped() -> None:
    """Devices without discovery info should be skipped during reconnect."""
    fleet = FleetManager(enable_ble=False, enable_serial=False)

    transport = MockTransport()
    device = Device(
        device_id="NO_DISC",
        port="/dev/fail",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    device.state = DeviceState.ERROR
    fleet._devices[device.id] = device
    # No entry in _device_discovery — device should be skipped

    await fleet._reconnect_disconnected()

    # Device should remain in fleet (not removed, not reconnected)
    assert "NO_DISC" in fleet._devices
    # No failure counter should be set (skipped, not attempted)
    assert fleet._reconnect_failures.get("NO_DISC", 0) == 0

    # Cleanup
    await device.disconnect()


# ── Device Info Population ──


async def test_apply_info_populates_sn_and_ble() -> None:
    """apply_info should extract sn and ble from json info response."""
    transport = MockTransport(
        device_info={
            "version": "1.0.1",
            "platform": "nrf52840",
            "sn": "ABCD1234",
            "ble": "AA:BB:CC:DD:EE:FF",
            "device": {"configured": False, "safeMode": True},
        }
    )
    device = Device(
        device_id="TEST_SN",
        port="/dev/test",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()

    assert device.hardware_sn == "ABCD1234"
    assert device.ble_address == "AA:BB:CC:DD:EE:FF"
    assert device.version == "1.0.1"
    assert device.platform == "nrf52840"

    await device.disconnect()


async def test_to_dict_includes_mtu_for_ble() -> None:
    """to_dict should include mtu field when transport has mtu property."""
    transport = MockTransport()
    transport.mtu = 20  # type: ignore[attr-defined]

    device = Device(
        device_id="BLE_TEST",
        port="AA:BB:CC:DD:EE:FF",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()

    d = device.to_dict()
    assert d["mtu"] == 20

    await device.disconnect()


async def test_to_dict_no_mtu_for_serial() -> None:
    """to_dict should not include mtu for transports without mtu property."""
    transport = MockTransport()

    device = Device(
        device_id="SER_TEST",
        port="/dev/ttyTEST",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()

    d = device.to_dict()
    assert "mtu" not in d

    await device.disconnect()


# ── Liveness Check ──


async def test_liveness_updates_last_seen() -> None:
    """Liveness ping should update last_seen and device info."""
    fleet = await _make_fleet_with_sn()
    device = fleet.get_device("SERIAL_001")
    assert device is not None

    old_last_seen = device.last_seen
    assert old_last_seen is not None

    # Small delay to ensure monotonic time advances
    import asyncio

    await asyncio.sleep(0.05)

    await fleet._ping_device(device)

    assert device.last_seen is not None
    assert device.last_seen > old_last_seen

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


async def test_liveness_triggers_disconnect_on_failure() -> None:
    """Liveness ping failure should trigger disconnect handling."""
    transport = MockTransport()
    device = Device(
        device_id="FAIL_PING",
        port="/dev/fail",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()
    assert device.state == DeviceState.CONNECTED

    # Disconnect transport to make ping fail
    await transport.disconnect()

    fleet = FleetManager()
    fleet._devices[device.id] = device

    await fleet._ping_device(device)

    assert device.state == DeviceState.DISCONNECTED

    # Cleanup
    for d in fleet.get_all_devices():
        await d.disconnect()


# ── Discovery Now ──


async def test_discover_now() -> None:
    """discover_now should run discovery and reconnect."""
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    # With no transports enabled, discover_now should complete without error
    await fleet.discover_now()
    assert fleet.get_all_devices() == []


# ── FleetManager Pause/Resume Discovery ──


async def test_pause_resume_discovery() -> None:
    fleet = FleetManager()
    assert fleet._discovery_pause_count == 0

    fleet.pause_discovery()
    assert fleet._discovery_pause_count == 1

    fleet.pause_discovery()
    assert fleet._discovery_pause_count == 2

    fleet.resume_discovery()
    assert fleet._discovery_pause_count == 1

    fleet.resume_discovery()
    assert fleet._discovery_pause_count == 0

    # Should not go below 0
    fleet.resume_discovery()
    assert fleet._discovery_pause_count == 0


# ── PR #143 follow-up: broadcaster re-emit reliability ──


async def test_broadcaster_emits_command_multiple_times_with_distinct_sequences(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A single broadcast_command call MUST emit the command payload
    multiple times with FRESH sequence numbers (per
    ``COMMAND_REEMIT_COUNT``). This is the fix for the cart-side BLE
    receive flakiness measured 2026-05-18: the firmware's BleScanner
    has a single-slot rxBuffer and dedups by (source, seq), so the
    pre-fix one-emit-per-command meant one chance per command to land.

    Layout in COMMAND_V2: ``[version, type, seq, fragment, cmdid_lo,
    cmdid_hi, ...command-string]`` — a 2-byte command_id prefix
    separates the header from the actual command bytes. The test pins
    both the re-emit count contract (N emits, fresh seqs each) AND the
    idempotency contract (all N emits share the SAME command_id).
    """
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster
    from blinky_server.ble.protocol import COMMAND_V2_TOKEN_SIZE, COMPANY_ID, PacketType

    bc = FleetBroadcaster()
    # Skip the real D-Bus connect; stub the advertisement object so we
    # can observe payload changes.
    fake_adv = MagicMock()
    payloads_emitted: list[bytes] = []

    def capture_payload(company_id: int, payload: bytes) -> None:
        assert company_id == COMPANY_ID
        payloads_emitted.append(payload)

    fake_adv._set_manufacturer_payload = capture_payload
    bc._adv = fake_adv

    # Speed up the test: emit hold-time → ~zero
    _fast_broadcaster(monkeypatch)

    await bc.broadcast_command("gen plasma")

    # Decode the payloads we captured.
    command_emits: list[tuple[int, int, str]] = []
    noop_emits: list[int] = []
    for p in payloads_emitted:
        # Packet layout: [version, type, seq, fragment, ...payload]
        if len(p) < 4:
            continue
        ptype = p[1]
        seq = p[2]
        if ptype == PacketType.COMMAND_V2.value:
            # COMMAND_V2: [hdr (4)] [cmdid LE (2)] [command string]
            assert len(p) >= 4 + COMMAND_V2_TOKEN_SIZE
            cmd_id = p[4] | (p[5] << 8)
            cmd = p[4 + COMMAND_V2_TOKEN_SIZE :].decode("utf-8", errors="replace")
            command_emits.append((seq, cmd_id, cmd))
        elif ptype == 0x00:
            noop_emits.append(seq)

    # Exactly COMMAND_REEMIT_COUNT command emits, all carrying the same
    # command, each with a distinct sequence, all sharing one command_id.
    assert len(command_emits) == FleetBroadcaster.COMMAND_REEMIT_COUNT, (
        f"expected {FleetBroadcaster.COMMAND_REEMIT_COUNT} re-emits, got {len(command_emits)}"
    )
    seqs = [s for s, _i, _c in command_emits]
    assert len(set(seqs)) == len(seqs), f"seqs not unique: {seqs}"
    assert all(c == "gen plasma" for _s, _i, c in command_emits)

    # Idempotency invariant: every re-emit of one LOGICAL command carries
    # the SAME command_id so the firmware can short-circuit re-emits as
    # the same logical event. See BLE_FLEET_RELIABILITY_PLAN item #2.
    cmd_ids = {i for _s, i, _c in command_emits}
    assert len(cmd_ids) == 1, f"all re-emits should share one command_id, got {cmd_ids}"
    assert 1 <= next(iter(cmd_ids)) <= 0xFFFF

    # Exactly one noop terminator at the end.
    assert len(noop_emits) == 1
    # And the noop's seq is fresh (advanced past all command emits).
    assert noop_emits[0] > max(seqs)

    # The noop MUST carry at least one payload byte. The firmware
    # rejects ``payloadLen == 0`` packets BEFORE recording in its
    # (src, seq) dedup ring, so a payload-less noop loops through the
    # firmware's drop path on every BlueZ on-air retransmit (and on
    # every device in range). Verified empirically 2026-05-19: a
    # malformed noop produced ``dropped=234`` on the test chip in <2 min
    # uptime. Any noop encoding that strips this byte regresses that bug.
    noop_payload_len = len(payloads_emitted[-1]) - 4  # subtract 4-byte header
    assert noop_payload_len >= 1, (
        f"noop terminator must have a non-empty payload, got "
        f"{noop_payload_len}-byte payload (header-only = malformed)"
    )


async def test_broadcaster_assigns_distinct_command_ids_across_calls(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Each call to ``broadcast_command`` MUST get its own command_id
    so the firmware treats consecutive calls as different logical
    commands (i.e. the second one isn't short-circuited as a re-emit
    of the first). The id is a monotonic uint16 starting at 1.
    """
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster
    from blinky_server.ble.protocol import COMMAND_V2_TOKEN_SIZE, PacketType

    bc = FleetBroadcaster()
    fake_adv = MagicMock()
    payloads: list[bytes] = []
    fake_adv._set_manufacturer_payload = lambda _c, p: payloads.append(p)
    bc._adv = fake_adv
    _fast_broadcaster(monkeypatch)

    await bc.broadcast_command("gen plasma")
    await bc.broadcast_command("effect hue")

    ids_per_command: dict[str, set[int]] = {}
    for p in payloads:
        if len(p) < 4 + COMMAND_V2_TOKEN_SIZE or p[1] != PacketType.COMMAND_V2.value:
            continue
        cmd_id = p[4] | (p[5] << 8)
        cmd = p[4 + COMMAND_V2_TOKEN_SIZE :].decode("utf-8", errors="replace")
        ids_per_command.setdefault(cmd, set()).add(cmd_id)

    assert set(ids_per_command.keys()) == {"gen plasma", "effect hue"}
    plasma_ids = ids_per_command["gen plasma"]
    hue_ids = ids_per_command["effect hue"]
    assert len(plasma_ids) == 1 and len(hue_ids) == 1
    assert plasma_ids != hue_ids, "consecutive broadcasts must use different command_ids"


async def test_discovery_plumbs_last_cmd_id_to_new_ble_device(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When discovery surfaces a previously-unseen BLE device whose adv
    carried a gossip-ACK block, the freshly-created Device must have
    last_cmd_id populated from disc.extra. Without this, the first cycle
    would compare against None and never trigger re-broadcast — the
    invariant we depend on for cascade #5."""
    from typing import Any

    from blinky_server.transport.discovery import DiscoveredDevice

    fleet = FleetManager(enable_ble=False, enable_serial=False)

    disc = DiscoveredDevice(
        device_id="AA:BB:CC:DD:EE:01",
        platform="nrf52840",
        transport_type="ble",
        address="AA:BB:CC:DD:EE:01",
        description="Blinky-tube_v2-01",
        rssi=-55,
        extra={"last_cmd_id": 42},
    )

    async def fake_discover_all(**_kwargs: Any) -> list[DiscoveredDevice]:
        return [disc]

    monkeypatch.setattr("blinky_server.device.manager.discover_all", fake_discover_all)
    monkeypatch.setattr(fleet, "_refresh_dedup_exclusions", lambda: None)

    await fleet._discover_and_connect()

    dev = fleet._devices["AA:BB:CC:DD:EE:01"]
    assert dev.last_cmd_id == 42


async def test_discovery_plumbs_last_cmd_id_to_known_ble_device(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When discovery re-sees a known BLE device, last_cmd_id MUST be
    updated each cycle — that's how the broadcaster watches catch-up
    progress. Setting it once at create-time wouldn't be enough.

    Negative: when the adv didn't carry an ACK block this cycle
    (``last_cmd_id`` absent from extra), the previous value is preserved
    rather than overwritten with None — the device is still on whatever
    command_id it last reported, and assuming "unknown" would mask
    legitimately-caught-up devices and cause spurious re-broadcast."""
    from typing import Any

    from blinky_server.transport.discovery import DiscoveredDevice

    fleet = FleetManager(enable_ble=False, enable_serial=False)

    # Seed an existing device with last_cmd_id=7.
    transport = MockTransport(transport_type="ble")
    existing = Device(
        device_id="AA:BB:CC:DD:EE:02",
        port="AA:BB:CC:DD:EE:02",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    existing.state = DeviceState.PRESENT
    existing.last_cmd_id = 7
    fleet._devices["AA:BB:CC:DD:EE:02"] = existing

    # Cycle 1: ACK advances to 8.
    disc_with_ack = DiscoveredDevice(
        device_id="AA:BB:CC:DD:EE:02",
        platform="nrf52840",
        transport_type="ble",
        address="AA:BB:CC:DD:EE:02",
        extra={"last_cmd_id": 8},
    )

    async def discover_ack(**_kwargs: Any) -> list[DiscoveredDevice]:
        return [disc_with_ack]

    monkeypatch.setattr("blinky_server.device.manager.discover_all", discover_ack)
    monkeypatch.setattr(fleet, "_refresh_dedup_exclusions", lambda: None)
    await fleet._discover_and_connect()
    assert existing.last_cmd_id == 8

    # Cycle 2: ACK missing from adv (scan miss, weak signal, etc.).
    # Previous value MUST be preserved.
    disc_no_ack = DiscoveredDevice(
        device_id="AA:BB:CC:DD:EE:02",
        platform="nrf52840",
        transport_type="ble",
        address="AA:BB:CC:DD:EE:02",
        extra={},  # no last_cmd_id key
    )

    async def discover_no_ack(**_kwargs: Any) -> list[DiscoveredDevice]:
        return [disc_no_ack]

    monkeypatch.setattr("blinky_server.device.manager.discover_all", discover_no_ack)
    await fleet._discover_and_connect()
    assert existing.last_cmd_id == 8  # preserved, NOT overwritten with None


async def test_rebroadcast_to_laggards_noop_before_first_broadcast(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """No command has ever been broadcast → re-broadcast must be a no-op
    even with laggards in the device list. The cached _command_id starts
    at 0 (reserved for "no command applied yet"); refusing to re-emit
    here prevents firmware confusion."""
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster

    bc = FleetBroadcaster()
    bc._adv = MagicMock()
    _fast_broadcaster(monkeypatch)

    n = await bc.rebroadcast_to_laggards([("dev-a", 0), ("dev-b", None)])
    assert n == 0
    bc._adv._set_manufacturer_payload.assert_not_called()


async def test_rebroadcast_to_laggards_skips_caught_up_and_unknown(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Devices whose ``last_cmd_id`` matches ``_command_id`` are caught
    up; ``None`` means the device's ACK wasn't observed this cycle
    (treated as unknown, NOT 0). Both are skipped."""
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster
    from blinky_server.ble.protocol import COMMAND_V2_TOKEN_SIZE, PacketType

    bc = FleetBroadcaster()
    payloads: list[bytes] = []
    fake_adv = MagicMock()
    fake_adv._set_manufacturer_payload = lambda _c, p: payloads.append(p)
    bc._adv = fake_adv
    _fast_broadcaster(monkeypatch)

    # Run a real broadcast so _command_id + _last_command get populated.
    await bc.broadcast_command("gen fire")
    payloads.clear()  # drop the initial broadcast's emits

    current = bc._command_id
    n = await bc.rebroadcast_to_laggards(
        [
            ("dev-caught-up", current),
            ("dev-unknown", None),
        ]
    )
    assert n == 0
    # Nothing aired.
    cmd_emits = [p for p in payloads if len(p) >= 4 and p[1] == PacketType.COMMAND_V2.value]
    assert cmd_emits == []
    # Sanity: COMMAND_V2_TOKEN_SIZE is a stable wire constant the firmware
    # still depends on; touch the import so a rename here triggers a test
    # failure rather than silent drift.
    assert COMMAND_V2_TOKEN_SIZE == 2


async def test_rebroadcast_to_laggards_re_emits_for_lagged_device(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A device whose last_cmd_id < broadcaster's _command_id is a
    laggard. Re-emit the cached _last_command REBROADCAST_EMIT_COUNT
    times with the SAME _command_id (so other devices that already have
    it idempotency-skip)."""
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster
    from blinky_server.ble.protocol import COMMAND_V2_TOKEN_SIZE, PacketType

    bc = FleetBroadcaster()
    payloads: list[bytes] = []
    fake_adv = MagicMock()
    fake_adv._set_manufacturer_payload = lambda _c, p: payloads.append(p)
    bc._adv = fake_adv
    _fast_broadcaster(monkeypatch)

    await bc.broadcast_command("gen plasma")
    original_cmd_id = bc._command_id
    payloads.clear()

    n = await bc.rebroadcast_to_laggards(
        [
            ("dev-caught-up", original_cmd_id),
            ("dev-lagged", original_cmd_id - 1),  # one behind
        ]
    )
    assert n == 1

    cmd_emits = [p for p in payloads if len(p) >= 4 and p[1] == PacketType.COMMAND_V2.value]
    assert len(cmd_emits) == FleetBroadcaster.REBROADCAST_EMIT_COUNT
    # All emits share the original command_id (idempotency invariant).
    for p in cmd_emits:
        cmd_id = p[4] | (p[5] << 8)
        cmd_str = p[4 + COMMAND_V2_TOKEN_SIZE :].decode()
        assert cmd_id == original_cmd_id
        assert cmd_str == "gen plasma"
    # _command_id MUST NOT advance — re-broadcast is the SAME logical
    # command, just retried.
    assert bc._command_id == original_cmd_id


async def test_rebroadcast_to_laggards_caps_at_max_attempts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """After REBROADCAST_MAX_ATTEMPTS cycles for a permanently-out-of-range
    device, the broadcaster gives up. The next cycle for the same
    (device, command_id) is a no-op so the radio isn't soaked."""
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster
    from blinky_server.ble.protocol import PacketType

    bc = FleetBroadcaster()
    payloads: list[bytes] = []
    fake_adv = MagicMock()
    fake_adv._set_manufacturer_payload = lambda _c, p: payloads.append(p)
    bc._adv = fake_adv
    _fast_broadcaster(monkeypatch)

    await bc.broadcast_command("gen fire")
    cmd_id = bc._command_id
    payloads.clear()

    # Cycle MAX_ATTEMPTS times — all should re-emit.
    for cycle in range(FleetBroadcaster.REBROADCAST_MAX_ATTEMPTS):
        n = await bc.rebroadcast_to_laggards([("dev-stuck", cmd_id - 1)])
        assert n == 1, f"cycle {cycle + 1} should re-broadcast"

    # The next cycle MUST give up (no on-air emit) even though the
    # device is still lagged.
    payloads.clear()
    n = await bc.rebroadcast_to_laggards([("dev-stuck", cmd_id - 1)])
    assert n == 0
    cmd_emits = [p for p in payloads if len(p) >= 4 and p[1] == PacketType.COMMAND_V2.value]
    assert cmd_emits == []


async def test_rebroadcast_to_laggards_counter_resets_on_new_command(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When the operator fires a fresh command, the per-device attempt
    counter resets — a device that was given up on for command_id=N is
    eligible again for command_id=N+1. Otherwise the cap would persist
    forever, defeating recovery."""
    from unittest.mock import MagicMock

    from blinky_server.ble.advertiser import FleetBroadcaster

    bc = FleetBroadcaster()
    payloads: list[bytes] = []
    fake_adv = MagicMock()
    fake_adv._set_manufacturer_payload = lambda _c, p: payloads.append(p)
    bc._adv = fake_adv
    _fast_broadcaster(monkeypatch)

    # First command + max-out the attempts for one device.
    await bc.broadcast_command("gen fire")
    cmd_id_1 = bc._command_id
    for _ in range(FleetBroadcaster.REBROADCAST_MAX_ATTEMPTS):
        await bc.rebroadcast_to_laggards([("dev-stuck", cmd_id_1 - 1)])
    # Cap reached.
    assert bc._rebroadcast_attempts["dev-stuck"] == FleetBroadcaster.REBROADCAST_MAX_ATTEMPTS

    # Fresh command — counter should clear.
    await bc.broadcast_command("effect hue")
    cmd_id_2 = bc._command_id
    assert cmd_id_2 != cmd_id_1
    assert bc._rebroadcast_attempts == {}, "fresh command must clear per-device counters"

    # Same device is now eligible for cmd_id_2.
    payloads.clear()
    n = await bc.rebroadcast_to_laggards([("dev-stuck", cmd_id_2 - 1)])
    assert n == 1


async def test_build_command_v2_packet_layout() -> None:
    """Wire-format pin for COMMAND_V2 — the firmware-side decoder relies on
    the exact byte order. Any change here MUST match
    ``blinky-things/comms/BleProtocol.h`` (COMMAND_V2 = 0x04) and the
    LE 2-byte command_id token immediately after the 4-byte header.
    """
    from blinky_server.ble.protocol import (
        FRAGMENT_SINGLE,
        PROTOCOL_VERSION,
        PacketType,
        build_command_v2_packet,
    )

    pkt = build_command_v2_packet("gen fire", sequence=7, command_id=0xBEEF)
    assert pkt[0] == PROTOCOL_VERSION
    assert pkt[1] == PacketType.COMMAND_V2.value == 0x04
    assert pkt[2] == 7
    assert pkt[3] == FRAGMENT_SINGLE
    # Command ID little-endian
    assert pkt[4] == 0xEF
    assert pkt[5] == 0xBE
    assert pkt[6:] == b"gen fire"


# ── Gossip-ACK parser (BLE_FLEET_RELIABILITY_PLAN #5 phase 2) ──────────


async def test_parse_gossip_ack_valid() -> None:
    """Wire-format pin for the firmware → server ACK block. The
    firmware encodes ``[marker=0xA0][cmd_id LE 2B]`` in scan-response
    manufacturer data; the parser returns the cmd_id."""
    from blinky_server.ble.protocol import GOSSIP_ACK_MARKER, parse_gossip_ack

    # cmd_id = 0xBEEF (little-endian on the wire)
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER, 0xEF, 0xBE))) == 0xBEEF
    # cmd_id = 0 (firmware boot default — no command applied yet)
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER, 0, 0))) == 0
    # cmd_id = 1 (first broadcaster command)
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER, 0x01, 0x00))) == 1
    # Max uint16
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER, 0xFF, 0xFF))) == 0xFFFF


async def test_parse_gossip_ack_rejects_wrong_marker() -> None:
    """Marker byte 0x01 means COMMAND_V2 (broadcaster's own payload echoed
    in scan), not an ACK. Return None so the caller doesn't confuse this
    for a real ACK at cmd_id=(seq<<8 | fragment) garbage."""
    from blinky_server.ble.protocol import parse_gossip_ack

    # PROTOCOL_VERSION (0x01) — would happen if we accidentally scanned
    # our own broadcaster echo / a misconfigured peer using the same
    # company ID.
    assert parse_gossip_ack(bytes((0x01, 0xEF, 0xBE))) is None
    # 0x00 — uninitialized manufacturer data
    assert parse_gossip_ack(bytes((0x00, 0x00, 0x00))) is None
    # Any non-0xA0 first byte
    assert parse_gossip_ack(bytes((0x42, 0xEF, 0xBE))) is None


async def test_parse_gossip_ack_rejects_wrong_size() -> None:
    """Only 3-byte payloads parse. Shorter = malformed; longer = some
    other manufacturer-data block (e.g., a COMMAND_V2 packet with the
    same company ID)."""
    from blinky_server.ble.protocol import GOSSIP_ACK_MARKER, parse_gossip_ack

    # Empty
    assert parse_gossip_ack(b"") is None
    # 1 byte (marker only)
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER,))) is None
    # 2 bytes (marker + 1 byte of cmd_id) — would silently parse as
    # cmd_id=byte if we didn't check size.
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER, 0x42))) is None
    # 4 bytes (marker + LE16 + trailing byte) — looks like a corrupted
    # extension or someone else's manufacturer data; refuse.
    assert parse_gossip_ack(bytes((GOSSIP_ACK_MARKER, 0xEF, 0xBE, 0x00))) is None
    # Full COMMAND_V2 packet (10+ bytes) — broadcaster's own emission
    # echoed in some unrelated peer's scan response with COMPANY_ID
    # collision. Don't misinterpret.
    assert parse_gossip_ack(b"\x01\x04\x07\x10\xef\xbegen fire") is None


async def test_parse_gossip_ack_handles_none() -> None:
    """``None`` is what bleak returns when the requested company ID is
    absent from manufacturer_data. Must not raise."""
    from blinky_server.ble.protocol import parse_gossip_ack

    assert parse_gossip_ack(None) is None


async def test_parse_gossip_ack_accepts_bytearray_and_memoryview() -> None:
    """bleak sometimes hands manufacturer_data values as bytes, sometimes
    as bytearray (depends on platform backend). Parser must accept both
    plus memoryview without conversion overhead at the call site."""
    from blinky_server.ble.protocol import GOSSIP_ACK_MARKER, parse_gossip_ack

    raw = bytes((GOSSIP_ACK_MARKER, 0xEF, 0xBE))
    assert parse_gossip_ack(raw) == 0xBEEF
    assert parse_gossip_ack(bytearray(raw)) == 0xBEEF
    assert parse_gossip_ack(memoryview(raw)) == 0xBEEF


# ── _scan_with_retry input validation (PR #144 review) ─────────────────


@pytest.mark.parametrize("attempts", [0, -1, -5])
async def test_scan_with_retry_rejects_non_positive_attempts(attempts: int) -> None:
    """``_scan_with_retry`` raises ``ValueError`` for ``attempts < 1``.

    The earlier implementation would fall through to
    ``assert last_exc is not None`` if no attempt was ever made,
    surfacing an unhelpful ``AssertionError``. PR #144 review pinned
    the contract: misuse should raise a clear ``ValueError`` at the
    call site instead.
    """
    from blinky_server.firmware.ble_dfu import _scan_with_retry

    async def _never_called() -> object:
        raise AssertionError("coroutine factory should not be invoked")

    with pytest.raises(ValueError, match="attempts must be >= 1"):
        await _scan_with_retry(_never_called, attempts=attempts)
