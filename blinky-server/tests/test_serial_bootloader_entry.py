"""Serial bootloader entry falls back to BLE NUS when the serial link is dead (#218).

After an INVALID_STATE reset the device can reboot into its app and USB
re-enumerates, leaving the serial transport the flash job bound at start
dead. Re-entry must then reach the app over BLE NUS, not fail on the stale
port.
"""

from __future__ import annotations

import pytest

from blinky_server.device import manager as manager_mod
from blinky_server.transport import ble_transport as ble_mod

BLE = "AA:BB:CC:DD:EE:10"


class FakeSerial:
    def __init__(self, connected: bool) -> None:
        self.is_connected = connected


class FakeProtocol:
    def __init__(self, exc: BaseException | None = None) -> None:
        self.sent: list[str] = []
        self.exc = exc

    async def send_command(self, cmd: str, timeout: float | None = None) -> str:
        self.sent.append(cmd)
        if self.exc:
            raise self.exc
        return ""


@pytest.fixture
def ble_log(monkeypatch: pytest.MonkeyPatch) -> list[tuple[str, str]]:
    log: list[tuple[str, str]] = []

    class FakeBle:
        def __init__(self, addr: str) -> None:
            self.addr = addr

        async def connect(self) -> None:
            log.append(("connect", self.addr))

        async def write_line(self, line: str) -> None:
            log.append(("write", line))

        async def disconnect(self) -> None:
            log.append(("disconnect", self.addr))

    async def no_sleep(_s: float) -> None:
        return None

    monkeypatch.setattr(ble_mod, "BleTransport", FakeBle)
    monkeypatch.setattr(manager_mod.asyncio, "sleep", no_sleep)
    return log


async def test_live_serial_is_used(ble_log: list[tuple[str, str]]) -> None:
    proto = FakeProtocol()
    enter = manager_mod._make_serial_bootloader_entry(proto, FakeSerial(True), BLE, "dev")
    await enter("bootloader ble")
    assert proto.sent == ["bootloader ble"]
    assert ble_log == []


async def test_disconnected_serial_falls_back_to_ble(ble_log: list[tuple[str, str]]) -> None:
    proto = FakeProtocol()
    enter = manager_mod._make_serial_bootloader_entry(proto, FakeSerial(False), BLE, "dev")
    await enter("bootloader ble")
    assert proto.sent == []
    assert ble_log == [("connect", BLE), ("write", "bootloader ble"), ("disconnect", BLE)]


async def test_stale_serial_error_falls_back_to_ble(ble_log: list[tuple[str, str]]) -> None:
    proto = FakeProtocol(exc=ConnectionError("port gone"))
    enter = manager_mod._make_serial_bootloader_entry(proto, FakeSerial(True), BLE, "dev")
    await enter("bootloader ble")
    assert ble_log == [("connect", BLE), ("write", "bootloader ble"), ("disconnect", BLE)]


async def test_serial_timeout_does_not_fall_back(ble_log: list[tuple[str, str]]) -> None:
    # Attempt 0: the device reboots on the command, so no reply is normal.
    proto = FakeProtocol(exc=TimeoutError())
    enter = manager_mod._make_serial_bootloader_entry(proto, FakeSerial(True), BLE, "dev")
    with pytest.raises(TimeoutError):
        await enter("bootloader ble")
    assert ble_log == []
