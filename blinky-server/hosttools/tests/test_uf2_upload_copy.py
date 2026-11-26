"""uf2_upload.py's copy is a pass only once the bootloader has the bytes.

os.write() fills the page cache; the bytes reach the bootloader during fsync. On
2026-11-25 (job ecc8a4780e9f) a USB reset hit the bootloader 3 s into that writeback,
fsync failed, and the tool still printed ``[PASS] Wrote N / N`` from the byte count.
"""

from __future__ import annotations

import errno
import os
import sys
from pathlib import Path
from types import ModuleType
from typing import Any

import pytest

# [blinky_time: hosttools/, the vendored copies]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import uf2_upload  # noqa: E402


@pytest.fixture
def tool() -> ModuleType:
    return uf2_upload


def _drive(tmp_path: Path) -> tuple[Path, Path]:
    mount = tmp_path / "mnt"
    mount.mkdir()
    (mount / "INFO_UF2.TXT").write_text("UF2 Bootloader 0.9.2\n")
    uf2 = tmp_path / "fw.uf2"
    uf2.write_bytes(b"\x55" * 4096)
    return mount, uf2


def _fail_fsync(monkeypatch: pytest.MonkeyPatch) -> None:
    def boom(_fd: int) -> None:
        raise OSError(errno.EIO, "Input/output error")

    monkeypatch.setattr(os, "fsync", boom)


def test_copy_passes_when_fsync_succeeds(tool: ModuleType, tmp_path: Path, capsys: Any) -> None:
    mount, uf2 = _drive(tmp_path)
    assert tool.copy_firmware(uf2, mount) is True
    assert "[PASS] Wrote 4,096 / 4,096 bytes" in capsys.readouterr().out


def test_copy_fails_when_fsync_fails_and_the_drive_stays(
    tool: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: Any
) -> None:
    mount, uf2 = _drive(tmp_path)
    _fail_fsync(monkeypatch)
    monkeypatch.setattr(tool, "SYNC_FAIL_GRACE_S", 0.0)
    assert tool.copy_firmware(uf2, mount) is False
    out = capsys.readouterr().out
    assert "[PASS]" not in out
    assert "[FAIL] Write not confirmed" in out


def test_copy_passes_when_fsync_fails_because_the_drive_left(
    tool: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: Any
) -> None:
    """The bootloader got every block and rebooted under the sync: expected, a pass."""
    mount, uf2 = _drive(tmp_path)
    _fail_fsync(monkeypatch)
    monkeypatch.setattr(tool, "SYNC_FAIL_GRACE_S", 0.0)
    monkeypatch.setattr(tool, "_drive_present", lambda *_a, **_k: False)
    assert tool.copy_firmware(uf2, mount) is True
    assert "[PASS] Wrote 4,096 / 4,096 bytes" in capsys.readouterr().out


def test_drive_presence_follows_the_block_device(tool: ModuleType, tmp_path: Path) -> None:
    mounts = tmp_path / "mounts"
    mounts.write_text("/dev/sdf /mnt/uf2\\040upload vfat rw 0 0\n")
    assert tool._mount_source("/mnt/uf2 upload", str(mounts)) == "/dev/sdf"
    assert tool._mount_source("/mnt/other", str(mounts)) is None
    sys_block = tmp_path / "block"
    (sys_block / "sdf").mkdir(parents=True)
    (sys_block / "sdf" / "size").write_text("65801\n")
    assert tool._drive_present("/mnt/uf2", "/dev/sdf", str(sys_block)) is True
    (sys_block / "sdf" / "size").write_text("0\n")  # capacity dropped on disconnect
    assert tool._drive_present("/mnt/uf2", "/dev/sdf", str(sys_block)) is False
    assert tool._drive_present("/mnt/uf2", "/dev/sdg", str(sys_block)) is False


def test_stale_mounts_are_released_down_to_a_live_drive(
    tool: ModuleType, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Every flash used to leave its gone drive mounted, stacked on /mnt/uf2-upload."""
    mounts = tmp_path / "mounts"
    stack = ["/dev/sda", "/dev/sdb", "/dev/sdc"]  # sda is live, sdb and sdc are gone

    def write_mounts() -> None:
        mounts.write_text("".join(f"{d} /mnt/uf2-upload vfat rw 0 0\n" for d in stack))

    write_mounts()
    calls: list[list[str]] = []

    def fake_run(cmd: list[str], **_: Any) -> Any:
        calls.append(cmd)
        stack.pop()
        write_mounts()
        return type("R", (), {"returncode": 0})()

    monkeypatch.setattr(tool.subprocess, "run", fake_run)
    monkeypatch.setattr(tool, "_drive_present", lambda _mp, dev=None: dev == "/dev/sda")
    assert tool.release_stale_mounts("/mnt/uf2-upload", str(mounts)) == 2
    assert calls == [["sudo", "-n", "umount", "-l", "/mnt/uf2-upload"]] * 2
    assert stack == ["/dev/sda"]
    assert tool.release_stale_mounts("/mnt/elsewhere", str(mounts)) == 0
