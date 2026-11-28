"""esp32_flash.py refuses every image and port that isn't a merged ESP32-S3 image and the chip it
was asked for, and keeps the NVS partition (device config) out of a routine write."""

from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import esp32_flash as ef  # noqa: E402

MAC = "02:00:5E:10:00:01"


def _header(chip_id: int) -> bytes:
    h = bytearray(24)
    h[0] = 0xE9
    h[1] = 3
    struct.pack_into("<H", h, 12, chip_id)
    return bytes(h)


def _entry(ptype: int, sub: int, off: int, size: int, label: bytes) -> bytes:
    return ef.PARTITION_ENTRY.pack(ef.PARTITION_MAGIC, ptype, sub, off, size, label, 0)


def merged(*, boot_chip: int = 9, app_chip: int = 9, table: bool = True, nvs: bool = True,
           size: int = 0x400000, app_len: int = 0x12345) -> bytes:
    img = bytearray(b"\xff" * size)
    img[0:24] = _header(boot_chip)
    img[24:0x5000] = b"\x11" * (0x5000 - 24)  # the bootloader
    if table:
        rows = []
        if nvs:
            rows.append(_entry(1, 2, 0x9000, 0x5000, b"nvs"))
        rows += [
            _entry(1, 0, 0xE000, 0x2000, b"otadata"),
            _entry(0, 0x10, 0x10000, 0x140000, b"app0"),
            _entry(0, 0x11, 0x150000, 0x140000, b"app1"),
        ]
        rows.append(b"\xeb\xeb" + b"\xff" * 14 + b"\x00" * 16)  # the MD5 entry
        blob = b"".join(rows)
        img[0x8000 : 0x8000 + len(blob)] = blob
    img[0xE000:0xE020] = b"\x00" * 32  # boot_app0
    img[0x10000 : 0x10000 + 24] = _header(app_chip)
    img[0x10000 + 24 : 0x10000 + app_len] = b"\x22" * (app_len - 24)
    return bytes(img)


def test_a_merged_s3_image_writes_everything_but_nvs() -> None:
    img = merged()
    plan = ef.write_plan(img, "app")
    assert [(a, len(d)) for a, d in plan] == [(0, 0x9000), (0xE000, 0x15000)]
    assert plan[0][1] == img[:0x9000] and plan[1][1] == img[0xE000:0x23000]
    # NVS (0x9000-0xE000) is never in a routine write.
    assert all(not (a < 0xE000 and a + len(d) > 0x9000) for a, d in plan)


def test_full_mode_writes_the_whole_image_trimmed() -> None:
    img = merged()
    assert [(a, len(d)) for a, d in ef.write_plan(img, "full")] == [(0, 0x23000)]


@pytest.mark.parametrize(
    ("img", "why"),
    [
        (b":10000000" + b"0" * 2000, "no 0xE9 magic"),
        (merged(boot_chip=0), "chip id 0, not the ESP32-S3"),  # an ESP32
        (merged(boot_chip=5), "chip id 5"),  # an ESP32-C3
        (merged(table=False), "app-only image"),
        (merged(app_chip=2), "no ESP32-S3 app image"),
    ],
)
def test_wrong_images_are_refused(img: bytes, why: str) -> None:
    with pytest.raises(ef.FlashError, match=why):
        ef.write_plan(img, "app")


def test_an_app_only_bin_is_refused() -> None:
    """The release's app-only bin starts like the merged image (0xE9, chip id 9) but has no
    partition table: written at 0x0 it would overwrite the bootloader."""
    app_only = merged()[0x10000:]
    with pytest.raises(ef.FlashError, match="app-only"):
        ef.write_plan(app_only, "full")


def test_no_nvs_partition_refuses_a_routine_write() -> None:
    with pytest.raises(ef.FlashError, match="no NVS"):
        ef.write_plan(merged(nvs=False), "app")
    assert ef.write_plan(merged(nvs=False), "full")


@pytest.mark.parametrize(
    ("ident", "why"),
    [
        ((0x2886, 0x8045, "0123456789ABCDEF"), "not an ESP32-S3"),  # the nRF52840 test chip
        ((0x303A, 0x1002, MAC), "not an ESP32-S3"),  # an Espressif CMSIS-DAP (the Debug Mate)
        ((0x303A, 0x1001, "AA:BB:CC:DD:EE:FF"), "not the expected"),  # another S3
        ((None, None, None), "not found"),
    ],
)
def test_the_port_must_be_the_expected_s3(
    monkeypatch: pytest.MonkeyPatch, ident: tuple[Any, Any, Any], why: str
) -> None:
    monkeypatch.setattr(ef, "port_identity", lambda port: ident)
    with pytest.raises(ef.FlashError, match=why):
        ef.check_port("/dev/ttyACM1", MAC)


def test_the_expected_port_passes(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(ef, "port_identity", lambda port: (0x303A, 0x1001, MAC.lower()))
    ef.check_port("/dev/ttyACM1", MAC)


def test_a_wrong_image_never_reaches_the_port(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    touched: list[str] = []
    monkeypatch.setattr(ef, "port_identity", lambda port: touched.append(port) or (0, 0, ""))
    bad = tmp_path / "x.bin"
    bad.write_bytes(merged(boot_chip=0))
    assert ef.main(["--port", "/dev/ttyACM1", "--image", str(bad), "--expect-mac", MAC]) == 1
    assert touched == []
    assert "ERROR: the image at 0x0 is for chip id 0" in capsys.readouterr().out


def test_check_image_prints_the_plan(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    p = tmp_path / "m.bin"
    p.write_bytes(merged())
    assert ef.main(["--port", "-", "--image", str(p), "--expect-mac", MAC, "--check-image"]) == 0
    assert capsys.readouterr().out.strip() == "0x0+36864 0xe000+86016"


def test_bad_mac_argument(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    p = tmp_path / "m.bin"
    p.write_bytes(merged())
    assert ef.main(["--port", "-", "--image", str(p), "--expect-mac", "0123456789ABCDEF"]) == 1
    assert "is not a MAC" in capsys.readouterr().out
