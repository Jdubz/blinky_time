#!/usr/bin/env python3
"""Enter BLE DFU bootloader via serial and immediately connect via BLE.

Must be fast — the bootloader may time out in 5-10 seconds.
"""
import asyncio
import logging
import struct
import sys
import serial
import time

from bleak import BleakClient, BleakScanner

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)-5s %(message)s",
                    datefmt="%H:%M:%S")
log = logging.getLogger()

DFU_CONTROL = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET = "00001532-1212-efde-1523-785feabcd123"
DFU_REVISION = "00001534-1212-efde-1523-785feabcd123"

got_notif = asyncio.Event()
notif_data = bytearray()


def on_notify(sender, data):
    global notif_data
    log.info("NOTIFICATION: %s (%d bytes)", data.hex(), len(data))
    notif_data = data
    got_notif.set()


def enter_bootloader_ble(serial_port):
    """Send 'bootloader ble' via serial to enter BLE DFU mode."""
    log.info("Sending 'bootloader ble' to %s...", serial_port)
    s = serial.Serial(serial_port, 115200, timeout=3)
    time.sleep(0.5)
    s.reset_input_buffer()
    s.write(b"bootloader ble\r\n")
    time.sleep(1)
    out = s.read(s.in_waiting).decode(errors='replace').strip()
    s.close()
    log.info("Serial response: %s", out)
    return "GPREGRET=0xB1" in out or "BLE DFU" in out


def clear_bluez(address):
    """Clear BlueZ state for address (without restarting bluetooth)."""
    import subprocess
    addr_u = address.replace(':', '_')
    subprocess.run(f"echo 'remove {address}' | bluetoothctl",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"sudo rm -rf /var/lib/bluetooth/*/cache/{addr_u}",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"sudo rm -rf /var/lib/bluetooth/*/{addr_u}",
                   shell=True, capture_output=True, timeout=5)


async def main():
    serial_port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM3"
    ble_addr = sys.argv[2] if len(sys.argv) > 2 else "F4:15:6D:FA:4D:93"

    # Step 1: Enter bootloader via serial FIRST (while serial still works)
    ok = enter_bootloader_ble(serial_port)
    if not ok:
        log.error("Failed to enter bootloader")
        return

    # Step 2: Clear BlueZ state AFTER bootloader entry.
    # Must restart bluetooth to purge in-memory GATT cache.
    # The bootloader has a ~120s timeout, so we have time.
    log.info("Clearing BlueZ state and restarting bluetooth...")
    clear_bluez(ble_addr)
    import subprocess
    subprocess.run(["sudo", "systemctl", "restart", "bluetooth"],
                   capture_output=True, timeout=10)
    await asyncio.sleep(3)

    # Step 3: Scan for the bootloader
    log.info("Scanning for bootloader...")
    t0 = time.monotonic()

    for attempt in range(10):
        dev = await BleakScanner.find_device_by_address(ble_addr, timeout=3.0)
        elapsed = time.monotonic() - t0
        if dev:
            log.info("Found after %.1fs: %s", elapsed, dev.name)
            break
        log.info("  Not found (%.1fs), retrying...", elapsed)
        if elapsed > 30:
            log.error("Gave up after 30s")
            return
    else:
        log.error("Device not found in scan")
        return

    # Step 4: Connect immediately
    log.info("Connecting...")
    client = BleakClient(dev, timeout=10.0)
    await client.connect()
    log.info("Connected! MTU=%d", client.mtu_size)

    # Step 5: Verify bootloader mode
    for svc in client.services:
        chars = [f"{c.uuid.split('-')[0]}[{','.join(c.properties)}]"
                 for c in svc.characteristics]
        log.info("  Svc %s: %s", svc.uuid.split('-')[0], ' '.join(chars))

    has_nus = any("6e400001" in str(s.uuid).lower() for s in client.services)
    try:
        rev = await client.read_gatt_char(DFU_REVISION)
        rv = int.from_bytes(rev[:2], "little")
        log.info("DFU Revision: 0x%04x (%s)", rv,
                 "BOOTLOADER" if rv > 1 else "APP!")
    except Exception:
        rv = 0

    if has_nus or rv <= 1:
        log.error("Device in app mode (NUS=%s, rev=0x%04x)", has_nus, rv)
        log.error("Bootloader may have timed out. Try with shorter delay.")
        await client.disconnect()
        return

    # Step 6: Test DFU notification
    log.info("In bootloader mode! Testing DFU...")
    log.info("Waiting 2s for sys_attr...")
    await asyncio.sleep(2)

    log.info("Subscribing to notifications...")
    await client.start_notify(DFU_CONTROL, on_notify)
    await asyncio.sleep(1)

    log.info("Sending START_DFU...")
    await client.write_gatt_char(DFU_CONTROL, bytes([0x01, 0x04]),
                                 response=True)
    log.info("Write OK")

    size = struct.pack('<III', 0, 0, 393548)
    log.info("Writing image size...")
    await client.write_gatt_char(DFU_PACKET, size, response=False)
    log.info("Write OK")

    log.info("Waiting 15s for notification...")
    try:
        await asyncio.wait_for(got_notif.wait(), timeout=15.0)
        log.info("=== GOT NOTIFICATION: %s ===", notif_data.hex())
        if len(notif_data) >= 3:
            log.info("  Opcode=0x%02x Procedure=0x%02x Result=0x%02x",
                     notif_data[0], notif_data[1], notif_data[2])
    except asyncio.TimeoutError:
        log.warning("TIMEOUT - no notification received")

    await client.disconnect()
    log.info("Done")


asyncio.run(main())
