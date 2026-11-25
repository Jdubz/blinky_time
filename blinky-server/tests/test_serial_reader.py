"""The serial reader tells a quiet device from a dead port. An idle read (nothing waiting,
read(1) times out) is normal; the old rule counted those and dropped an unconfigured ESP32-S3
in safe mode (it prints nothing until asked) every ~10 s. Only the dead-port symptom (bytes
reported waiting, read() returns none) or a vanished device node disconnect."""

from __future__ import annotations

import threading
from pathlib import Path

from blinky_server.transport.serial_transport import SerialTransport


class _FakeSerial:
    """Scripted serial port: `script` is a list of (in_waiting, chunk); then idle forever."""

    def __init__(self, script: list[tuple[int, bytes]], stop: threading.Event, after: int) -> None:
        self.script = list(script)
        self.stop = stop
        self.after = after  # stop the loop after this many reads past the script
        self.reads = 0
        self.last_waiting = 0

    @property
    def in_waiting(self) -> int:
        self.last_waiting = self.script[0][0] if self.script else 0
        return self.last_waiting

    def read(self, n: int) -> bytes:
        self.reads += 1
        if self.script:
            return self.script.pop(0)[1]
        if self.reads >= self.after:
            self.stop.set()
        return b""


class _Loop:
    """Records what the reader hands to the event loop."""

    def __init__(self) -> None:
        self.calls: list[str] = []

    def call_soon_threadsafe(self, fn, *args) -> None:  # type: ignore[no-untyped-def]
        self.calls.append(getattr(fn, "__name__", repr(fn)))


def _run(tmp_path: Path, script: list[tuple[int, bytes]], after: int, node: bool = True):
    port = tmp_path / "ttyACM9"
    if node:
        port.write_text("")
    t = SerialTransport(str(port))
    t._serial = _FakeSerial(script, t._stop_event, after)  # type: ignore[assignment]
    t._connected = True
    t._loop = _Loop()  # type: ignore[assignment]
    t._reader_loop()
    return t


def _disconnected(t: SerialTransport) -> bool:
    return "_fire_disconnect" in t._loop.calls  # type: ignore[union-attr]


def test_a_quiet_device_stays_connected(tmp_path: Path) -> None:
    # It spoke once (so the old >100-empty rule applied), then 500 idle reads (~50 s).
    t = _run(tmp_path, [(5, b"hello\n")], after=500)
    # It read through all 500 idle reads: only the test's own stop ended it (the old rule
    # broke out after 100).
    assert t._serial.reads >= 500  # type: ignore[union-attr]
    assert t._stop_event.is_set()


def test_bytes_waiting_but_none_read_is_still_a_dead_port(tmp_path: Path) -> None:
    t = _run(tmp_path, [(5, b"hello\n")] + [(3, b"")] * 150, after=10_000)
    assert not t._stop_event.is_set()  # the reader broke out on its own
    assert _disconnected(t)


def test_a_vanished_device_node_disconnects(tmp_path: Path) -> None:
    t = _run(tmp_path, [], after=10_000, node=False)
    assert not t._stop_event.is_set()  # the reader broke out on its own
    assert _disconnected(t)
