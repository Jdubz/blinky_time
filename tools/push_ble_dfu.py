#!/usr/bin/env python3
"""Push a DFU.zip to a device already in BLE DFU mode (advertising AdaDFU).

Usage:
  ./push_ble_dfu.py <dfu.zip>             # auto-pick first AdaDFU device
  ./push_ble_dfu.py <dfu.zip> <MAC>       # specific bootloader BLE address (xx:xx:xx:xx:xx:xx)

WARNING: Do NOT wrap this script with a `timeout` shorter than the estimated
transfer time printed at start. Interrupting mid-flash can brick the target
device on both transports (AdaDFU stops advertising; only SWD recovers it).
A 500 KB application DFU takes ~5-6 minutes at default MTU 23.
"""
import asyncio
import logging
import os
import re
import signal
import sys
import time
from pathlib import Path

# BD_ADDR canonical form: 6 hex octets separated by colons, e.g. F2:1B:FD:62:12:C4.
# Bleak accepts upper- or lower-case; we validate then upper() for downstream
# stability. A bad MAC slipping through silently produces wrong arithmetic in
# _bl_minus_one() and a bewildering "device not found" scan failure with no
# breadcrumb pointing at the typo (PR #139 review).
_BLE_MAC_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")

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
    target_mac = sys.argv[2] if len(sys.argv) == 3 else None

    if target_mac is not None:
        if not _BLE_MAC_RE.match(target_mac):
            print(
                f"ERROR: invalid BLE MAC format: {target_mac!r}\n"
                f"       expected six hex octets separated by ':' "
                f"(e.g. F2:1B:FD:62:12:C4)",
                file=sys.stderr,
            )
            return 1
        target_mac = target_mac.upper()

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

    # ── Pre-flight: estimate transfer time + warn loudly ──────────────────
    #
    # Legacy DFU at default MTU 23 → 20-byte data chunks per BLE write.
    # Empirically each chunk is ~8-12 ms round-trip in practice (Nordic
    # legacy DFU + BlueZ host stack, no service-discovery cache, BlueZ
    # default connection interval ~30 ms). That's ~1.7 KB/s sustained
    # throughput. Add ~30 s for connect/scan/erase/verify overhead.
    payload_bytes = _payload_size(dfu_zip)
    bytes_per_sec = 1700.0
    overhead_s = 45.0
    estimated_s = overhead_s + payload_bytes / bytes_per_sec
    recommended_timeout_s = int(estimated_s * 1.5 + 60)
    print()
    print("=" * 68)
    print("  BLE DFU PRE-FLIGHT")
    print("=" * 68)
    print(f"  payload         : {payload_bytes:,} bytes")
    print(f"  estimated time  : {estimated_s/60:.1f} min")
    print(f"  recommended     : `timeout {recommended_timeout_s}` (or no wrapper)")
    print(f"  DO NOT INTERRUPT — SIGTERM mid-flash can brick the target on")
    print(f"  both transports, leaving SWD as the only recovery path.")
    print("=" * 68)
    print()

    # If we *are* sitting under a `timeout` wrapper, the OS will SIGTERM us
    # mid-flash with no recourse. Catch it and at least log the danger before
    # the kernel cleans up — the BLE transaction won't complete but a future
    # operator will at least know what to look for.
    def _on_term(signum, _frame):
        sys.stderr.write(
            f"\n!!! SIGTERM received during BLE DFU — target may be bricked !!!\n"
            f"!!! See tools/push_ble_dfu.py preflight notice !!!\n"
        )
        sys.stderr.flush()
        # Re-raise default behaviour so we still exit; caller's timeout has spoken.
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)

    signal.signal(signal.SIGTERM, _on_term)

    t0 = time.monotonic()
    result = await upload_ble_dfu(
        app_ble_address=app_addr,
        dfu_zip_path=dfu_zip,
        enter_bootloader_via_serial=None,
        enter_bootloader_via_ble=None,
        progress_callback=lambda phase, msg, pct: print(
            f"  [{phase}] {msg}"
            + (f"  ({pct}%)" if pct is not None else "")
            + (f"  [+{time.monotonic() - t0:.0f}s]" if pct is not None else "")
        ),
    )
    print(f"\nResult: {result}")
    return 0 if result.get("status") == "ok" else 3


def _payload_size(zip_path: str) -> int:
    """Return the size in bytes of the firmware payload inside a DFU.zip,
    accounting for the same leading-FF trim _parse_trim does for BL/SD types.
    """
    init_packet, firmware, image_type = _parse_trim(zip_path)
    return len(firmware)


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
