#!/usr/bin/env python3
"""Test BLE DFU notification reception with various timing strategies.

The bootloader's service_change_indicate() calls sd_ble_gatts_service_changed()
which triggers a Service Changed indication. If BlueZ processes this after our
CCCD subscription, the subscription may be invalidated. This script tests
different timing approaches to find one that reliably receives notifications.

Usage: python3 test_ble_dfu_notify.py --address F4:15:6D:FA:4D:93
       (device must already be in bootloader mode)
"""

import argparse
import asyncio
import logging
import struct
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("ERROR: pip install bleak")
    sys.exit(1)

log = logging.getLogger(__name__)

DFU_SERVICE = "00001530-1212-efde-1523-785feabcd123"
DFU_CONTROL = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET = "00001532-1212-efde-1523-785feabcd123"
DFU_REVISION = "00001534-1212-efde-1523-785feabcd123"

notification_received = asyncio.Event()
notification_data = bytearray()


def on_notify(sender, data):
    global notification_data
    log.info("NOTIFICATION: %s (%d bytes)", data.hex(), len(data))
    notification_data = data
    notification_received.set()


def clear_bluez(address):
    """Thoroughly clear BlueZ state for this device."""
    import subprocess
    addr_u = address.replace(':', '_')
    subprocess.run(f"echo 'remove {address}' | bluetoothctl",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"sudo rm -rf /var/lib/bluetooth/*/cache/{addr_u}",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"sudo rm -rf /var/lib/bluetooth/*/{addr_u}",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(["sudo", "systemctl", "restart", "bluetooth"],
                   capture_output=True, timeout=10)


async def test_strategy(address, strategy_name, pre_notify_delay, post_notify_delay,
                        use_pair=False, write_cccd_manually=False):
    """Test a specific timing strategy for receiving DFU notifications."""
    global notification_data
    notification_received.clear()
    notification_data = bytearray()

    log.info("=== Strategy: %s ===", strategy_name)
    log.info("  pre_notify_delay=%ss, post_notify_delay=%ss, pair=%s, manual_cccd=%s",
             pre_notify_delay, post_notify_delay, use_pair, write_cccd_manually)

    # Clear BlueZ state
    clear_bluez(address)
    await asyncio.sleep(3)

    try:
        client = BleakClient(address, timeout=15.0)
        await client.connect()
        mtu = client.mtu_size
        log.info("  Connected, MTU=%d", mtu)

        # Verify we're in bootloader mode
        try:
            rev_data = await client.read_gatt_char(DFU_REVISION)
            rev = int.from_bytes(rev_data[:2], 'little')
            log.info("  DFU Revision: 0x%04x", rev)
            if rev == 0x0001:
                log.error("  Still in app mode!")
                await client.disconnect()
                return False
        except Exception as e:
            log.warning("  Could not read revision: %s", e)

        # Optional: trigger pairing to force sys_attr processing
        if use_pair:
            log.info("  Triggering pair...")
            try:
                await client.pair()
                await asyncio.sleep(1)
            except Exception as e:
                log.debug("  Pair result: %s", e)

        # Pre-notification delay (let SoftDevice process sys_attr)
        log.info("  Waiting %ss for sys_attr processing...", pre_notify_delay)
        await asyncio.sleep(pre_notify_delay)

        # Enable notifications
        if write_cccd_manually:
            # Write CCCD descriptor directly
            ctrl_char = client.services.get_characteristic(DFU_CONTROL)
            if ctrl_char:
                for desc in ctrl_char.descriptors:
                    if "2902" in str(desc.uuid):
                        log.info("  Writing CCCD manually at handle %d", desc.handle)
                        await client.write_gatt_descriptor(desc.handle, b'\x01\x00')
                        break
            # Also use start_notify for the callback
            await client.start_notify(DFU_CONTROL, on_notify)
        else:
            log.info("  Subscribing to notifications via start_notify...")
            await client.start_notify(DFU_CONTROL, on_notify)

        # Post-notification delay
        log.info("  Waiting %ss for notification subscription to propagate...",
                 post_notify_delay)
        await asyncio.sleep(post_notify_delay)

        # Send START_DFU (0x01, 0x04=application)
        log.info("  Sending START_DFU...")
        await client.write_gatt_char(DFU_CONTROL, bytes([0x01, 0x04]),
                                     response=True)
        log.info("  START_DFU write succeeded")

        # Write image size to DFU Packet
        size_data = struct.pack('<III', 0, 0, 393548)
        log.info("  Writing image size: %s", size_data.hex())
        await client.write_gatt_char(DFU_PACKET, size_data, response=False)
        log.info("  Image size written")

        # Wait for notification
        log.info("  Waiting for notification (10s timeout)...")
        try:
            await asyncio.wait_for(notification_received.wait(), timeout=10.0)
            log.info("  GOT NOTIFICATION: %s", notification_data.hex())
            if len(notification_data) >= 3:
                log.info("    Opcode: 0x%02x (expect 0x10=RESPONSE)",
                         notification_data[0])
                log.info("    Procedure: 0x%02x (expect 0x01=START_DFU)",
                         notification_data[1])
                log.info("    Result: 0x%02x (0x01=SUCCESS)",
                         notification_data[2])
            await client.disconnect()
            return True
        except asyncio.TimeoutError:
            log.warning("  TIMEOUT - no notification received")
            await client.disconnect()
            return False

    except Exception as e:
        log.error("  Failed: %s", e)
        return False


async def main():
    parser = argparse.ArgumentParser(description="Test BLE DFU notification timing")
    parser.add_argument("--address", "-a", required=True, help="BLE address")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-5s %(message)s",
        datefmt="%H:%M:%S",
    )

    strategies = [
        # (name, pre_delay, post_delay, pair, manual_cccd)
        ("baseline_3s_1s", 3, 1, False, False),
        ("long_wait_5s_2s", 5, 2, False, False),
        ("manual_cccd_3s_2s", 3, 2, False, True),
        ("with_pair_3s_1s", 3, 1, True, False),
        ("very_long_8s_3s", 8, 3, False, False),
    ]

    for name, pre, post, pair, manual in strategies:
        success = await test_strategy(args.address, name, pre, post, pair, manual)
        if success:
            print(f"\n*** SUCCESS with strategy: {name} ***")
            break
        print(f"  Strategy {name}: FAILED")
        # Brief pause between attempts
        await asyncio.sleep(2)
    else:
        print("\n*** ALL STRATEGIES FAILED ***")
        print("The bootloader may need firmware changes to fix notification delivery.")


if __name__ == "__main__":
    asyncio.run(main())
