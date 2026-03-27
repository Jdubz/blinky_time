"""Tests for Pydantic models."""

from __future__ import annotations

import pytest
from pydantic import ValidationError

from blinky_server.api.models import (
    CommandRequest,
    CommandResponse,
    DeviceResponse,
    SettingValueRequest,
    StatusResponse,
    WsCommandMessage,
    WsStreamControlMessage,
    WsStreamData,
)


def test_command_request_valid() -> None:
    req = CommandRequest(command="ble")
    assert req.command == "ble"


def test_command_request_empty_rejected() -> None:
    with pytest.raises(ValidationError):
        CommandRequest(command="")


def test_setting_value_float() -> None:
    sv = SettingValueRequest(value=0.5)
    assert sv.value == 0.5


def test_setting_value_int() -> None:
    sv = SettingValueRequest(value=42)
    assert sv.value == 42


def test_setting_value_str() -> None:
    sv = SettingValueRequest(value="on")
    assert sv.value == "on"


def test_command_response() -> None:
    r = CommandResponse(response="OK switched to Water")
    assert r.response == "OK switched to Water"


def test_status_response() -> None:
    r = StatusResponse(status="released")
    assert r.status == "released"


def test_device_response_full() -> None:
    d = DeviceResponse(
        id="SN123",
        port="/dev/ttyACM0",
        platform="nrf52840",
        transport="serial",
        state="connected",
        version="1.0.1",
        device_type="long_tube_v1",
        device_name="Long Tube",
        width=4,
        height=60,
        leds=240,
        configured=True,
        safe_mode=False,
        streaming=False,
    )
    assert d.id == "SN123"
    assert d.configured is True


def test_device_response_minimal() -> None:
    d = DeviceResponse(
        id="SN456",
        port="/dev/ttyACM1",
        platform="esp32s3",
        transport="serial",
        state="connected",
    )
    assert d.version is None
    assert d.configured is False


def test_ws_command_message() -> None:
    msg = WsCommandMessage(command="set basespawnchance 0.5")
    assert msg.type == "command"
    assert msg.device_id is None


def test_ws_stream_control() -> None:
    msg = WsStreamControlMessage(enabled=True, mode="fast")
    assert msg.type == "stream_control"
    assert msg.enabled is True


def test_ws_stream_data() -> None:
    msg = WsStreamData(
        type="audio",
        device_id="SN123",
        data={"a": {"l": 0.5}},
    )
    assert msg.type == "audio"
