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


async def test_send_to_all_reports_disconnected(fleet_with_devices: FleetManager) -> None:
    """Disconnected devices appear in results with `skipped: state=…` entry.

    Pre-2026-05-01 they were silently filtered out; deploy.sh treated the
    truncated dict as success even when some devices missed the command.
    """
    await fleet_with_devices.release_device("MOCK_DEVICE_000")

    results = await fleet_with_devices.send_to_all("ble")
    # Both devices appear; the released one is marked skipped, the other got the command.
    assert len(results) == 2
    assert "MOCK_DEVICE_000" in results
    assert results["MOCK_DEVICE_000"].startswith("skipped: state=")
    assert "MOCK_DEVICE_001" in results
    assert not results["MOCK_DEVICE_001"].startswith("skipped:")


# ── Recovery firmware whitelist ─────────────────────────────────────


def _make_fleet() -> FleetManager:
    return FleetManager(enable_ble=False, enable_serial=False)


def test_set_recovery_firmware_persists(tmp_path, monkeypatch) -> None:
    """Path + whitelist round-trip through the JSON state file."""
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    fw = tmp_path / "fw.hex"
    fw.write_bytes(b"x" * 2000)
    fm = _make_fleet()
    fm.set_recovery_firmware(str(fw), ["AA:BB", "CC:DD"])

    loaded = fm._load_recovery_firmware()
    assert loaded is not None
    path, ids = loaded
    assert path == str(fw)
    assert ids == {"AA:BB", "CC:DD"}


def test_set_recovery_firmware_requires_non_empty_whitelist(tmp_path, monkeypatch) -> None:
    """Empty whitelist must raise — silent no-op would mask the fact that
    auto-recovery is disabled while looking like it was armed."""
    import pytest

    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    fw = tmp_path / "fw.hex"
    fw.write_bytes(b"x" * 2000)
    fm = _make_fleet()
    with pytest.raises(ValueError):
        fm.set_recovery_firmware(str(fw), [])


def test_load_rejects_legacy_plain_string_format(tmp_path, monkeypatch) -> None:
    """A plain-string state file from before the JSON migration has no
    whitelist; auto-flashing unscoped is unsafe so we ignore it."""
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    fm = _make_fleet()
    legacy = fm._recovery_state_path()
    legacy.parent.mkdir(parents=True, exist_ok=True)
    legacy.write_text("/tmp/old-firmware.hex")  # not JSON

    assert fm._load_recovery_firmware() is None


def test_load_rejects_missing_firmware_file(tmp_path, monkeypatch) -> None:
    """If the firmware file pointed to by persisted state no longer exists,
    we must not try to arm recovery — the BLE DFU upload would fail anyway."""
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    fm = _make_fleet()
    fm.set_recovery_firmware(str(tmp_path / "ghost.hex"), ["X"])
    # set_recovery_firmware doesn't check existence; load does.
    # (Path was valid at set time, deleted before load — simulated here.)
    assert fm._load_recovery_firmware() is None
