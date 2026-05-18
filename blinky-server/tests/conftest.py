"""Shared test fixtures."""

from __future__ import annotations

from collections.abc import AsyncGenerator, Generator
from pathlib import Path

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api.app import create_app
from blinky_server.api.deps import set_fleet
from blinky_server.device.device import Device
from blinky_server.device.manager import FleetManager

from .mock_transport import MockTransport


@pytest.fixture(autouse=True)
def _isolated_data_dir(
    tmp_path_factory: pytest.TempPathFactory, monkeypatch: pytest.MonkeyPatch
) -> Generator[Path, None, None]:
    """Redirect ``paths.data_dir()`` to a per-test tmp dir for every test.

    Verified necessary 2026-05-18 — without this, ANY test that creates a
    ``FleetManager`` and calls ``set_recovery_firmware()`` (directly or
    via a flash route) writes to the developer's real
    ``~/.local/share/blinky-server/recovery-firmware.json``. The running
    blinky-server reads that file at boot, so a leaked test write
    persisted into production auto-recovery state until manually cleaned
    up. The first version of test_routes_devices_flash.py did this with
    a fake ``L3A_TEST_DEV`` device id and we had to remove the file by
    hand.

    autouse + session-scoped tmp_path_factory + per-test monkeypatch
    means every test gets a fresh isolated directory; the production
    data dir is never touched by the test suite.
    """
    from blinky_server import paths as paths_mod

    tmp_path = tmp_path_factory.mktemp("blinky-server-data")
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    paths_mod._clear_cache()
    yield tmp_path
    paths_mod._clear_cache()


@pytest.fixture
def mock_transport() -> MockTransport:
    return MockTransport()


@pytest.fixture
def unconfigured_transport() -> MockTransport:
    return MockTransport(
        device_info={
            "version": "1.0.1",
            "device": {"configured": False, "safeMode": True},
        }
    )


@pytest_asyncio.fixture
async def mock_device(mock_transport: MockTransport) -> Device:
    device = Device(
        device_id="TEST_SERIAL_001",
        port="/dev/ttyTEST0",
        platform="nrf52840",
        transport=mock_transport,  # type: ignore[arg-type]
    )
    await device.connect()
    return device


@pytest_asyncio.fixture
async def fleet_with_devices() -> AsyncGenerator[FleetManager, None]:
    """Fleet manager with 2 mock devices (no real hardware)."""
    # enable_ble=False keeps the FleetBroadcaster out of tests — it needs a
    # real BlueZ D-Bus connection and would also add a "broadcast" entry to
    # fleet command results that the existing test cases don't expect.
    fleet = FleetManager(enable_ble=False, enable_serial=False)

    # Manually add mock devices instead of running discovery
    for i, (platform, info) in enumerate(
        [
            (
                "nrf52840",
                {
                    "version": "1.0.1",
                    "device": {
                        "id": "long_tube_v1",
                        "name": "Long Tube",
                        "width": 4,
                        "height": 60,
                        "leds": 240,
                        "configured": True,
                    },
                },
            ),
            (
                "esp32s3",
                {
                    "version": "1.0.1",
                    "device": {"configured": False, "safeMode": True},
                },
            ),
        ]
    ):
        transport = MockTransport(device_info=info)
        device = Device(
            device_id=f"MOCK_DEVICE_{i:03d}",
            port=f"/dev/ttyTEST{i}",
            platform=platform,
            transport=transport,  # type: ignore[arg-type]
        )
        await device.connect()
        fleet._devices[device.id] = device

    yield fleet

    for device in fleet.get_all_devices():
        await device.disconnect()


@pytest_asyncio.fixture
async def api_client(
    fleet_with_devices: FleetManager,
) -> AsyncGenerator[AsyncClient, None]:
    """FastAPI test client with mock fleet."""
    set_fleet(fleet_with_devices)
    app = create_app()

    transport = ASGITransport(app=app)  # type: ignore[arg-type]
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        yield client

    set_fleet(None)  # type: ignore[arg-type]
