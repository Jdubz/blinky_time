"""BLE DFU firmware upload for nRF52840 devices.

Implements the Nordic Legacy DFU protocol (SDK v11) over BLE using bleak.
Called by blinky-server when the device is connected via serial (for
bootloader entry) or BLE (for direct DFU).

Key protocol details (all discovered via testing, Mar 2026):
- Adafruit bootloader v0.6.1 uses Legacy DFU, NOT Secure DFU v2
- DFU Control writes MUST use write-without-response
- Bootloader BLE address = app_address + 1 (last octet)
- Must force StartNotify (not AcquireNotify) for reliable notifications
- BlueZ GATT cache must be cleared between app and bootloader connections
- GPREGRET=0xA8 for serial-triggered BLE DFU entry
"""
from __future__ import annotations

import asyncio
import logging
import struct
import subprocess
import time
import zipfile
import json
from pathlib import Path

from bleak import BleakClient, BleakScanner

log = logging.getLogger(__name__)

# Nordic DFU Service UUIDs
DFU_CONTROL_UUID = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET_UUID = "00001532-1212-efde-1523-785feabcd123"
DFU_REVISION_UUID = "00001534-1212-efde-1523-785feabcd123"

PRN_INTERVAL = 8  # Packet Receipt Notification interval (max 8 for Adafruit bootloader)


def bootloader_address(app_address: str) -> str:
    """Bootloader advertises at app_address + 1 (last octet)."""
    parts = app_address.split(':')
    last = int(parts[-1], 16)
    parts[-1] = f"{(last + 1) & 0xFF:02X}"
    return ':'.join(parts)


def clear_bluez_state(address: str):
    """Clear BlueZ GATT cache for an address. Required between app/bootloader."""
    addr_u = address.replace(':', '_')
    subprocess.run(f"echo 'remove {address}' | bluetoothctl",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"sudo rm -rf /var/lib/bluetooth/*/cache/{addr_u}",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"rm -rf /var/lib/bluetooth/*/cache/{addr_u}",
                   shell=True, capture_output=True, timeout=5)
    subprocess.run(f"sudo rm -rf /var/lib/bluetooth/*/{addr_u}",
                   shell=True, capture_output=True, timeout=5)


def parse_dfu_zip(zip_path: str) -> tuple[bytes, bytes]:
    """Extract init packet and firmware binary from DFU zip."""
    with zipfile.ZipFile(zip_path) as zf:
        manifest = json.loads(zf.read('manifest.json'))
        app = manifest['manifest']['application']
        return zf.read(app['dat_file']), zf.read(app['bin_file'])


async def upload_ble_dfu(
    app_ble_address: str,
    dfu_zip_path: str,
    enter_bootloader_via_serial: callable | None = None,
    progress_callback: callable | None = None,
) -> dict:
    """Upload firmware via BLE DFU.

    Args:
        app_ble_address: Device's BLE address in app mode
        dfu_zip_path: Path to DFU .zip from adafruit-nrfutil genpkg
        enter_bootloader_via_serial: Optional async callable that sends
            'bootloader ble' via serial. If None, assumes device already
            in bootloader or uses BLE DFU trigger.
        progress_callback: Optional callable(phase, msg, pct)

    Returns:
        dict with status, message, elapsed_s
    """
    t0 = time.monotonic()
    result = {"status": "error", "message": "", "elapsed_s": 0}
    boot_addr = bootloader_address(app_ble_address)

    def progress(phase, msg, pct=None):
        log.info("[BLE-DFU %s] %s", phase, msg)
        if progress_callback:
            progress_callback(phase, msg, pct)

    # Parse firmware
    try:
        init_packet, firmware = parse_dfu_zip(dfu_zip_path)
    except Exception as e:
        result["message"] = f"Failed to parse DFU zip: {e}"
        return result
    progress("init", f"Firmware: {len(firmware)} bytes, init: {len(init_packet)} bytes", 5)

    # Enter bootloader
    if enter_bootloader_via_serial:
        progress("bootloader", "Entering BLE DFU via serial command...", 10)
        try:
            await enter_bootloader_via_serial("bootloader ble")
        except Exception as e:
            log.debug("Bootloader command result (may disconnect): %s", e)
        await asyncio.sleep(2)

    # Clear BlueZ state for both addresses
    progress("cache", "Clearing BlueZ GATT cache...", 15)
    await asyncio.to_thread(clear_bluez_state, app_ble_address)
    await asyncio.to_thread(clear_bluez_state, boot_addr)
    await asyncio.sleep(3)

    # Scan for bootloader
    progress("scan", f"Scanning for bootloader at {boot_addr}...", 20)
    dev = await BleakScanner.find_device_by_address(boot_addr, timeout=15.0)
    if not dev:
        result["message"] = f"Bootloader not found at {boot_addr}"
        return result
    progress("scan", f"Found: {dev.name}", 25)

    # Connect
    client = BleakClient(dev, timeout=15.0)
    try:
        await client.connect()
    except Exception as e:
        result["message"] = f"Failed to connect to bootloader: {e}"
        return result
    mtu = min(client.mtu_size - 3, 20)
    progress("connect", f"Connected, MTU={client.mtu_size}", 30)

    # Verify bootloader mode
    try:
        rev = await client.read_gatt_char(DFU_REVISION_UUID)
        rev_val = int.from_bytes(rev[:2], 'little')
        if rev_val <= 1:
            await client.disconnect()
            result["message"] = f"Device in app mode (revision=0x{rev_val:04x}), not bootloader"
            return result
    except Exception:
        pass  # Some bootloader versions may not expose revision

    # Wait for sys_attr, subscribe to notifications
    await asyncio.sleep(2)
    response_event = asyncio.Event()
    response_data = bytearray()

    def on_notify(sender, data):
        nonlocal response_data
        response_data = data
        response_event.set()

    await client.start_notify(DFU_CONTROL_UUID, on_notify,
                              bluez={"use_start_notify": True})
    await asyncio.sleep(1)

    async def wait_response(name, timeout=30.0):
        nonlocal response_data
        response_event.clear()
        response_data = bytearray()
        try:
            await asyncio.wait_for(response_event.wait(), timeout=timeout)
            if len(response_data) >= 3 and response_data[2] != 0x01:
                msg = f"{name} failed: result=0x{response_data[2]:02x}"
                log.error(msg)
                return False, msg
            return True, ""
        except asyncio.TimeoutError:
            return False, f"{name} timeout ({timeout}s)"

    try:
        # START_DFU
        progress("dfu", "START_DFU...", 35)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x01, 0x04]), response=False)
        await client.write_gatt_char(DFU_PACKET_UUID,
                                     struct.pack('<III', 0, 0, len(firmware)),
                                     response=False)
        ok, msg = await wait_response("START_DFU")
        if not ok:
            result["message"] = msg
            return result

        # INIT_DFU
        progress("dfu", "INIT_DFU...", 40)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x02, 0x00]), response=False)
        await client.write_gatt_char(DFU_PACKET_UUID,
                                     init_packet, response=False)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x02, 0x01]), response=False)
        ok, msg = await wait_response("INIT_DFU")
        if not ok:
            result["message"] = msg
            return result

        # Set PRN
        prn_cmd = bytes([0x08]) + struct.pack('<H', PRN_INTERVAL)
        await client.write_gatt_char(DFU_CONTROL_UUID, prn_cmd, response=False)

        # RECEIVE_FIRMWARE
        progress("transfer", f"Sending {len(firmware)} bytes...", 45)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x03]), response=False)

        sent = 0
        pkt_count = 0
        last_pct = 0
        while sent < len(firmware):
            chunk = firmware[sent:sent + mtu]
            await client.write_gatt_char(DFU_PACKET_UUID, chunk, response=False)
            sent += len(chunk)
            pkt_count += 1

            if PRN_INTERVAL > 0 and pkt_count % PRN_INTERVAL == 0:
                response_event.clear()
                try:
                    await asyncio.wait_for(response_event.wait(), timeout=10.0)
                except asyncio.TimeoutError:
                    log.warning("PRN timeout at %d/%d bytes", sent, len(firmware))

            pct = 45 + (sent * 50) // len(firmware)
            if pct >= last_pct + 5:
                progress("transfer", f"{(sent*100)//len(firmware)}%", pct)
                last_pct = pct

        progress("transfer", "Waiting for completion...", 95)
        ok, msg = await wait_response("RECEIVE_FIRMWARE", timeout=60.0)
        if not ok:
            result["message"] = msg
            return result

        # VALIDATE
        progress("validate", "Validating firmware...", 97)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x04]), response=False)
        ok, msg = await wait_response("VALIDATE")
        if not ok:
            result["message"] = msg
            return result

        # ACTIVATE_AND_RESET
        progress("activate", "Activating and resetting...", 99)
        try:
            await client.write_gatt_char(DFU_CONTROL_UUID,
                                         bytes([0x05]), response=False)
        except Exception:
            pass  # Device disconnects immediately

        progress("done", "BLE DFU complete!", 100)
        elapsed = time.monotonic() - t0
        result.update(status="ok", message="BLE DFU upload successful",
                      elapsed_s=round(elapsed, 1))

    except Exception as e:
        result["message"] = f"DFU transfer error: {e}"
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    result["elapsed_s"] = round(time.monotonic() - t0, 1)
    return result
