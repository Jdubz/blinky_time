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


# ── Watchdog ping regression: cart_inner brick 2026-05-16 ──────────────


async def test_watchdog_ping_fires_during_full_cycle(monkeypatch) -> None:
    """In the normal (non-paused) loop path, systemd_notify.watchdog() must
    be invoked at the end of each successful cycle.

    Without this, systemd's WatchdogSec timer fires and SIGKILLs the process
    — even if the process is otherwise healthy.
    """
    import asyncio

    from blinky_server import systemd_notify
    from blinky_server.device import manager as mgr_mod

    ping_count = 0

    def fake_ping() -> None:
        nonlocal ping_count
        ping_count += 1

    monkeypatch.setattr(systemd_notify, "watchdog", fake_ping)
    monkeypatch.setattr(mgr_mod, "DISCOVERY_INTERVAL_S", 0.01)

    async def noop_async(*_a, **_kw):
        return None

    # Avoid real network/scan side-effects.
    monkeypatch.setattr(
        "blinky_server.transport.discovery.cleanup_stale_ble_connections", noop_async
    )

    fleet = FleetManager(enable_ble=False, enable_serial=False)
    monkeypatch.setattr(fleet, "_discover_and_connect", noop_async)
    monkeypatch.setattr(fleet, "_reconnect_disconnected", noop_async)
    monkeypatch.setattr(fleet, "_check_serial_threads", lambda: None)

    await fleet.start()
    try:
        await asyncio.sleep(0.05)  # ~5 cycles at 10ms interval
    finally:
        await fleet.stop()

    # At least one full cycle should have pinged. Exact count is timing-
    # dependent (asyncio scheduling), but zero would mean the bug is back.
    assert ping_count >= 1, "systemd_notify.watchdog() not called from full-cycle path"


async def test_watchdog_ping_fires_during_paused_discovery(monkeypatch) -> None:
    """REGRESSION: while pause_discovery() depth > 0 (i.e. a flash is
    running), the loop takes a 'continue' path. That path MUST still
    ping systemd, or the watchdog fires mid-flash and SIGKILLs the
    process — bricking the device being flashed.

    Evidence: 2026-05-16 cart_inner was bricked because the original
    pause path skipped the ping. Server's last ping was just before
    pause_discovery() was called; systemd killed the process exactly
    WatchdogSec=120s later, mid-DFU at 20% transfer.
    """
    import asyncio

    from blinky_server import systemd_notify
    from blinky_server.device import manager as mgr_mod

    ping_count = 0

    def fake_ping() -> None:
        nonlocal ping_count
        ping_count += 1

    monkeypatch.setattr(systemd_notify, "watchdog", fake_ping)
    monkeypatch.setattr(mgr_mod, "DISCOVERY_INTERVAL_S", 0.01)

    async def noop_async(*_a, **_kw):
        return None

    monkeypatch.setattr(
        "blinky_server.transport.discovery.cleanup_stale_ble_connections", noop_async
    )

    fleet = FleetManager(enable_ble=False, enable_serial=False)
    # If the loop takes the pause path, none of these should be called.
    discover_called = 0

    async def discover(*_a, **_kw):
        nonlocal discover_called
        discover_called += 1

    monkeypatch.setattr(fleet, "_discover_and_connect", discover)
    monkeypatch.setattr(fleet, "_reconnect_disconnected", noop_async)
    monkeypatch.setattr(fleet, "_check_serial_threads", lambda: None)

    fleet.pause_discovery()
    await fleet.start()
    try:
        await asyncio.sleep(0.05)  # ~5 paused cycles at 10ms interval
    finally:
        await fleet.stop()

    assert discover_called == 0, "pause_discovery() did not block the work path"
    assert (
        ping_count >= 1
    ), "systemd_notify.watchdog() not called from pause path (this is the cart_inner brick bug)"


async def test_watchdog_ping_continues_during_inline_blocking_call(monkeypatch) -> None:
    """REGRESSION 2: watchdog must ping even when the background loop is
    blocked on an inline `await` (e.g. multi-minute BLE DFU via auto-recovery).

    Evidence: 2026-05-16 cart_inner SECOND brick attempt — the pause-path
    ping I added in e79f8a41 only fires when the loop iterates. Auto-recovery
    calls `await upload_ble_dfu(...)` INSIDE the loop body, so the loop
    never returns to its `while` head for the multi-minute duration; the
    in-loop ping never fires; systemd SIGKILLs again.

    Fix: separate _watchdog_pinger task running independently. This test
    verifies the pinger continues even with the main loop's
    _discover_and_connect deliberately stuck on a long await.
    """
    import asyncio

    from blinky_server import systemd_notify
    from blinky_server.device import manager as mgr_mod

    ping_count = 0

    def fake_ping() -> None:
        nonlocal ping_count
        ping_count += 1

    monkeypatch.setattr(systemd_notify, "watchdog", fake_ping)
    monkeypatch.setattr(mgr_mod, "DISCOVERY_INTERVAL_S", 0.01)
    # Tight watchdog cadence so the test runs fast
    monkeypatch.setattr(mgr_mod.FleetManager, "WATCHDOG_PING_INTERVAL_S", 0.01)

    async def noop_async(*_a, **_kw):
        return None

    monkeypatch.setattr(
        "blinky_server.transport.discovery.cleanup_stale_ble_connections", noop_async
    )

    fleet = FleetManager(enable_ble=False, enable_serial=False)

    # Simulate the main loop being stuck on a long inline await (like
    # auto-recovery's upload_ble_dfu). The loop enters this and never
    # returns to its while head until the test ends.
    stuck_started = asyncio.Event()

    async def stuck_forever(*_a, **_kw):
        stuck_started.set()
        await asyncio.sleep(10)  # would block longer than the test

    monkeypatch.setattr(fleet, "_discover_and_connect", stuck_forever)
    monkeypatch.setattr(fleet, "_reconnect_disconnected", noop_async)
    monkeypatch.setattr(fleet, "_check_serial_threads", lambda: None)

    await fleet.start()
    try:
        # Wait for the loop to enter the stuck call
        await asyncio.wait_for(stuck_started.wait(), timeout=1.0)
        # Now the main loop is stuck. Sleep long enough for the
        # independent watchdog pinger to fire many times.
        await asyncio.sleep(0.1)  # 10x WATCHDOG_PING_INTERVAL_S=0.01
    finally:
        await fleet.stop()

    # Without the separate pinger task, ping_count would be 0 (or near 0)
    # — only the start-time first ping. With the separate task, we expect
    # multiple pings during the blocked window.
    assert ping_count >= 3, (
        f"Watchdog pinger did not continue while main loop was blocked. "
        f"Got {ping_count} pings. This is the cart_inner SECOND brick bug."
    )
