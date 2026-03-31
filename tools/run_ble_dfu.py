#!/usr/bin/env python3
"""Run BLE DFU transfer using ble_dfu.py module directly.

Usage: python3 run_ble_dfu.py <app_ble_address> <dfu_zip_path> [serial_port]
"""
import asyncio
import logging
import sys
import time

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)-5s %(message)s",
    datefmt="%H:%M:%S",
)

sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent.parent / 'blinky-server'))

from blinky_server.ota.ble_dfu import upload_ble_dfu


def progress(phase, msg, pct=None):
    pct_str = f" ({pct}%)" if pct is not None else ""
    print(f"[{phase}{pct_str}] {msg}", flush=True)


async def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    app_addr = sys.argv[1]
    dfu_zip = sys.argv[2]
    serial_port = sys.argv[3] if len(sys.argv) > 3 else None

    t0 = time.monotonic()
    result = await upload_ble_dfu(
        app_ble_address=app_addr,
        dfu_zip_path=dfu_zip,
        # No bootloader entry — device must already be in bootloader mode
        progress_callback=progress,
    )
    elapsed = time.monotonic() - t0

    print()
    print("=" * 60)
    print(f"Status:   {result.get('status')}")
    print(f"Message:  {result.get('message')}")
    print(f"Elapsed:  {elapsed:.1f}s")
    print(f"Verified: {result.get('verified', 'N/A')}")
    print("=" * 60)

    if result.get("status") == "ok" and serial_port:
        await asyncio.sleep(5)
        import serial
        for attempt in range(3):
            try:
                ser = serial.Serial(serial_port, 115200, timeout=5)
                await asyncio.sleep(3)
                ser.reset_input_buffer()
                ser.read(4096)
                await asyncio.sleep(0.5)
                ser.write(b"json info\r\n")
                ser.flush()
                await asyncio.sleep(2)
                resp = ser.read(4096).decode("utf-8", errors="replace")
                for line in resp.split("\n"):
                    if "version" in line:
                        print(f"\nPOST-DFU: {line.strip()}")
                        ser.close()
                        sys.exit(0)
                ser.close()
            except Exception as e:
                print(f"Serial attempt {attempt+1}: {e}")
                await asyncio.sleep(5)

    sys.exit(0 if result.get("status") == "ok" else 1)


if __name__ == "__main__":
    asyncio.run(main())
