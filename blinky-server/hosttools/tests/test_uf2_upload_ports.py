# Ported from Jdubz/blinky-firmware tests/tools/test_uf2_upload_ports.py at commit
# 33e005982c34e7c3664215cacf767bd20910deb2, for blinky-server's own copy in hosttools/
# (2026-11-24 decoupling). Changes are marked "[blinky_time: ...]".
"""uf2_upload.py finds blinky-server's device whichever form of the port either side uses.

The server reports its stable /dev/serial/by-id/... symlink and the tool is usually given the
/dev/ttyACMn it points to. Comparing only the tool's own path missed every server device, so
the release (and its reconnect hold) never happened, and the server reopened the port while
the tool was entering the bootloader (test box, 2026-11-23).
"""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path
from typing import Any

import pytest

# [blinky_time: hosttools/, the vendored copies]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import uf2_upload  # noqa: E402


@pytest.fixture
def ports(tmp_path: Path) -> tuple[str, str]:
    """A stand-in tty node and a by-id style symlink to it."""
    tty = tmp_path / "ttyACM0"
    tty.write_text("")
    by_id = tmp_path / "usb-Seeed_XIAO_nRF52840_Sense_0123456789ABCDEF-if00"
    by_id.symlink_to(tty)
    return str(tty), str(by_id)


def test_same_port_resolves_both_sides(ports: tuple[str, str], tmp_path: Path) -> None:
    tty, by_id = ports
    assert uf2_upload._same_port(by_id, tty)
    assert uf2_upload._same_port(tty, by_id)
    assert uf2_upload._same_port(tty, tty)
    assert not uf2_upload._same_port(by_id, str(tmp_path / "ttyACM1"))
    assert not uf2_upload._same_port("", tty)


class _Server:
    """urlopen as a fake blinky-server: one device on the by-id port; records POSTs."""

    def __init__(self, port: str) -> None:
        self.port = port
        self.posts: list[tuple[str, Any]] = []

    def __call__(self, req: Any, timeout: float = 0) -> io.BytesIO:
        url = req.full_url
        if url.endswith("/api/devices"):
            return io.BytesIO(json.dumps([{"id": "0123456789ABCDEF", "port": self.port}]).encode())
        self.posts.append((url, json.loads(req.data or b"{}")))
        return io.BytesIO(b"{}")


def test_release_matches_the_servers_by_id_port_given_the_tty(
    ports: tuple[str, str], monkeypatch: pytest.MonkeyPatch
) -> None:
    tty, by_id = ports
    server = _Server(by_id)
    monkeypatch.setattr(uf2_upload, "urlopen", server)
    assert uf2_upload._request_server_release(tty, hold_seconds=90)
    assert server.posts == [
        (f"{uf2_upload.BLINKY_SERVER_URL}/api/devices/0123456789ABCDEF/release", {"hold_seconds": 90})
    ]


def test_reconnect_matches_the_servers_by_id_port_given_the_tty(
    ports: tuple[str, str], monkeypatch: pytest.MonkeyPatch
) -> None:
    tty, by_id = ports
    server = _Server(by_id)
    monkeypatch.setattr(uf2_upload, "urlopen", server)
    uf2_upload._request_server_reconnect(tty)
    assert [u for u, _ in server.posts] == [
        f"{uf2_upload.BLINKY_SERVER_URL}/api/devices/0123456789ABCDEF/reconnect"
    ]


# -- bootloader entry: a serial error after `bootloader` was sent ------------------------------


class _Port:
    """A serial handle whose flush fails like a chip resetting under it (termios EIO)."""

    def __init__(self, fail_on: str) -> None:
        self.fail_on = fail_on
        self.writes: list[bytes] = []

    def reset_input_buffer(self) -> None:
        if self.fail_on == "reset":
            raise uf2_upload._TERMIOS_ERRORS[0](5, "Input/output error")

    def write(self, data: bytes) -> None:
        self.writes.append(data)

    def flush(self) -> None:
        if self.fail_on == "flush":
            raise uf2_upload._TERMIOS_ERRORS[0](5, "Input/output error")

    def read(self, _n: int) -> bytes:
        return b""

    def close(self) -> None:
        pass


def _bootloader_entry(monkeypatch: pytest.MonkeyPatch, fail_on: str, drives: list[bool]) -> Any:
    """trigger_bootloader with the host side faked: each attempt opens a fresh _Port, and
    `drives` says whether each UF2 drive check finds the drive."""
    if not uf2_upload._TERMIOS_ERRORS:
        pytest.skip("termios is POSIX-only")
    import serial_lock

    opened: list[_Port] = []

    def open_port(*_a: Any, **_k: Any) -> _Port:
        opened.append(_Port(fail_on))
        return opened[-1]

    checks = iter(drives)
    monkeypatch.setattr(uf2_upload, "_request_server_release", lambda *a, **k: True)
    monkeypatch.setattr(serial_lock, "acquire", lambda *a, **k: True)
    monkeypatch.setattr(uf2_upload, "_check_port_available", lambda *a, **k: True)
    monkeypatch.setattr(uf2_upload, "get_serial_number", lambda _p: "0123456789ABCDEF")
    monkeypatch.setattr(uf2_upload, "_find_usb_hub_port", lambda _p: (None, None))
    monkeypatch.setattr(uf2_upload, "_get_usb_block_devices", lambda: set())
    monkeypatch.setattr(uf2_upload, "find_port_by_serial", lambda *a, **k: "/dev/ttyACM0")
    monkeypatch.setattr(uf2_upload, "_serial_open_with_timeout", open_port)
    monkeypatch.setattr(uf2_upload, "_wait_for_uf2_drive", lambda *a, **k: next(checks, False))
    monkeypatch.setattr(uf2_upload.time, "sleep", lambda _s: None)
    monkeypatch.setattr(uf2_upload, "MAX_BOOTLOADER_RETRIES", 2)
    return uf2_upload.trigger_bootloader("/dev/ttyACM0"), opened


def test_an_error_after_sending_bootloader_still_checks_for_the_drive(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The chip took `bootloader`, reset and broke the port mid-flush: its drive is there."""
    serial, opened = _bootloader_entry(monkeypatch, "flush", drives=[True])
    assert serial == "0123456789ABCDEF"
    assert len(opened) == 1 and opened[0].writes == [b"bootloader\n"]


def test_eio_before_sending_retries_instead_of_aborting(monkeypatch: pytest.MonkeyPatch) -> None:
    """A stale port (EIO on the first call) is retried, not an uncaught crash."""
    serial, opened = _bootloader_entry(monkeypatch, "reset", drives=[])
    assert serial is None  # both attempts hit EIO; the caller reports the failure
    assert len(opened) == 2 and all(p.writes == [] for p in opened)
