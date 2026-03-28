#!/usr/bin/env python3
"""Quick BLE DFU notification test.

Assumes: BlueZ cache already cleared, device already in bootloader mode.
Usage: python3 test_dfu_quick.py --address F4:15:6D:FA:4D:93
"""
import asyncio
import logging
import struct
import sys
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


async def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "F4:15:6D:FA:4D:93"

    # Scan first so BlueZ discovers the device (needed after cache clear)
    log.info("Scanning for %s...", addr)
    dev = await BleakScanner.find_device_by_address(addr, timeout=10.0)
    if dev:
        log.info("Found: %s", dev.name)
    else:
        log.warning("Not found in scan, trying direct connect anyway...")

    log.info("Connecting to %s...", addr)
    client = BleakClient(dev or addr, timeout=15.0)
    await client.connect()
    log.info("Connected! MTU=%d", client.mtu_size)

    # List services
    for svc in client.services:
        chars = [f"{c.uuid.split('-')[0]}[{','.join(c.properties)}]"
                 for c in svc.characteristics]
        log.info("  Svc %s: %s", svc.uuid.split('-')[0], ' '.join(chars))

    # Check revision
    try:
        rev = await client.read_gatt_char(DFU_REVISION)
        rev_val = int.from_bytes(rev[:2], "little")
        mode = "BOOTLOADER" if rev_val > 1 else "APP"
        log.info("DFU Revision: 0x%04x = %s", rev_val, mode)
        if rev_val <= 1:
            log.error("Device in app mode! Enter bootloader first.")
            await client.disconnect()
            return
    except Exception as e:
        log.warning("Could not read revision: %s", e)

    # Wait for sys_attr to settle
    log.info("Waiting 3s for SoftDevice sys_attr processing...")
    await asyncio.sleep(3)

    # Subscribe to notifications
    log.info("Subscribing to DFU Control notifications...")
    await client.start_notify(DFU_CONTROL, on_notify)
    log.info("Waiting 2s for CCCD subscription to propagate...")
    await asyncio.sleep(2)

    # Send START_DFU with APPLICATION type
    log.info("Sending START_DFU (0x01 0x04)...")
    await client.write_gatt_char(DFU_CONTROL, bytes([0x01, 0x04]),
                                 response=True)
    log.info("START_DFU write OK")

    # Write image size to DFU Packet
    firmware_size = 393548
    size_data = struct.pack('<III', 0, 0, firmware_size)
    log.info("Writing image size (%d bytes) to DFU Packet...", firmware_size)
    await client.write_gatt_char(DFU_PACKET, size_data, response=False)
    log.info("Image size write OK")

    # Wait for notification response
    log.info("Waiting 15s for notification...")
    try:
        await asyncio.wait_for(got_notif.wait(), timeout=15.0)
        log.info("=== GOT NOTIFICATION: %s ===", notif_data.hex())
        if len(notif_data) >= 3:
            log.info("  Opcode: 0x%02x (0x10=RESPONSE)", notif_data[0])
            log.info("  Procedure: 0x%02x (0x01=START_DFU)", notif_data[1])
            log.info("  Result: 0x%02x (0x01=SUCCESS)", notif_data[2])
    except asyncio.TimeoutError:
        log.warning("=== TIMEOUT: No notification received ===")
        log.info("Trying write-without-response approach...")
        # Some bootloader versions need write-without-response for DFU Control
        got_notif.clear()
        await client.write_gatt_char(DFU_CONTROL, bytes([0x01, 0x04]),
                                     response=False)
        log.info("Write-without-response sent, waiting 10s...")
        try:
            await asyncio.wait_for(got_notif.wait(), timeout=10.0)
            log.info("=== GOT NOTIFICATION (write-no-resp): %s ===",
                     notif_data.hex())
        except asyncio.TimeoutError:
            log.warning("=== Still no notification ===")

    await client.disconnect()
    log.info("Done")


asyncio.run(main())
