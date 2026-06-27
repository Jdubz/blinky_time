"""Tests for the device protocol (command/response handling)."""

from __future__ import annotations

import json

import pytest
import pytest_asyncio

from blinky_server.device.protocol import DeviceProtocol

from .mock_transport import MockTransport


@pytest_asyncio.fixture
async def transport() -> MockTransport:
    t = MockTransport()
    await t.connect()
    return t


@pytest.fixture
def protocol(transport: MockTransport) -> DeviceProtocol:
    return DeviceProtocol(transport)  # type: ignore[arg-type]


async def test_get_info(protocol: DeviceProtocol) -> None:
    info = await protocol.get_info()
    assert info["version"] == "1.0.1"
    assert info["device"]["name"] == "Long Tube"
    assert info["device"]["leds"] == 240


async def test_get_settings(protocol: DeviceProtocol) -> None:
    settings = await protocol.get_settings()
    assert len(settings) == 2
    assert settings[0]["name"] == "basespawnchance"
    assert settings[0]["cat"] == "fire"


async def test_set_setting(protocol: DeviceProtocol) -> None:
    resp = await protocol.set_setting("basespawnchance", 0.8)
    assert "OK" in resp
    assert "basespawnchance" in resp


async def test_set_generator(protocol: DeviceProtocol) -> None:
    resp = await protocol.set_generator("water")
    assert "Water" in resp


async def test_set_effect(protocol: DeviceProtocol) -> None:
    resp = await protocol.set_effect("hue")
    assert "hue" in resp


async def test_save_settings(protocol: DeviceProtocol) -> None:
    resp = await protocol.save_settings()
    assert "OK" in resp


async def test_load_settings(protocol: DeviceProtocol) -> None:
    resp = await protocol.load_settings()
    assert "OK" in resp


async def test_restore_defaults(protocol: DeviceProtocol) -> None:
    resp = await protocol.restore_defaults()
    assert "OK" in resp


async def test_slow_flash_commands_get_longer_timeout_floor(
    transport: MockTransport,
) -> None:
    """save/load get the 10 s floor on the 2 s serial path; other commands keep
    the base timeout. Regression guard for the deploy-critical fix (PR #148):
    a successful `save` was being timed out at 2 s, so deploy.sh reported
    FAILURE on a successful flash. Keyed on the first whitespace token.
    """
    from blinky_server.device import protocol as proto_mod

    proto = DeviceProtocol(transport)  # type: ignore[arg-type]
    captured: list[float] = []

    async def _capture(command: str, timeout: float) -> str:
        captured.append(timeout)
        return "OK"

    proto._send_and_collect = _capture  # type: ignore[method-assign]

    # Serial base is 2 s (COMMAND_TIMEOUT_S); the slow-flash floor is 10 s.
    await proto.send_command("save")
    assert captured[-1] == proto_mod.SLOW_FLASH_COMMAND_TIMEOUT_S
    await proto.send_command("load")
    assert captured[-1] == proto_mod.SLOW_FLASH_COMMAND_TIMEOUT_S
    # First-token keying: `save <arg>` still gets the floor.
    await proto.send_command("save foo")
    assert captured[-1] == proto_mod.SLOW_FLASH_COMMAND_TIMEOUT_S
    # A non-flash command keeps the base serial timeout.
    await proto.send_command("ble")
    assert captured[-1] == proto_mod.COMMAND_TIMEOUT_S
    # An explicit timeout argument always wins over the floor.
    await proto.send_command("save", timeout=1.5)
    assert captured[-1] == 1.5


async def test_slow_flash_floor_never_shortens_ble_timeout() -> None:
    """On BLE (15 s base) the 10 s floor must NOT shorten the timeout —
    ``max(base, floor)`` keeps the more generous ceiling.
    """
    from blinky_server.device import protocol as proto_mod

    ble_transport = MockTransport(transport_type="ble")
    await ble_transport.connect()
    proto = DeviceProtocol(ble_transport)  # type: ignore[arg-type]
    captured: list[float] = []

    async def _capture(command: str, timeout: float) -> str:
        captured.append(timeout)
        return "OK"

    proto._send_and_collect = _capture  # type: ignore[method-assign]

    await proto.send_command("save")
    assert captured[-1] == proto_mod.BLE_COMMAND_TIMEOUT_S


async def test_stream_start_stop(protocol: DeviceProtocol) -> None:
    assert not protocol.streaming

    await protocol.start_stream("on")
    assert protocol.streaming

    await protocol.stop_stream()
    assert not protocol.streaming


async def test_command_pauses_streaming(protocol: DeviceProtocol, transport: MockTransport) -> None:
    """Commands sent while streaming should auto-pause/resume."""
    await protocol.start_stream("on")
    assert protocol.streaming

    # Sending a non-stream command should pause, then resume
    transport._sent_commands.clear()
    resp = await protocol.send_command("ble")
    assert "BLE" in resp

    # Check that stream off was sent before the command
    cmds = transport.sent_commands
    assert "stream off" in cmds
    # And stream on was sent after
    assert "stream on" in cmds


async def test_send_command_basic(protocol: DeviceProtocol) -> None:
    resp = await protocol.send_command("ble")
    assert "[BLE]" in resp


async def test_stream_line_callback(protocol: DeviceProtocol, transport: MockTransport) -> None:
    """Streaming JSON lines should be routed to the stream callback."""
    received: list[str] = []
    protocol.on_stream_line(lambda line: received.append(line))

    # Inject audio data (simulates device streaming)
    audio_line = json.dumps({"a": {"l": 0.5, "t": 0.1}})
    transport.inject_line(audio_line)

    assert len(received) == 1
    assert '"a"' in received[0]


async def test_raw_line_callback(protocol: DeviceProtocol, transport: MockTransport) -> None:
    """Non-JSON lines should be routed to the raw line callback."""
    received: list[str] = []
    protocol.on_raw_line(lambda line: received.append(line))

    transport.inject_line("some log output")
    assert received == ["some log output"]


async def test_json_response_during_command(
    protocol: DeviceProtocol, transport: MockTransport
) -> None:
    """JSON responses (like json info) should be collected, not routed to stream."""
    stream_received: list[str] = []
    protocol.on_stream_line(lambda line: stream_received.append(line))

    info = await protocol.get_info()
    # The JSON response should have been parsed, not sent to stream callback
    assert info["version"] == "1.0.1"
    assert len(stream_received) == 0


async def test_parse_json_response_full() -> None:
    result = DeviceProtocol._parse_json_response('{"key": "value"}')
    assert result == {"key": "value"}


async def test_parse_json_response_multiline() -> None:
    text = 'some prefix\n{"key": "value"}\nsome suffix'
    result = DeviceProtocol._parse_json_response(text)
    assert result == {"key": "value"}


async def test_parse_json_response_empty() -> None:
    result = DeviceProtocol._parse_json_response("")
    assert result == {}


async def test_parse_json_response_raw_fallback() -> None:
    result = DeviceProtocol._parse_json_response("not json at all")
    assert result == {"raw": "not json at all"}
