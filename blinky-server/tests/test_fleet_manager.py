"""Tests for the FleetManager."""

from __future__ import annotations

from blinky_server.device.device import DeviceState
from blinky_server.device.manager import FleetManager


async def test_get_device_by_full_id(fleet_with_devices: FleetManager) -> None:
    device = fleet_with_devices.get_device("MOCK_DEVICE_000")
    assert device is not None
    assert device.platform == "nrf52840"


async def test_get_device_by_prefix(fleet_with_devices: FleetManager) -> None:
    device = fleet_with_devices.get_device("MOCK_DEVICE_001")
    assert device is not None
    assert device.platform == "esp32s3"


async def test_get_device_not_found(fleet_with_devices: FleetManager) -> None:
    device = fleet_with_devices.get_device("NONEXISTENT")
    assert device is None


async def test_get_all_devices(fleet_with_devices: FleetManager) -> None:
    devices = fleet_with_devices.get_all_devices()
    assert len(devices) == 2


async def test_send_to_all(fleet_with_devices: FleetManager) -> None:
    results = await fleet_with_devices.send_to_all("ble")
    assert len(results) == 2
    for _device_id, resp in results.items():
        assert "[BLE]" in resp


async def test_release_device(fleet_with_devices: FleetManager) -> None:
    ok = await fleet_with_devices.release_device("MOCK_DEVICE_000")
    assert ok

    device = fleet_with_devices.get_device("MOCK_DEVICE_000")
    assert device is not None
    assert device.state == DeviceState.DISCONNECTED


async def test_release_nonexistent(fleet_with_devices: FleetManager) -> None:
    ok = await fleet_with_devices.release_device("NONEXISTENT")
    assert not ok


async def test_send_to_all_skips_disconnected(fleet_with_devices: FleetManager) -> None:
    # Disconnect one device
    await fleet_with_devices.release_device("MOCK_DEVICE_000")

    results = await fleet_with_devices.send_to_all("ble")
    # Only the connected device should respond
    assert len(results) == 1
    assert "MOCK_DEVICE_001" in results
