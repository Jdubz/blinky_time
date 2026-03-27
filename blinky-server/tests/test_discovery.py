"""Tests for device discovery."""

from __future__ import annotations

from unittest.mock import patch

from blinky_server.transport.discovery import discover_serial_devices


class FakePortInfo:
    def __init__(self, device: str, vid: int | None, pid: int | None, serial_number: str) -> None:
        self.device = device
        self.vid = vid
        self.pid = pid
        self.serial_number = serial_number
        self.description = f"Test device on {device}"


def test_discover_filters_by_vid_pid() -> None:
    ports = [
        FakePortInfo("/dev/ttyACM0", 0x2886, 0x8045, "SN_NRF"),  # nRF52840
        FakePortInfo("/dev/ttyACM1", 0x303A, 0x1001, "SN_ESP"),  # ESP32-S3
        FakePortInfo("/dev/ttyUSB0", 0x1234, 0x5678, "SN_OTHER"),  # Unknown
        FakePortInfo("/dev/ttyS0", None, None, ""),  # No VID/PID
    ]

    with patch(
        "blinky_server.transport.discovery.serial.tools.list_ports.comports", return_value=ports
    ):
        devices = discover_serial_devices()

    assert len(devices) == 2
    assert devices[0].platform == "nrf52840"
    assert devices[0].serial_number == "SN_NRF"
    assert devices[1].platform == "esp32s3"
    assert devices[1].serial_number == "SN_ESP"


def test_discover_empty_when_no_devices() -> None:
    with patch(
        "blinky_server.transport.discovery.serial.tools.list_ports.comports", return_value=[]
    ):
        devices = discover_serial_devices()

    assert devices == []
