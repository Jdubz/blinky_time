#!/usr/bin/env python3
"""Test BLE DFU notification subscription on a device already in bootloader mode.

Usage:
    # First: send device to bootloader manually via serial
    # Then run this to test DFU communication
    python3 test_ble_dfu_notify.py <bootloader_ble_address>

Example:
    python3 test_ble_dfu_notify.py E3:8D:10:5F:17:67
"""
import asyncio
import logging
import struct
import subprocess
import sys
import time

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-5s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger(__name__)

from bleak import BleakClient, BleakScanner

DFU_CONTROL = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET = "00001532-1212-efde-1523-785feabcd123"
DFU_REVISION = "00001534-1212-efde-1523-785feabcd123"
CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"


async def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)

    boot_addr = sys.argv[1]

    # Clear BlueZ state
    log.info("Clearing BlueZ state for %s...", boot_addr)
    addr_u = boot_addr.replace(":", "_")
    subprocess.run(["bluetoothctl", "remove", boot_addr], capture_output=True, timeout=5)
    import glob
    for d in glob.glob(f"/var/lib/bluetooth/*/cache/{addr_u}"):
        subprocess.run(["rm", "-rf", d], capture_output=True)
    for d in glob.glob(f"/var/lib/bluetooth/*/{addr_u}"):
        subprocess.run(["rm", "-rf", d], capture_output=True)
    await asyncio.sleep(3)

    # Scan
    log.info("Scanning for bootloader at %s...", boot_addr)
    dev = await BleakScanner.find_device_by_address(boot_addr, timeout=15.0)
    if not dev:
        log.error("Bootloader not found!")
        sys.exit(1)
    log.info("Found: %s (%s) RSSI=%s", dev.name, dev.address, getattr(dev, 'rssi', '?'))

    # Connect
    client = BleakClient(dev, timeout=15.0)
    await client.connect()
    mtu = max(client.mtu_size - 3, 20)
    log.info("Connected. MTU=%d, payload=%d", client.mtu_size, mtu)

    # Dump services
    log.info("Services:")
    ctrl_cccd_handle = None
    for svc in client.services:
        log.info("  Service: %s", svc.uuid)
        for char in svc.characteristics:
            log.info("    Char: %s props=%s handle=%d", char.uuid, char.properties, char.handle)
            for desc in char.descriptors:
                log.info("      Desc: %s handle=%d", desc.uuid, desc.handle)
                if char.uuid == DFU_CONTROL and desc.uuid == CCCD_UUID:
                    ctrl_cccd_handle = desc.handle

    # Read revision
    try:
        rev = await client.read_gatt_char(DFU_REVISION)
        log.info("DFU Revision: %s (val=%d)", rev.hex(), int.from_bytes(rev[:2], 'little'))
    except Exception as e:
        log.warning("Rev read: %s", e)

    # Method 1: Try bleak start_notify with use_start_notify
    notifications = []

    def on_notify(sender, data):
        log.info("NOTIFY: %s (%d bytes)", data.hex(), len(data))
        notifications.append(bytes(data))

    log.info("")
    log.info("=== Method 1: bleak start_notify (use_start_notify=True) ===")
    await client.start_notify(DFU_CONTROL, on_notify, bluez={"use_start_notify": True})
    await asyncio.sleep(2)

    # Read CCCD to check if notifications are enabled
    if ctrl_cccd_handle:
        try:
            cccd_val = await client.read_gatt_descriptor(ctrl_cccd_handle)
            log.info("CCCD value after start_notify: %s", cccd_val.hex())
        except Exception as e:
            log.warning("CCCD read failed: %s", e)

    log.info("Sending START_DFU (response=True)...")
    await client.write_gatt_char(DFU_CONTROL, bytes([0x01, 0x04]), response=True)
    size_pkt = struct.pack('<III', 0, 0, 510144)
    await client.write_gatt_char(DFU_PACKET, size_pkt, response=False)
    await asyncio.sleep(3)
    log.info("Method 1 notifications: %d", len(notifications))

    if not notifications and ctrl_cccd_handle:
        # Method 2: Manual CCCD write
        log.info("")
        log.info("=== Method 2: Manual CCCD write ===")
        await client.stop_notify(DFU_CONTROL)
        await asyncio.sleep(1)

        # Disconnect and reconnect to reset DFU state
        await client.disconnect()
        log.info("Disconnected. Reconnecting...")
        await asyncio.sleep(2)

        # Clear cache again
        subprocess.run(["bluetoothctl", "remove", boot_addr], capture_output=True, timeout=5)
        await asyncio.sleep(2)

        dev = await BleakScanner.find_device_by_address(boot_addr, timeout=15.0)
        if not dev:
            log.error("Bootloader not found on reconnect!")
            sys.exit(1)

        client = BleakClient(dev, timeout=15.0)
        await client.connect()
        log.info("Reconnected. MTU=%d", client.mtu_size)
        await asyncio.sleep(2)

        # Find CCCD handle again
        for svc in client.services:
            for char in svc.characteristics:
                for desc in char.descriptors:
                    if char.uuid == DFU_CONTROL and desc.uuid == CCCD_UUID:
                        ctrl_cccd_handle = desc.handle

        notifications.clear()

        # Subscribe via start_notify first (to register callback)
        await client.start_notify(DFU_CONTROL, on_notify)
        await asyncio.sleep(1)

        # Then manually write CCCD to ensure notifications are enabled
        if ctrl_cccd_handle:
            log.info("Writing CCCD handle %d = 0x0001 (enable notifications)", ctrl_cccd_handle)
            await client.write_gatt_descriptor(ctrl_cccd_handle, b'\x01\x00')
            await asyncio.sleep(1)

            cccd_val = await client.read_gatt_descriptor(ctrl_cccd_handle)
            log.info("CCCD value after manual write: %s", cccd_val.hex())

        log.info("Sending START_DFU (response=True)...")
        await client.write_gatt_char(DFU_CONTROL, bytes([0x01, 0x04]), response=True)
        size_pkt = struct.pack('<III', 0, 0, 510144)
        await client.write_gatt_char(DFU_PACKET, size_pkt, response=False)
        await asyncio.sleep(3)
        log.info("Method 2 notifications: %d", len(notifications))

    if notifications:
        log.info("SUCCESS! Received %d notifications:", len(notifications))
        for n in notifications:
            log.info("  %s", n.hex())
    else:
        log.error("FAILED: No notifications received with either method")

    await client.disconnect()
    log.info("Done.")


if __name__ == "__main__":
    asyncio.run(main())
