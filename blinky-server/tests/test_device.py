"""Tests for the Device class."""

from __future__ import annotations

import asyncio
import json

from blinky_server.device.device import Device, DeviceState

from .mock_transport import MockTransport


async def test_connect_populates_info(mock_transport: MockTransport) -> None:
    device = Device(
        device_id="SN123",
        port="/dev/ttyTEST0",
        platform="nrf52840",
        transport=mock_transport,  # type: ignore[arg-type]
    )
    await device.connect()

    assert device.state == DeviceState.CONNECTED
    assert device.version == "1.0.1"
    assert device.device_name == "Long Tube"
    assert device.device_type == "long_tube_v1"
    assert device.width == 4
    assert device.height == 60
    assert device.leds == 240
    assert device.configured is True
    assert device.safe_mode is False


async def test_connect_unconfigured(unconfigured_transport: MockTransport) -> None:
    device = Device(
        device_id="SN456",
        port="/dev/ttyTEST1",
        platform="esp32s3",
        transport=unconfigured_transport,  # type: ignore[arg-type]
    )
    await device.connect()

    assert device.state == DeviceState.CONNECTED
    assert device.configured is False
    assert device.safe_mode is True
    assert device.device_name is None


async def test_disconnect(mock_device: Device) -> None:
    assert mock_device.state == DeviceState.CONNECTED
    await mock_device.disconnect()
    assert mock_device.state == DeviceState.DISCONNECTED


async def test_to_dict(mock_device: Device) -> None:
    d = mock_device.to_dict()
    assert d["id"] == "TEST_SERIAL_001"
    assert d["port"] == "/dev/ttyTEST0"
    assert d["platform"] == "nrf52840"
    assert d["state"] == "connected"
    assert d["version"] == "1.0.1"
    assert d["device_name"] == "Long Tube"
    assert d["configured"] is True
    assert d["streaming"] is False


async def test_stream_subscription(mock_device: Device) -> None:
    queue: asyncio.Queue[dict[str, object] | None] = mock_device.subscribe_stream()

    # Inject streaming audio data
    transport: MockTransport = mock_device.transport  # type: ignore[assignment]
    audio_json = json.dumps({"a": {"l": 0.5, "t": 0.1}})
    transport.inject_line(audio_json)

    msg = queue.get_nowait()
    assert msg is not None
    assert msg["type"] == "audio"
    assert msg["device_id"] == "TEST_SERIAL_001"


async def test_stream_battery(mock_device: Device) -> None:
    queue: asyncio.Queue[dict[str, object] | None] = mock_device.subscribe_stream()

    transport: MockTransport = mock_device.transport  # type: ignore[assignment]
    transport.inject_line(json.dumps({"b": {"v": 4.1, "pct": 85}}))

    msg = queue.get_nowait()
    assert msg is not None
    assert msg["type"] == "battery"


async def test_stream_status(mock_device: Device) -> None:
    queue: asyncio.Queue[dict[str, object] | None] = mock_device.subscribe_stream()

    transport: MockTransport = mock_device.transport  # type: ignore[assignment]
    transport.inject_line(json.dumps({"type": "STATUS", "gen": "fire"}))

    msg = queue.get_nowait()
    assert msg is not None
    assert msg["type"] == "status"


async def test_unsubscribe(mock_device: Device) -> None:
    queue: asyncio.Queue[dict[str, object] | None] = mock_device.subscribe_stream()
    mock_device.unsubscribe_stream(queue)

    transport: MockTransport = mock_device.transport  # type: ignore[assignment]
    transport.inject_line(json.dumps({"a": {"l": 0.5}}))

    # Should not receive anything after unsubscribe
    assert queue.empty()


async def test_disconnect_notifies_subscribers(mock_device: Device) -> None:
    queue: asyncio.Queue[dict[str, object] | None] = mock_device.subscribe_stream()
    await mock_device.disconnect()

    msg = queue.get_nowait()
    assert msg is None  # None signals disconnection
