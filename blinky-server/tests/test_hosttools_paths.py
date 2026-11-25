"""The server runs its own host tools (blinky-server/hosttools/), never a firmware checkout's
(2026-11-24 decoupling): these resolve with no firmware/ submodule present."""

from __future__ import annotations

from pathlib import Path

from blinky_server.firmware import uf2_upload
from blinky_server.paths import hosttools_dir


def test_hosttools_dir_is_next_to_the_package() -> None:
    d = hosttools_dir()
    assert d == Path(uf2_upload.__file__).resolve().parents[2] / "hosttools"
    assert (d / "firmware_release.py").is_file()


def test_the_flash_tool_and_usb_reset_come_from_hosttools() -> None:
    assert uf2_upload._find_uf2_tool() == hosttools_dir() / "uf2_upload.py"
    assert uf2_upload._find_usb_reset_tool() == hosttools_dir() / "usb_reset.py"
