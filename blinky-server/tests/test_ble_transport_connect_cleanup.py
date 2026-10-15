"""BleTransport.connect() releases a half-open client on any failure.

GATT can connect and then start_notify fail. disconnect() is a no-op while
_connected is False, so connect() itself must disconnect the BleakClient,
or it stays connected and blocks the next connection (e.g. the bootloader's).
"""

from __future__ import annotations

from typing import Any, ClassVar

import pytest

from blinky_server.transport import ble_transport as ble_mod


class FakeClient:
    instances: ClassVar[list[FakeClient]] = []

    def __init__(self, *a: Any, **k: Any) -> None:
        self.connected = False
        self.disconnects = 0
        FakeClient.instances.append(self)

    async def connect(self) -> None:
        self.connected = True

    @property
    def mtu_size(self) -> int:
        return 247

    async def start_notify(self, *a: Any, **k: Any) -> None:
        raise RuntimeError("org.bluez.Error.Failed: notify")

    async def disconnect(self) -> None:
        self.disconnects += 1
        self.connected = False


async def test_failed_subscription_disconnects_client(monkeypatch: pytest.MonkeyPatch) -> None:
    FakeClient.instances.clear()
    monkeypatch.setattr(ble_mod, "BleakClient", FakeClient)
    t = ble_mod.BleTransport("AA:BB:CC:DD:EE:10")
    with pytest.raises(RuntimeError):
        await t.connect()
    (client,) = FakeClient.instances
    assert client.disconnects == 1
    assert not client.connected
    assert not t.is_connected
