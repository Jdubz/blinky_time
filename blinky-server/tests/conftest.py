"""Shared test fixtures."""

from __future__ import annotations

from collections.abc import AsyncGenerator

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api.app import create_app
from blinky_server.api.deps import set_fleet
from blinky_server.device.device import Device
from blinky_server.device.manager import FleetManager

from .mock_transport import MockTransport


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
    fleet = FleetManager()

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
