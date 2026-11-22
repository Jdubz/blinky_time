"""The UF2 drive of a device in its USB bootloader is bound by USB serial."""

from __future__ import annotations

from pathlib import Path

from blinky_server.firmware.uf2_upload import _find_uf2_block_device_for_serial, _mount_point_of


def _fake_drive(root: Path, block: str, vid: str, serial: str) -> None:
    usb_dev = root / "devices" / f"usb-{block}" / "1-1.2"
    target = usb_dev / "1-1.2:1.2" / "host0" / "target0:0:0" / "0:0:0:0"
    target.mkdir(parents=True)
    (usb_dev / "idVendor").write_text(vid + "\n")
    (usb_dev / "serial").write_text(serial + "\n")
    blk = root / "block" / block
    blk.mkdir(parents=True)
    (blk / "device").symlink_to(target)


def test_drive_matched_by_usb_serial(tmp_path: Path) -> None:
    _fake_drive(tmp_path, "sda", "2886", "OTHERDEVICE00001")
    _fake_drive(tmp_path, "sdb", "2886", "04F2A79622BF8B6F")
    sys_block = tmp_path / "block"
    assert _find_uf2_block_device_for_serial("04F2A79622BF8B6F", sys_block) == "/dev/sdb"


def test_drive_missing_or_mismatched_is_none(tmp_path: Path) -> None:
    _fake_drive(tmp_path, "sda", "2886", "OTHERDEVICE00001")
    _fake_drive(tmp_path, "sdc", "1234", "04F2A79622BF8B6F")  # not a Seeed device
    sys_block = tmp_path / "block"
    assert _find_uf2_block_device_for_serial("04F2A79622BF8B6F", sys_block) is None


def test_mount_point_lookup(tmp_path: Path) -> None:
    mounts = tmp_path / "mounts"
    mounts.write_text(
        "/dev/sda /media/jdubz/XIAO-SENSE vfat rw 0 0\n"
        "/dev/sdb /media/jdubz/XIAO\\040SENSE1 vfat rw 0 0\n"
    )
    assert _mount_point_of("/dev/sda", mounts) == "/media/jdubz/XIAO-SENSE"
    assert _mount_point_of("/dev/sdb", mounts) == "/media/jdubz/XIAO SENSE1"
    assert _mount_point_of("/dev/sdz", mounts) is None
