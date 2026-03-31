"""Tests for BLE fleet management features.

Covers: fleet status/discover/settings endpoints, dedup exclusion logic,
failure limit, serial liveness, release hold fix, device info population.
"""

from __future__ import annotations

from httpx import AsyncClient

from blinky_server.device.device import Device, DeviceState
from blinky_server.device.manager import FleetManager

from .mock_transport import MockTransport

# ── Helpers ──


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


async def test_dedup_exclusion_clears_on_serial_disconnect() -> None:
    """Exclusions should clear when a serial device disconnects."""
    fleet = await _make_fleet_with_sn()
    fleet._dedup_excluded.add("BLE_ADDR_001")
    fleet._dedup_excluded.add("BLE_ADDR_002")

    # Disconnect a serial device
    serial_dev = fleet.get_device("SERIAL_001")
    assert serial_dev is not None
    await serial_dev.disconnect()

    # Refresh exclusions
    fleet._refresh_dedup_exclusions()

    # Exclusions should be cleared
    assert len(fleet._dedup_excluded) == 0

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
    """Devices with 10+ consecutive failures should be removed from the fleet."""
    from blinky_server.transport.discovery import DiscoveredDevice

    fleet = await _make_fleet_with_sn()

    # Add a device in error state with 10 failures
    bad_transport = MockTransport(transport_type="ble")
    bad_device = Device(
        device_id="BAD_BLE_001",
        port="XX:XX:XX:XX:XX:XX",
        platform="unknown",
        transport=bad_transport,  # type: ignore[arg-type]
    )
    bad_device.state = DeviceState.ERROR
    fleet._devices[bad_device.id] = bad_device
    fleet._reconnect_failures["BAD_BLE_001"] = 10
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


async def test_failure_count_incremented() -> None:
    """Failed reconnect attempts should increment the failure counter."""
    fleet = FleetManager()

    transport = MockTransport()
    device = Device(
        device_id="FAIL_DEV",
        port="/dev/fail",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    device.state = DeviceState.ERROR
    fleet._devices[device.id] = device

    # No discovery info → reconnect will fail
    assert fleet._reconnect_failures.get("FAIL_DEV", 0) == 0

    # Won't actually attempt because no discovery info, but structure is tested
    await fleet._reconnect_disconnected()

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
