"""_find_usb_reset_tool: $BLINKY_USB_RESET_TOOL pins a root-owned copy."""

from __future__ import annotations

import logging
from pathlib import Path

import pytest

from blinky_server.firmware.uf2_upload import _find_usb_reset_tool


def test_repo_copy_without_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("BLINKY_USB_RESET_TOOL", raising=False)
    tool = _find_usb_reset_tool()
    assert tool is not None
    assert tool.parts[-2:] == ("tools", "usb_reset.py")


def test_env_path_is_used(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    pinned = tmp_path / "usb_reset.py"
    pinned.write_text("")
    monkeypatch.setenv("BLINKY_USB_RESET_TOOL", str(pinned))
    assert _find_usb_reset_tool() == pinned


def test_missing_env_path_fails_loud(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    # Never falls back to the user-writable repo copy when a pinned path was asked for.
    monkeypatch.setenv("BLINKY_USB_RESET_TOOL", str(tmp_path / "missing.py"))
    with caplog.at_level(logging.ERROR):
        assert _find_usb_reset_tool() is None
    assert "BLINKY_USB_RESET_TOOL" in caplog.text
