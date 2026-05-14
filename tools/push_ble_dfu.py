#!/usr/bin/env python3
"""Push a DFU.zip to a device already in BLE DFU mode (advertising AdaDFU).

Usage:
  ./push_ble_dfu.py <dfu.zip>             # auto-pick first AdaDFU device
  ./push_ble_dfu.py <dfu.zip> <MAC>       # specific bootloader BLE address (xx:xx:xx:xx:xx:xx)
"""
import asyncio
import logging
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "blinky-server"))

from bleak import BleakScanner  # noqa: E402
from blinky_server.firmware import ble_dfu as ble_dfu_mod  # noqa: E402
from blinky_server.firmware.ble_dfu import upload_ble_dfu  # noqa: E402

# Patch parse_dfu_zip to strip the leading FF padding adafruit-nrfutil emits
# when it pads the hex-to-bin conversion from address 0 up to the bootloader's
# load address. The Legacy DFU protocol declares the firmware size to the
# bootloader as len(firmware); if that's the padded 1 MB it fails START_DFU
# with DATA_SIZE_EXCEEDS_LIMIT (result=0x04) because the bootloader region is
# < 50 KB. The actual bootloader content sits at the tail of the bin.
_orig_parse = ble_dfu_mod.parse_dfu_zip


def _crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _parse_trim(zip_path):
    import struct

    init_packet, firmware, image_type = _orig_parse(zip_path)
    if image_type in (0x02, 0x01):  # bootloader or softdevice
        i = 0
        n = len(firmware)
        while i < n and firmware[i] == 0xff:
            i += 1
        if i > 0:
            print(f"[trim] stripping {i} leading 0xFF bytes from image_type=0x{image_type:02x} firmware ({n} -> {n - i} bytes)")
            firmware = firmware[i:]
            # Patch init packet's CRC16 field (last 2 bytes, little-endian)
            # to match the trimmed firmware. adafruit-nrfutil computed it
            # over the padded bin.
            new_crc = _crc16_ccitt(firmware)
            old_crc = struct.unpack_from("<H", init_packet, len(init_packet) - 2)[0]
            patched = bytearray(init_packet)
            struct.pack_into("<H", patched, len(patched) - 2, new_crc)
            init_packet = bytes(patched)
            print(f"[trim] patched init packet CRC16: 0x{old_crc:04x} -> 0x{new_crc:04x}")
    return init_packet, firmware, image_type


ble_dfu_mod.parse_dfu_zip = _parse_trim

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")


def _bl_minus_one(boot_mac: str) -> str:
    # upload_ble_dfu expects the APP-mode BLE address. Bootloader address =
    # app + 1 (last octet). So app = bootloader - 1.
    parts = boot_mac.split(":")
    last = int(parts[-1], 16)
    parts[-1] = f"{(last - 1) & 0xff:02X}"
    return ":".join(parts)


async def scan_for_adadfu(timeout: float = 8.0) -> list[tuple[str, int]]:
    """Return list of (mac, rssi) for nearby AdaDFU advertisements."""
    out: list[tuple[str, int]] = []

    def handle(device, advert):
        name = (advert.local_name or device.name or "") if advert else (device.name or "")
        if name and "AdaDFU" in name:
            out.append((device.address, advert.rssi if advert else -100))

    async with BleakScanner(detection_callback=handle):
        await asyncio.sleep(timeout)
    # dedupe by mac, keep best rssi
    best: dict[str, int] = {}
    for mac, rssi in out:
        if mac not in best or rssi > best[mac]:
            best[mac] = rssi
    return sorted(best.items(), key=lambda x: x[1], reverse=True)


async def main() -> int:
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print(__doc__, file=sys.stderr)
        return 1
    dfu_zip = sys.argv[1]
    target_mac = sys.argv[2].upper() if len(sys.argv) == 3 else None

    if not Path(dfu_zip).is_file():
        print(f"DFU zip not found: {dfu_zip}", file=sys.stderr)
        return 1

    if target_mac is None:
        print("Scanning for AdaDFU advertisements (8s)...")
        results = await scan_for_adadfu()
        if not results:
            print("No AdaDFU device found. Is the bootloader in DFU mode?", file=sys.stderr)
            return 2
        for mac, rssi in results:
            print(f"  {mac}  rssi={rssi}")
        target_mac = results[0][0]
        print(f"Picking {target_mac} (best signal)")

    app_addr = _bl_minus_one(target_mac)
    print(f"Bootloader MAC: {target_mac}  (app-mode MAC: {app_addr})")
    print(f"Pushing: {dfu_zip}")

    result = await upload_ble_dfu(
        app_ble_address=app_addr,
        dfu_zip_path=dfu_zip,
        enter_bootloader_via_serial=None,
        enter_bootloader_via_ble=None,
        progress_callback=lambda phase, msg, pct: print(f"  [{phase}] {msg}" + (f"  ({pct}%)" if pct is not None else "")),
    )
    print(f"\nResult: {result}")
    return 0 if result.get("status") == "ok" else 3


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
