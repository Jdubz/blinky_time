#!/usr/bin/env python3
"""esp32_flash.py - write a blinky ESP32-S3 image over the chip's USB Serial/JTAG (blinky_time's
own tool, not vendored; docs/TEST_BOX.md, "ESP32-S3 flash and recovery").

blinky-server runs it from its single flash path (``firmware/esp32_flash.py``, reached only
through ``FleetManager.flash_device``), with the server's own interpreter, which has the pinned
``esptool`` (blinky-server's ``firmware`` extra). Nothing else should run it on a chip the
server owns.

USB Serial/JTAG reaches the ROM download mode with no BOOT button: esptool's USB-JTAG reset
sequence (DTR/RTS driven by the USB peripheral in silicon) works whatever the firmware is doing,
crashing included. That is what makes a full re-flash the S3's recovery.

The image is the release's merged full-flash image, written at 0x0 (``blinky-things-b<N>-
esp32s3.bin``: second-stage bootloader at 0x0, partition table at 0x8000, boot_app0, the app).
Before anything is written, every one of these must hold, or it exits non-zero and writes
nothing:

  - the image: an ESP image at 0x0 for the ESP32-S3 (magic 0xE9, chip id 9), a partition table
    at 0x8000, and an app partition holding an ESP32-S3 image;
  - the port: USB VID:PID 303a:1001 (Espressif USB Serial/JTAG) and the USB serial the caller
    expects (the chip's MAC, its id on the server);
  - the chip, once in download mode: esptool reports an ESP32-S3 with that MAC, and its flash
    holds the image.

Modes:
  app   (default) everything but the NVS partition, so the device config and settings survive,
        as the nRF52840's settings page survives a UF2 write. Trailing 0xFF is not written.
  full  the whole chip erased, then the image: the recovery re-flash (a crash caused by what NVS
        holds is cleared too; the server then uploads the chip's config again on connect).

esptool verifies every written region (MD5) and then hard-resets the chip into the new app.
Output lines the server reads:
  [PASS] Wrote N / N bytes        the image is on flash, verified
  [PASS] Reset into the app       the chip was reset (it re-enumerates and boots)
  ERROR: ...                      the reason for a refusal or a failure

The port is held with blinky's serial lock (serial_lock.py) for the whole run, as uf2_upload.py
does; the server additionally holds its reconnect off the target and disconnects first.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9  # esp_image_header_t.chip_id (ESP-IDF esp_app_format.h): ESP_CHIP_ID_ESP32S3
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_ENTRY = struct.Struct("<2sBBII16sI")  # magic, type, subtype, offset, size, label, flags
PARTITION_MAGIC = b"\xaa\x50"
PARTITION_MD5_MAGIC = b"\xeb\xeb"
TYPE_APP, TYPE_DATA = 0x00, 0x01
SUBTYPE_FACTORY, SUBTYPE_OTA_0, SUBTYPE_NVS = 0x00, 0x10, 0x02
SECTOR = 0x1000
ESPRESSIF_VID, USB_JTAG_SERIAL_PID = 0x303A, 0x1001
MAC_RE = re.compile(r"^([0-9A-F]{2}:){5}[0-9A-F]{2}$")


class FlashError(Exception):
    pass


@dataclass(frozen=True)
class Partition:
    type: int
    subtype: int
    offset: int
    size: int
    label: str


def esp_chip_id(data: bytes, offset: int = 0) -> int | None:
    """The chip id of the ESP image at ``offset``, or None if there is no ESP image there."""
    if len(data) < offset + 24 or data[offset] != ESP_IMAGE_MAGIC:
        return None
    return int(struct.unpack_from("<H", data, offset + 12)[0])


def partitions(image: bytes) -> list[Partition]:
    """The partition table at 0x8000 (ESP-IDF gen_esp32part.py's binary format)."""
    out: list[Partition] = []
    for i in range(0, 0xC00, PARTITION_ENTRY.size):
        at = PARTITION_TABLE_OFFSET + i
        if at + PARTITION_ENTRY.size > len(image):
            break
        magic, ptype, sub, off, size, label, _flags = PARTITION_ENTRY.unpack_from(image, at)
        if magic != PARTITION_MAGIC:
            break  # the MD5 entry (0xEBEB) or the 0xFF end
        out.append(Partition(ptype, sub, off, size, label.rstrip(b"\0").decode("ascii", "replace")))
    return out


def check_image(image: bytes) -> list[Partition]:
    """Refuse anything but a merged ESP32-S3 full-flash image; return its partition table."""
    chip = esp_chip_id(image)
    if chip is None:
        raise FlashError("not an ESP image at 0x0 (no 0xE9 magic): not a merged ESP32 image")
    if chip != ESP32S3_CHIP_ID:
        raise FlashError(f"the image at 0x0 is for chip id {chip}, not the ESP32-S3 ({ESP32S3_CHIP_ID})")
    parts = partitions(image)
    if not parts:
        raise FlashError("no partition table at 0x8000: an app-only image, not a merged one")
    apps = [p for p in parts if p.type == TYPE_APP and p.subtype in (SUBTYPE_FACTORY, SUBTYPE_OTA_0)]
    if not apps:
        raise FlashError("the partition table has no factory or ota_0 app partition")
    app = min(apps, key=lambda p: p.offset)
    if esp_chip_id(image, app.offset) != ESP32S3_CHIP_ID:
        raise FlashError(f"no ESP32-S3 app image at its app partition {app.label} (0x{app.offset:x})")
    for p in parts:
        if p.offset % SECTOR or p.size % SECTOR:
            raise FlashError(f"partition {p.label} is not sector aligned")
    return parts


def _trimmed_end(image: bytes) -> int:
    """Where the image's content ends: trailing 0xFF dropped, rounded up to a sector."""
    end = len(image.rstrip(b"\xff"))
    return min(len(image), -(-end // SECTOR) * SECTOR)


def write_plan(image: bytes, mode: str) -> list[tuple[int, bytes]]:
    """The (address, bytes) regions to write. ``app``: all but the NVS partition; ``full``:
    the whole image (after a full chip erase)."""
    parts = check_image(image)
    end = _trimmed_end(image)
    if mode == "full":
        return [(0, image[:end])]
    if mode != "app":
        raise FlashError(f"unknown mode {mode!r}")
    nvs = [p for p in parts if p.type == TYPE_DATA and p.subtype == SUBTYPE_NVS]
    if not nvs:
        raise FlashError("the partition table has no NVS partition: nothing to keep, use --mode full")
    regions: list[tuple[int, bytes]] = []
    start = 0
    for p in sorted(nvs, key=lambda p: p.offset):
        if p.offset > start:
            regions.append((start, image[start : min(p.offset, end)]))
        start = p.offset + p.size
    if start < end:
        regions.append((start, image[start:end]))
    return [(a, d) for a, d in regions if d]


def port_identity(port: str) -> tuple[int | None, int | None, str | None]:
    """(VID, PID, USB serial) of a serial port, by-id path or tty."""
    from serial.tools import list_ports

    real = str(Path(port).resolve())
    for info in list_ports.comports():
        if str(Path(info.device).resolve()) == real:
            return info.vid, info.pid, info.serial_number
    return None, None, None


def check_port(port: str, expect_mac: str) -> None:
    vid, pid, serial = port_identity(port)
    if (vid, pid) != (ESPRESSIF_VID, USB_JTAG_SERIAL_PID):
        got = f"{vid:04x}:{pid:04x}" if vid is not None and pid is not None else "not found"
        raise FlashError(f"{port} is {got}, not an ESP32-S3's USB Serial/JTAG (303a:1001)")
    if (serial or "").upper() != expect_mac:
        raise FlashError(f"{port}'s USB serial is {serial!r}, not the expected {expect_mac}")


def _mac(raw: tuple[int, ...] | None) -> str:
    return ":".join(f"{b:02X}" for b in raw or ())


def flash(port: str, image_path: Path, expect_mac: str, mode: str) -> None:
    image = image_path.read_bytes()
    plan = write_plan(image, mode)  # refuses a wrong image before the chip is touched
    check_port(port, expect_mac)
    total = sum(len(d) for _, d in plan)
    print(f"  image {image_path.name}: {len(image)} bytes, mode {mode}, "
          + ", ".join(f"0x{a:x}+{len(d)}" for a, d in plan), flush=True)

    import serial_lock

    if not serial_lock.acquire(port, "esp32_flash", "firmware_flash", hold_seconds=300):
        _locked, holder = serial_lock.is_locked(port)
        raise FlashError(f"{port} is locked by {holder or 'another process'}")
    try:
        _write(str(Path(port).resolve()), plan, total, image, expect_mac, mode)
    finally:
        serial_lock.release(port)


def _write(port: str, plan: list[tuple[int, bytes]], total: int, image: bytes,
           expect_mac: str, mode: str) -> None:
    from esptool.cmds import attach_flash, detect_chip, detect_flash_size, erase_flash
    from esptool.cmds import reset_chip, run_stub, write_flash
    from esptool.util import flash_size_bytes

    esp = detect_chip(port, connect_mode="default-reset")  # USB-JTAG reset into download mode
    try:
        if esp.CHIP_NAME != "ESP32-S3":
            raise FlashError(f"the chip on {port} is an {esp.CHIP_NAME}, not an ESP32-S3")
        mac = _mac(esp.read_mac())
        if mac != expect_mac:
            raise FlashError(f"the chip's MAC is {mac}, not the expected {expect_mac}")
        print(f"[PASS] Chip ESP32-S3 {mac} in download mode", flush=True)
        esp = run_stub(esp)
        attach_flash(esp)
        size_name = detect_flash_size(esp)
        size = flash_size_bytes(size_name) if size_name else None
        if not size:
            raise FlashError("could not read the chip's flash size")
        end = max(a + len(d) for a, d in plan)
        if end > size:
            raise FlashError(f"the image needs {end} bytes, the chip's flash is {size_name}")
        esp.flash_set_parameters(size)  # what esptool's "keep" does: the header stays as built
        if mode == "full":
            erase_flash(esp)
            print("[PASS] Erased the whole flash", flush=True)
        write_flash(esp, plan, no_progress=True)  # verifies each region's MD5
        print(f"[PASS] Wrote {total:,} / {total:,} bytes", flush=True)
        reset_chip(esp, "hard-reset")
        print("[PASS] Reset into the app", flush=True)
    finally:
        try:
            esp._port.close()
        except Exception:  # noqa: BLE001 - closing a port the reset may have dropped
            pass


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="the chip's serial port (by-id path or tty)")
    ap.add_argument("--image", required=True, type=Path, help="the merged ESP32-S3 image")
    ap.add_argument("--expect-mac", required=True, help="the chip's MAC (its USB serial)")
    ap.add_argument("--mode", choices=("app", "full"), default="app")
    ap.add_argument("--check-image", action="store_true",
                    help="only check the image and print the write plan; no chip is touched")
    args = ap.parse_args(argv)
    mac = args.expect_mac.upper()
    try:
        if not MAC_RE.match(mac):
            raise FlashError(f"--expect-mac {args.expect_mac!r} is not a MAC")
        if args.check_image:
            plan = write_plan(args.image.read_bytes(), args.mode)
            print(" ".join(f"0x{a:x}+{len(d)}" for a, d in plan))
            return 0
        flash(args.port, args.image, mac, args.mode)
    except (FlashError, OSError) as e:
        print(f"ERROR: {e}", flush=True)
        return 1
    except Exception as e:  # noqa: BLE001 - esptool's FatalError and serial errors, said plainly
        print(f"ERROR: {type(e).__name__}: {e}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
