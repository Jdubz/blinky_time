"""BLE DFU firmware upload for nRF52840 devices.

Implements the Nordic Legacy DFU protocol (SDK v11) over BLE using bleak.
Called by blinky-server when the device is connected via serial (for
bootloader entry) or BLE (for direct DFU).

Key protocol details (all discovered via testing, Mar 2026):
- Adafruit bootloader v0.6.1 uses Legacy DFU, NOT Secure DFU v2
- DFU Control writes MUST use write-with-response (characteristic only advertises 'write', not 'write-without-response')
- DFU Packet writes use write-without-response for speed
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

PRN_INTERVAL = 8  # Packet Receipt Notification interval (0-65535, max 8 recommended for Adafruit bootloader)
assert 0 <= PRN_INTERVAL <= 65535, f"PRN_INTERVAL must be 0-65535, got {PRN_INTERVAL}"


def bootloader_address(app_address: str) -> str:
    """Bootloader advertises at app_address + 1 (last octet)."""
    parts = app_address.split(':')
    last = int(parts[-1], 16)
    parts[-1] = f"{(last + 1) & 0xFF:02X}"
    return ':'.join(parts)


def _validate_ble_address(address: str) -> None:
    """Validate BLE address format (XX:XX:XX:XX:XX:XX)."""
    import re
    if not re.match(r'^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$', address):
        raise ValueError(f"Invalid BLE address format: {address!r}")


def clear_bluez_state(address: str):
    """Clear BlueZ GATT cache for an address. Required between app/bootloader."""
    _validate_ble_address(address)
    addr_u = address.replace(':', '_')
    subprocess.run(["bluetoothctl", "remove", address],
                   capture_output=True, timeout=5)
    # Clean cache directories — use glob to expand wildcards safely
    import glob as _glob
    for cache_dir in _glob.glob(f"/var/lib/bluetooth/*/cache/{addr_u}"):
        subprocess.run(["sudo", "rm", "-rf", cache_dir],
                       capture_output=True, timeout=5)
        subprocess.run(["rm", "-rf", cache_dir],
                       capture_output=True, timeout=5)
    for dev_dir in _glob.glob(f"/var/lib/bluetooth/*/{addr_u}"):
        subprocess.run(["sudo", "rm", "-rf", dev_dir],
                       capture_output=True, timeout=5)


def parse_dfu_zip(zip_path: str) -> tuple[bytes, bytes, int]:
    """Extract init packet, firmware binary, and image type from DFU zip.

    Returns (init_packet, firmware, image_type) where image_type is:
        0x04 = application, 0x02 = bootloader, 0x01 = softdevice
    """
    IMAGE_TYPES = {"application": 0x04, "bootloader": 0x02, "softdevice": 0x01}
    with zipfile.ZipFile(zip_path) as zf:
        manifest = json.loads(zf.read('manifest.json'))['manifest']
        for key, img_type in IMAGE_TYPES.items():
            if key in manifest:
                entry = manifest[key]
                return zf.read(entry['dat_file']), zf.read(entry['bin_file']), img_type
        raise ValueError(f"No recognized image type in manifest: {list(manifest.keys())}")


async def upload_ble_dfu(
    app_ble_address: str,
    dfu_zip_path: str,
    enter_bootloader_via_serial: callable | None = None,
    enter_bootloader_via_ble: callable | None = None,
    progress_callback: callable | None = None,
) -> dict:
    """Upload firmware via BLE DFU.

    Args:
        app_ble_address: Device's BLE address in app mode
        dfu_zip_path: Path to DFU .zip from adafruit-nrfutil genpkg
        enter_bootloader_via_serial: Optional async callable that sends
            'bootloader ble' via serial transport.
        enter_bootloader_via_ble: Optional async callable that sends
            'bootloader ble' via BLE NUS transport (for wireless-only devices).
            The transport will disconnect when the device resets.
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
        init_packet, firmware, image_type = parse_dfu_zip(dfu_zip_path)
    except Exception as e:
        result["message"] = f"Failed to parse DFU zip: {e}"
        return result
    type_name = {0x04: "application", 0x02: "bootloader", 0x01: "softdevice"}.get(image_type, "unknown")
    progress("init", f"{type_name}: {len(firmware)} bytes, init: {len(init_packet)} bytes", 5)

    # Enter bootloader (serial or BLE — device resets immediately)
    if enter_bootloader_via_serial:
        progress("bootloader", "Entering BLE DFU via serial command...", 10)
        try:
            await enter_bootloader_via_serial("bootloader ble")
        except Exception as e:
            log.debug("Bootloader command result (may disconnect): %s", e)
        await asyncio.sleep(2)
    elif enter_bootloader_via_ble:
        progress("bootloader", "Entering BLE DFU via BLE NUS command...", 10)
        try:
            await enter_bootloader_via_ble("bootloader ble")
        except Exception as e:
            # Device resets immediately — BLE disconnect is expected
            log.debug("BLE bootloader command result (expected disconnect): %s", e)
        await asyncio.sleep(3)  # Extra time: BLE disconnect + device reset + bootloader init

    # Clear BlueZ state for both addresses
    progress("cache", "Clearing BlueZ GATT cache...", 15)
    await asyncio.to_thread(clear_bluez_state, app_ble_address)
    await asyncio.to_thread(clear_bluez_state, boot_addr)
    await asyncio.sleep(3)

    # Scan for bootloader (retry with cache-clear — bootloader advertises ~30s)
    dev = None
    for scan_attempt in range(3):
        progress("scan", f"Scanning for bootloader at {boot_addr} (attempt {scan_attempt + 1}/3)...", 20)
        dev = await BleakScanner.find_device_by_address(boot_addr, timeout=15.0)
        if dev:
            break
        log.warning("Bootloader not found on attempt %d, clearing cache and retrying", scan_attempt + 1)
        await asyncio.to_thread(clear_bluez_state, boot_addr)
        await asyncio.sleep(2)
    if not dev:
        result["message"] = f"Bootloader not found at {boot_addr} after 3 scan attempts"
        return result
    progress("scan", f"Found: {dev.name}", 25)

    # Connect
    client = BleakClient(dev, timeout=15.0)
    try:
        await client.connect()
    except Exception as e:
        result["message"] = f"Failed to connect to bootloader: {e}"
        return result
    # Legacy DFU bootloader caps packet payload at 20 bytes regardless of
    # negotiated MTU (BLEGATT_ATT_MTU_MAX=23 in bootloader firmware).
    mtu = 20
    progress("connect", f"Connected, MTU={client.mtu_size}, DFU chunk={mtu}", 30)

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
    notify_queue: asyncio.Queue[bytes] = asyncio.Queue()

    def on_notify(sender, data):
        notify_queue.put_nowait(bytes(data))

    await client.start_notify(DFU_CONTROL_UUID, on_notify,
                              bluez={"use_start_notify": True})
    await asyncio.sleep(1)

    async def wait_response(name, expected_opcode=None, timeout=30.0):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False, f"{name} timeout ({timeout}s)"
            try:
                response_data = await asyncio.wait_for(
                    notify_queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                return False, f"{name} timeout ({timeout}s)"

            if len(response_data) < 3:
                log.warning("Short notification (%d bytes), ignoring", len(response_data))
                continue

            # PKT_RCPT_NOTIF (0x11) — stale PRN from prior transfer, skip it
            if response_data[0] == 0x11:
                log.debug("Skipping stale PKT_RCPT_NOTIF while waiting for %s", name)
                continue

            # Command response (0x10)
            if response_data[0] != 0x10:
                log.warning("Unexpected notification opcode 0x%02x, ignoring", response_data[0])
                continue

            if expected_opcode is not None and response_data[1] != expected_opcode:
                log.warning("Response for opcode 0x%02x, expected 0x%02x, ignoring",
                            response_data[1], expected_opcode)
                continue

            if response_data[2] != 0x01:
                msg = f"{name} failed: result=0x{response_data[2]:02x}"
                log.error(msg)
                return False, msg
            return True, ""

    try:
        # START_DFU — image_type: 0x04=app, 0x02=bootloader, 0x01=softdevice
        # Size packet: [sd_size, bl_size, app_size] as 3x uint32 LE
        progress("dfu", f"START_DFU (type=0x{image_type:02x})...", 35)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x01, image_type]), response=True)
        if image_type == 0x04:
            size_pkt = struct.pack('<III', 0, 0, len(firmware))
        elif image_type == 0x02:
            size_pkt = struct.pack('<III', 0, len(firmware), 0)
        else:  # softdevice
            size_pkt = struct.pack('<III', len(firmware), 0, 0)
        await client.write_gatt_char(DFU_PACKET_UUID, size_pkt, response=False)
        # Flash erase takes ~25s for 500KB firmware (85ms per 4KB page)
        ok, msg = await wait_response("START_DFU", expected_opcode=0x01, timeout=60.0)
        if not ok:
            result["message"] = msg
            return result

        # INIT_DFU
        progress("dfu", "INIT_DFU...", 40)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x02, 0x00]), response=True)
        await client.write_gatt_char(DFU_PACKET_UUID,
                                     init_packet, response=False)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x02, 0x01]), response=True)
        ok, msg = await wait_response("INIT_DFU", expected_opcode=0x02, timeout=60.0)
        if not ok:
            result["message"] = msg
            return result

        # Set PRN
        prn_cmd = bytes([0x08]) + struct.pack('<H', PRN_INTERVAL)
        await client.write_gatt_char(DFU_CONTROL_UUID, prn_cmd, response=True)

        # RECEIVE_FIRMWARE
        progress("transfer", f"Sending {len(firmware)} bytes...", 45)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x03]), response=True)

        sent = 0
        pkt_count = 0
        last_pct = 0
        while sent < len(firmware):
            end = min(sent + mtu, len(firmware))
            chunk = firmware[sent:end]
            # Pad last chunk to word alignment (4 bytes) per bootloader requirement
            if end >= len(firmware) and len(chunk) % 4 != 0:
                chunk = chunk + b'\x00' * (4 - len(chunk) % 4)
            await client.write_gatt_char(DFU_PACKET_UUID, chunk, response=False)
            sent = end
            pkt_count += 1

            if PRN_INTERVAL > 0 and pkt_count % PRN_INTERVAL == 0:
                try:
                    prn_data = await asyncio.wait_for(notify_queue.get(), timeout=10.0)
                    if len(prn_data) >= 1 and prn_data[0] == 0x11:
                        rcvd = int.from_bytes(prn_data[1:5], 'little') if len(prn_data) >= 5 else 0
                        log.debug("PRN: bootloader received %d bytes", rcvd)
                except asyncio.TimeoutError:
                    log.error("PRN timeout at %d/%d bytes — aborting", sent, len(firmware))
                    result["message"] = f"PRN timeout at {sent}/{len(firmware)} bytes (connection lost or buffer overflow)"
                    return result

            pct = 45 + (sent * 50) // len(firmware)
            if pct >= last_pct + 5:
                progress("transfer", f"{(sent*100)//len(firmware)}%", pct)
                last_pct = pct

        progress("transfer", "Waiting for completion...", 95)
        ok, msg = await wait_response("RECEIVE_FIRMWARE", expected_opcode=0x03, timeout=60.0)
        if not ok:
            result["message"] = msg
            return result

        # VALIDATE
        progress("validate", "Validating firmware...", 97)
        await client.write_gatt_char(DFU_CONTROL_UUID,
                                     bytes([0x04]), response=True)
        ok, msg = await wait_response("VALIDATE", expected_opcode=0x04)
        if not ok:
            result["message"] = msg
            return result

        # ACTIVATE_AND_RESET
        progress("activate", "Activating and resetting...", 99)
        try:
            await client.write_gatt_char(DFU_CONTROL_UUID,
                                         bytes([0x05]), response=True)
        except Exception:
            pass  # Device disconnects immediately

        # Disconnect from bootloader (it's resetting anyway)
        try:
            await client.disconnect()
        except Exception:
            pass
        client = None  # Prevent double-disconnect in finally

        # Post-DFU verification: wait for device to reboot and verify it's alive
        progress("verify", "Waiting for device to reboot...", 99)
        await asyncio.sleep(5)  # Device needs time to boot new firmware

        # Clear BlueZ cache again (bootloader → app transition)
        await asyncio.to_thread(clear_bluez_state, boot_addr)
        await asyncio.to_thread(clear_bluez_state, app_ble_address)
        await asyncio.sleep(2)

        # Scan for device at its original app BLE address
        verified = False
        for verify_attempt in range(3):
            progress("verify", f"Scanning for rebooted device (attempt {verify_attempt + 1}/3)...", 99)
            dev = await BleakScanner.find_device_by_address(
                app_ble_address, timeout=10.0)
            if dev:
                verified = True
                break
            await asyncio.sleep(2)

        if verified:
            progress("done", "BLE DFU complete — device verified!", 100)
            elapsed = time.monotonic() - t0
            result.update(status="ok",
                          message="BLE DFU upload successful, device verified",
                          elapsed_s=round(elapsed, 1), verified=True)
        else:
            progress("done", "BLE DFU transfer complete but device not seen advertising", 100)
            elapsed = time.monotonic() - t0
            result.update(status="ok",
                          message="BLE DFU transfer complete (device not yet seen — may still be booting)",
                          elapsed_s=round(elapsed, 1), verified=False)

    except Exception as e:
        result["message"] = f"DFU transfer error: {e}"
    finally:
        if client:
            try:
                await client.disconnect()
            except Exception:
                pass

    result["elapsed_s"] = round(time.monotonic() - t0, 1)
    return result
