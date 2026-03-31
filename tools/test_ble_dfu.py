#!/usr/bin/env python3
"""Test BLE DFU end-to-end on a bare test chip.

Sends 'bootloader ble' via serial, then performs BLE DFU transfer.
The device must have USB serial available as recovery backup.

Usage:
    python3 test_ble_dfu.py <serial_port> <ble_address> <dfu_zip_path>

Example:
    python3 test_ble_dfu.py /dev/ttyACM2 E3:8D:10:5F:17:66 /tmp/blinky-build-nrf-dahmxf2a/blinky-things.ino.dfu.zip
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
log = logging.getLogger(__name__)

# Add parent dir to path for imports
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent.parent / 'blinky-server'))


async def send_bootloader_via_serial(serial_port: str) -> None:
    """Send 'bootloader ble' command via serial and close the port."""
    import serial
    log.info("Opening serial port %s...", serial_port)
    ser = serial.Serial(serial_port, 115200, timeout=2)
    time.sleep(0.5)  # Wait for port to settle

    # Drain any pending output
    ser.reset_input_buffer()

    log.info("Sending 'bootloader ble' command...")
    ser.write(b"bootloader ble\n")
    ser.flush()

    # Read response (device will print message then reset)
    time.sleep(0.3)
    response = b""
    while ser.in_waiting:
        response += ser.read(ser.in_waiting)
        time.sleep(0.1)
    if response:
        log.info("Device response: %s", response.decode('utf-8', errors='replace').strip())

    ser.close()
    log.info("Serial port closed. Device should be entering BLE DFU bootloader...")


async def verify_serial_alive(serial_port: str) -> bool:
    """Check if device responds to serial commands."""
    import serial
    try:
        ser = serial.Serial(serial_port, 115200, timeout=3)
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.write(b"json info\n")
        ser.flush()
        time.sleep(1)
        response = b""
        while ser.in_waiting:
            response += ser.read(ser.in_waiting)
            time.sleep(0.1)
        ser.close()
        text = response.decode('utf-8', errors='replace').strip()
        if 'version' in text:
            log.info("Device alive: %s", text)
            return True
        log.warning("Device responded but no version info: %s", text)
        return False
    except Exception as e:
        log.warning("Serial check failed: %s", e)
        return False


async def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)

    serial_port = sys.argv[1]
    ble_address = sys.argv[2]
    dfu_zip_path = sys.argv[3]

    from blinky_server.ota.ble_dfu import upload_ble_dfu, bootloader_address

    boot_addr = bootloader_address(ble_address)
    log.info("=" * 60)
    log.info("BLE DFU Test")
    log.info("  Serial port:     %s", serial_port)
    log.info("  BLE app address: %s", ble_address)
    log.info("  BLE boot addr:   %s", boot_addr)
    log.info("  DFU zip:         %s", dfu_zip_path)
    log.info("=" * 60)

    # Pre-flight: verify device is alive via serial
    log.info("")
    log.info("PRE-FLIGHT: Checking device is alive via serial...")
    alive = await verify_serial_alive(serial_port)
    if not alive:
        log.error("Device not responding on %s — aborting!", serial_port)
        sys.exit(1)
    log.info("Device is alive. Proceeding with BLE DFU.")

    # Release any server connection first
    log.info("")
    log.info("Releasing device from blinky-server...")
    try:
        import urllib.request
        import json
        # Try to find device by checking all devices
        req = urllib.request.Request("http://localhost:8420/api/devices")
        resp = urllib.request.urlopen(req, timeout=5)
        devices = json.loads(resp.read())
        for dev in devices:
            if dev['port'] == serial_port:
                release_url = f"http://localhost:8420/api/devices/{dev['id']}/release"
                data = json.dumps({"hold_seconds": 300}).encode()
                req = urllib.request.Request(release_url, data=data,
                                            headers={"Content-Type": "application/json"},
                                            method="POST")
                urllib.request.urlopen(req, timeout=5)
                log.info("Released device %s from server (300s hold)", dev['id'][:12])
                break
    except Exception as e:
        log.warning("Could not release from server (may not be running): %s", e)

    time.sleep(2)  # Let server disconnect

    # Define the serial bootloader entry callback
    async def enter_bootloader(cmd: str):
        await send_bootloader_via_serial(serial_port)

    # Progress callback with timestamps
    def on_progress(phase, msg, pct=None):
        pct_str = f" ({pct}%)" if pct is not None else ""
        log.info("[%s%s] %s", phase, pct_str, msg)

    # Run BLE DFU
    log.info("")
    log.info("Starting BLE DFU transfer...")
    t0 = time.monotonic()
    result = await upload_ble_dfu(
        app_ble_address=ble_address,
        dfu_zip_path=dfu_zip_path,
        enter_bootloader_via_serial=enter_bootloader,
        progress_callback=on_progress,
    )
    elapsed = time.monotonic() - t0

    log.info("")
    log.info("=" * 60)
    log.info("RESULT: %s", result.get("status", "unknown"))
    log.info("  Message:  %s", result.get("message", ""))
    log.info("  Elapsed:  %.1fs", elapsed)
    log.info("  Verified: %s", result.get("verified", "N/A"))
    log.info("=" * 60)

    if result.get("status") == "ok":
        # Post-flight: verify device is alive via serial after reboot
        log.info("")
        log.info("POST-FLIGHT: Waiting 10s for device to fully boot...")
        await asyncio.sleep(10)
        log.info("Checking device is alive via serial...")
        alive = await verify_serial_alive(serial_port)
        if alive:
            log.info("SUCCESS: Device is alive after BLE DFU!")
        else:
            log.warning("Device not responding on serial after BLE DFU.")
            log.warning("It may need more time to boot, or may be in safe mode.")
    else:
        log.error("BLE DFU FAILED: %s", result.get("message", ""))
        log.info("")
        log.info("Checking if device is still alive via serial...")
        await asyncio.sleep(5)
        alive = await verify_serial_alive(serial_port)
        if alive:
            log.info("Device is still alive on serial — no harm done.")
        else:
            log.warning("Device not responding on serial!")
            log.warning("Try power-cycling: sudo uhubctl -a cycle -p <port>")

    # Resume server reconnect
    try:
        import urllib.request
        import json
        for dev in devices:
            if dev['port'] == serial_port:
                req = urllib.request.Request(
                    f"http://localhost:8420/api/devices/{dev['id']}/reconnect",
                    method="POST")
                urllib.request.urlopen(req, timeout=5)
                log.info("Resumed server reconnect for %s", dev['id'][:12])
                break
    except Exception:
        pass

    sys.exit(0 if result.get("status") == "ok" else 1)


if __name__ == "__main__":
    asyncio.run(main())
