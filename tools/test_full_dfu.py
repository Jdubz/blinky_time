#!/usr/bin/env python3
"""Full BLE DFU transfer test. Device must already be in bootloader BLE mode."""
import asyncio
import logging
import struct
import sys
import zipfile
import json
from bleak import BleakClient, BleakScanner

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)-5s %(message)s",
                    datefmt="%H:%M:%S")
log = logging.getLogger()

DFU_CONTROL = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET = "00001532-1212-efde-1523-785feabcd123"

got_notif = asyncio.Event()
notif_data = bytearray()


def on_notify(sender, data):
    global notif_data
    log.info("NOTIFICATION: %s (%d bytes)", data.hex(), len(data))
    notif_data = data
    got_notif.set()


async def wait_response(opcode_name, timeout=30.0):
    global notif_data
    got_notif.clear()
    notif_data = bytearray()
    try:
        await asyncio.wait_for(got_notif.wait(), timeout=timeout)
        if len(notif_data) >= 3:
            op, proc, res = notif_data[0], notif_data[1], notif_data[2]
            result_names = {1: "SUCCESS", 2: "INVALID_STATE", 3: "NOT_SUPPORTED",
                           4: "DATA_SIZE", 5: "CRC_ERROR", 6: "FAILED"}
            log.info("  Response: op=0x%02x proc=0x%02x result=%s",
                     op, proc, result_names.get(res, f"0x{res:02x}"))
            return res == 1  # SUCCESS
        return False
    except asyncio.TimeoutError:
        log.warning("  TIMEOUT waiting for %s response", opcode_name)
        return False


async def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "F4:15:6D:FA:4D:94"
    dfu_zip = sys.argv[2] if len(sys.argv) > 2 else "/tmp/blinky-dfu.zip"

    # Parse DFU package
    with zipfile.ZipFile(dfu_zip, 'r') as zf:
        manifest = json.loads(zf.read('manifest.json'))
        app = manifest['manifest']['application']
        init_packet = zf.read(app['dat_file'])
        firmware = zf.read(app['bin_file'])
    log.info("Firmware: %d bytes, init: %d bytes", len(firmware), len(init_packet))

    # Scan and connect
    dev = await BleakScanner.find_device_by_address(addr, timeout=5.0)
    if not dev:
        log.error("Device not found")
        return
    log.info("Found: %s", dev.name)

    client = BleakClient(dev, timeout=15.0)
    await client.connect()
    log.info("Connected MTU=%d", client.mtu_size)
    mtu = min(client.mtu_size - 3, 20)

    await asyncio.sleep(2)
    await client.start_notify(DFU_CONTROL, on_notify)
    await asyncio.sleep(1)

    # 1. START_DFU
    log.info("=== START_DFU ===")
    await client.write_gatt_char(DFU_CONTROL,
                                 bytes([0x01, 0x04]), response=False)
    size_data = struct.pack('<III', 0, 0, len(firmware))
    await client.write_gatt_char(DFU_PACKET, size_data, response=False)
    if not await wait_response("START_DFU"):
        log.error("START_DFU failed")
        await client.disconnect()
        return

    # 2. INIT_DFU
    log.info("=== INIT_DFU ===")
    await client.write_gatt_char(DFU_CONTROL,
                                 bytes([0x02, 0x00]), response=False)
    await client.write_gatt_char(DFU_PACKET, init_packet, response=False)
    await client.write_gatt_char(DFU_CONTROL,
                                 bytes([0x02, 0x01]), response=False)
    if not await wait_response("INIT_DFU"):
        log.error("INIT_DFU failed")
        await client.disconnect()
        return

    # 3. Set PRN (8 packets)
    log.info("=== Set PRN=8 ===")
    prn_cmd = bytes([0x08]) + struct.pack('<H', 8)
    await client.write_gatt_char(DFU_CONTROL, prn_cmd, response=False)

    # 4. RECEIVE_FIRMWARE
    log.info("=== RECEIVE_FIRMWARE (%d bytes, MTU=%d) ===", len(firmware), mtu)
    await client.write_gatt_char(DFU_CONTROL,
                                 bytes([0x03]), response=False)

    sent = 0
    pkt_count = 0
    while sent < len(firmware):
        chunk = firmware[sent:sent + mtu]
        await client.write_gatt_char(DFU_PACKET, chunk, response=False)
        sent += len(chunk)
        pkt_count += 1

        # PRN every 8 packets
        if pkt_count % 8 == 0:
            got_notif.clear()
            try:
                await asyncio.wait_for(got_notif.wait(), timeout=10.0)
                if notif_data[0] == 0x11:  # PKT_RCPT_NOTIF
                    rcvd = struct.unpack('<I', notif_data[1:5])[0]
                    pct = (rcvd * 100) // len(firmware)
                    if pct % 10 < 3:
                        log.info("  Progress: %d%% (%d/%d)", pct, rcvd, len(firmware))
            except asyncio.TimeoutError:
                log.warning("  PRN timeout at %d bytes", sent)

    log.info("Firmware sent, waiting for completion...")
    if not await wait_response("RECEIVE_FIRMWARE", timeout=60.0):
        log.error("RECEIVE_FIRMWARE failed")
        await client.disconnect()
        return

    # 5. VALIDATE
    log.info("=== VALIDATE ===")
    await client.write_gatt_char(DFU_CONTROL,
                                 bytes([0x04]), response=False)
    if not await wait_response("VALIDATE"):
        log.error("VALIDATE failed")
        await client.disconnect()
        return

    # 6. ACTIVATE_AND_RESET
    log.info("=== ACTIVATE_AND_RESET ===")
    try:
        await client.write_gatt_char(DFU_CONTROL,
                                     bytes([0x05]), response=False)
    except Exception as e:
        log.debug("Activate (may disconnect): %s", e)

    log.info("DFU COMPLETE! Device rebooting.")
    await asyncio.sleep(2)
    try:
        await client.disconnect()
    except Exception:
        pass


asyncio.run(main())
