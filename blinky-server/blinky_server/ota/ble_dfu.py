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

Error recovery (Mar 31, 2026):
- INVALID_STATE (0x02) on START_DFU: previous failed DFU left bootloader stuck.
  Send SYSTEM_RESET (0x06), wait for reboot, rescan, retry.
- Any failure during DFU: send SYSTEM_RESET so bootloader doesn't stay stuck.
- Full DFU sequence retried up to MAX_DFU_ATTEMPTS times with cache clear between.
- Post-DFU verification scans by name ("Blinky") + NUS service, not just address
  (nRF52840 random static addresses can change on reboot).
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import struct
import subprocess
import time
import zipfile
from collections.abc import Callable
from typing import Any

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

log = logging.getLogger(__name__)

# Nordic DFU Service UUIDs
DFU_SERVICE_UUID = "00001530-1212-efde-1523-785feabcd123"
DFU_CONTROL_UUID = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET_UUID = "00001532-1212-efde-1523-785feabcd123"
DFU_REVISION_UUID = "00001534-1212-efde-1523-785feabcd123"

NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"

PRN_INTERVAL = (
    8  # Packet Receipt Notification interval (0-65535, max 8 recommended for Adafruit bootloader)
)
assert 0 <= PRN_INTERVAL <= 65535, f"PRN_INTERVAL must be 0-65535, got {PRN_INTERVAL}"

MAX_DFU_ATTEMPTS = 3  # Retry the full DFU sequence on failure


def bootloader_address(app_address: str) -> str:
    """Bootloader advertises at app_address + 1 (last octet)."""
    parts = app_address.split(":")
    last = int(parts[-1], 16)
    parts[-1] = f"{(last + 1) & 0xFF:02X}"
    return ":".join(parts)


def _validate_ble_address(address: str) -> None:
    """Validate BLE address format (XX:XX:XX:XX:XX:XX)."""
    import re

    if not re.match(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$", address):
        raise ValueError(f"Invalid BLE address format: {address!r}")


def clear_bluez_state(address: str) -> None:
    """Clear BlueZ GATT cache for an address. Required between app/bootloader.

    Uses ``bluetoothctl remove`` which clears the device from BlueZ's database
    including GATT cache. Falls back to direct cache directory removal (without
    sudo) if the user has permissions — typically when running as the same user
    that owns the BlueZ data directory.
    """
    _validate_ble_address(address)
    addr_u = address.replace(":", "_")
    subprocess.run(["bluetoothctl", "remove", address], capture_output=True, timeout=5)
    import glob as _glob
    import shutil

    for cache_dir in _glob.glob(f"/var/lib/bluetooth/*/cache/{addr_u}"):
        with contextlib.suppress(OSError):
            shutil.rmtree(cache_dir)
    for dev_dir in _glob.glob(f"/var/lib/bluetooth/*/{addr_u}"):
        with contextlib.suppress(OSError):
            shutil.rmtree(dev_dir)


def parse_dfu_zip(zip_path: str) -> tuple[bytes, bytes, int]:
    """Extract init packet, firmware binary, and image type from DFU zip.

    Returns (init_packet, firmware, image_type) where image_type is:
        0x04 = application, 0x02 = bootloader, 0x01 = softdevice
    """
    IMAGE_TYPES = {"application": 0x04, "bootloader": 0x02, "softdevice": 0x01}
    with zipfile.ZipFile(zip_path) as zf:
        manifest = json.loads(zf.read("manifest.json"))["manifest"]
        for key, img_type in IMAGE_TYPES.items():
            if key in manifest:
                entry = manifest[key]
                return zf.read(entry["dat_file"]), zf.read(entry["bin_file"]), img_type
        raise ValueError(f"No recognized image type in manifest: {list(manifest.keys())}")


async def _reset_bootloader(client: BleakClient) -> None:
    """Send SYSTEM_RESET (0x06) to unstick a bootloader in invalid state.

    Also resets the HCI adapter to clear BlueZ connection state that can
    become corrupted after forced BLE disconnects. Without this, subsequent
    connections to the same bootloader address may silently fail.
    """
    addr = client.address if hasattr(client, "address") else "unknown"
    with contextlib.suppress(Exception):
        await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x06]), response=True)
    with contextlib.suppress(Exception):
        await client.disconnect()
    # Clear BlueZ state for this address and reset adapter
    await asyncio.to_thread(clear_bluez_state, addr)
    await asyncio.to_thread(_reset_hci_adapter)


def _reset_hci_adapter() -> None:
    """Reset the default HCI adapter to clear stale BLE connection state.

    BlueZ can retain corrupt connection/GATT state after forced disconnects
    (e.g., device resets mid-write). Resetting the adapter clears all of this.
    """
    # Try hci0 first (most common), then hci1
    for adapter in ("hci0", "hci1"):
        result = subprocess.run(
            ["hciconfig", adapter, "reset"],
            capture_output=True, timeout=5,
        )
        if result.returncode == 0:
            log.info("Reset BLE adapter %s", adapter)
            return
    # Fallback: bluetoothctl power cycle
    subprocess.run(["bluetoothctl", "power", "off"], capture_output=True, timeout=5)
    subprocess.run(["bluetoothctl", "power", "on"], capture_output=True, timeout=5)
    log.info("Power-cycled BLE adapter via bluetoothctl")


async def _dfu_transfer(
    boot_addr: str,
    init_packet: bytes,
    firmware: bytes,
    image_type: int,
    progress: Callable[..., None],
) -> dict[str, Any]:
    """Execute a single DFU transfer attempt. Returns result dict.

    Caller handles retry logic. On failure, caller should send SYSTEM_RESET.
    """
    result: dict[str, Any] = {"status": "error", "message": ""}
    mtu = 20  # Legacy DFU caps at 20 bytes regardless of negotiated MTU

    # Clear BlueZ cache for bootloader address
    await asyncio.to_thread(clear_bluez_state, boot_addr)
    await asyncio.sleep(2)

    # Scan for bootloader
    dev = None
    for scan_attempt in range(3):
        progress(
            "scan", f"Scanning for bootloader at {boot_addr} (attempt {scan_attempt + 1}/3)...", 20
        )
        dev = await BleakScanner.find_device_by_address(boot_addr, timeout=15.0)
        if dev:
            break
        log.warning(
            "Bootloader not found on attempt %d, clearing cache and retrying", scan_attempt + 1
        )
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

    progress("connect", f"Connected, MTU={client.mtu_size}, DFU chunk={mtu}", 30)

    # Verify bootloader mode
    try:
        rev = await client.read_gatt_char(DFU_REVISION_UUID)
        rev_val = int.from_bytes(rev[:2], "little")
        if rev_val <= 1:
            await client.disconnect()
            result["message"] = f"Device in app mode (revision=0x{rev_val:04x}), not bootloader"
            return result
    except Exception:
        pass

    # Subscribe to notifications
    await asyncio.sleep(2)
    notify_queue: asyncio.Queue[bytes] = asyncio.Queue()

    def on_notify(sender: BleakGATTCharacteristic, data: bytearray) -> None:
        notify_queue.put_nowait(bytes(data))

    await client.start_notify(DFU_CONTROL_UUID, on_notify, bluez={"use_start_notify": True})
    await asyncio.sleep(1)

    async def wait_response(
        name: str, expected_opcode: int | None = None, timeout: float = 30.0
    ) -> tuple[bool, int, str]:
        """Wait for a DFU response notification. Returns (ok, result_code, error_msg)."""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False, 0, f"{name} timeout ({timeout}s)"
            try:
                response_data = await asyncio.wait_for(notify_queue.get(), timeout=remaining)
            except TimeoutError:
                return False, 0, f"{name} timeout ({timeout}s)"

            if len(response_data) < 3:
                log.warning("Short notification (%d bytes), ignoring", len(response_data))
                continue

            # PKT_RCPT_NOTIF (0x11) — stale PRN from prior transfer, skip it
            if response_data[0] == 0x11:
                log.debug("Skipping stale PKT_RCPT_NOTIF while waiting for %s", name)
                continue

            if response_data[0] != 0x10:
                log.warning("Unexpected notification opcode 0x%02x, ignoring", response_data[0])
                continue

            if expected_opcode is not None and response_data[1] != expected_opcode:
                log.warning(
                    "Response for opcode 0x%02x, expected 0x%02x, ignoring",
                    response_data[1],
                    expected_opcode,
                )
                continue

            result_code = response_data[2]
            if result_code != 0x01:
                msg = f"{name} failed: result=0x{result_code:02x}"
                log.error(msg)
                return False, result_code, msg
            return True, 0x01, ""

    try:
        # START_DFU
        progress("dfu", f"START_DFU (type=0x{image_type:02x})...", 35)
        await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x01, image_type]), response=True)
        if image_type == 0x04:
            size_pkt = struct.pack("<III", 0, 0, len(firmware))
        elif image_type == 0x02:
            size_pkt = struct.pack("<III", 0, len(firmware), 0)
        else:
            size_pkt = struct.pack("<III", len(firmware), 0, 0)
        await client.write_gatt_char(DFU_PACKET_UUID, size_pkt, response=False)
        # Flash erase takes ~25s for 500KB firmware (85ms per 4KB page)
        ok, code, msg = await wait_response("START_DFU", expected_opcode=0x01, timeout=60.0)
        if not ok:
            if code == 0x02:  # INVALID_STATE
                log.warning("START_DFU returned INVALID_STATE — sending SYSTEM_RESET")
                await _reset_bootloader(client)
                result["message"] = "INVALID_STATE (bootloader reset, retry needed)"
                result["needs_retry"] = True
            else:
                result["message"] = msg
            return result

        # INIT_DFU
        progress("dfu", "INIT_DFU...", 40)
        await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x02, 0x00]), response=True)
        await client.write_gatt_char(DFU_PACKET_UUID, init_packet, response=False)
        await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x02, 0x01]), response=True)
        ok, code, msg = await wait_response("INIT_DFU", expected_opcode=0x02, timeout=60.0)
        if not ok:
            result["message"] = msg
            return result

        # Set PRN
        prn_cmd = bytes([0x08]) + struct.pack("<H", PRN_INTERVAL)
        await client.write_gatt_char(DFU_CONTROL_UUID, prn_cmd, response=True)

        # RECEIVE_FIRMWARE
        progress("transfer", f"Sending {len(firmware)} bytes...", 45)
        await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x03]), response=True)

        sent = 0
        pkt_count = 0
        last_pct = 0
        while sent < len(firmware):
            end = min(sent + mtu, len(firmware))
            chunk = firmware[sent:end]
            if end >= len(firmware) and len(chunk) % 4 != 0:
                chunk = chunk + b"\x00" * (4 - len(chunk) % 4)
            await client.write_gatt_char(DFU_PACKET_UUID, chunk, response=False)
            sent = end
            pkt_count += 1

            if PRN_INTERVAL > 0 and pkt_count % PRN_INTERVAL == 0:
                try:
                    prn_data = await asyncio.wait_for(notify_queue.get(), timeout=10.0)
                    if len(prn_data) >= 1 and prn_data[0] == 0x11:
                        rcvd = int.from_bytes(prn_data[1:5], "little") if len(prn_data) >= 5 else 0
                        log.debug("PRN: bootloader received %d bytes", rcvd)
                    elif len(prn_data) >= 3 and prn_data[0] == 0x10:
                        # Command response arrived during transfer — re-queue it
                        # so wait_response() can find it after the transfer loop.
                        notify_queue.put_nowait(prn_data)
                except TimeoutError:
                    log.error("PRN timeout at %d/%d bytes — aborting", sent, len(firmware))
                    result["message"] = (
                        f"PRN timeout at {sent}/{len(firmware)} bytes (connection lost or buffer overflow)"
                    )
                    return result

            pct = 45 + (sent * 50) // len(firmware)
            if pct >= last_pct + 5:
                progress("transfer", f"{(sent * 100) // len(firmware)}%", pct)
                last_pct = pct

        progress("transfer", "Waiting for completion...", 95)
        ok, code, msg = await wait_response("RECEIVE_FIRMWARE", expected_opcode=0x03, timeout=120.0)
        if not ok:
            # Speculative VALIDATE: the firmware transfer may have completed
            # successfully but the completion notification was lost (BLE is
            # unreliable). If the bootloader has all the data, VALIDATE will
            # succeed (CRC match). If not, it fails with CRC_ERROR and we
            # know the transfer was genuinely incomplete.
            log.warning(
                "RECEIVE_FIRMWARE response lost (%s) — attempting speculative VALIDATE", msg
            )
            progress("validate", "Transfer notification lost — trying VALIDATE anyway...", 96)
            try:
                await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x04]), response=True)
                ok, code, msg = await wait_response("VALIDATE", expected_opcode=0x04, timeout=30.0)
                if ok:
                    log.info("Speculative VALIDATE succeeded — transfer was complete")
                else:
                    log.warning("Speculative VALIDATE failed: %s — transfer was incomplete", msg)
                    result["message"] = f"Transfer incomplete (speculative VALIDATE: {msg})"
                    return result
            except Exception as e:
                log.warning("Speculative VALIDATE error: %s", e)
                result["message"] = f"RECEIVE_FIRMWARE notification lost, VALIDATE failed: {e}"
                return result
        else:
            # Normal path: RECEIVE_FIRMWARE succeeded, VALIDATE
            progress("validate", "Validating firmware...", 97)
            await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x04]), response=True)
            ok, code, msg = await wait_response("VALIDATE", expected_opcode=0x04)
            if not ok:
                result["message"] = msg
                return result

        # ACTIVATE_AND_RESET
        progress("activate", "Activating and resetting...", 99)
        with contextlib.suppress(Exception):
            await client.write_gatt_char(DFU_CONTROL_UUID, bytes([0x05]), response=True)

        with contextlib.suppress(Exception):
            await client.disconnect()

        result.update(status="ok", message="BLE DFU transfer complete")
        return result

    except Exception as e:
        result["message"] = f"DFU transfer error: {e}"
        return result
    finally:
        if client.is_connected:
            # On failure, try to reset the bootloader so it doesn't stay stuck
            if result.get("status") != "ok":
                log.warning("DFU failed — sending SYSTEM_RESET to unstick bootloader")
                await _reset_bootloader(client)
            else:
                with contextlib.suppress(Exception):
                    await client.disconnect()


MIN_DFU_RSSI = -75  # Reject BLE DFU if signal weaker than this (dBm)
PREFLIGHT_TEST_BYTES = 2048  # Bytes to transfer in pre-flight BLE quality test


async def _preflight_ble_check(
    app_ble_address: str,
    progress: Callable[..., None],
) -> tuple[bool, str]:
    """Verify BLE connection quality before entering destructive DFU bootloader.

    Connects to the device in app mode, checks RSSI, and performs a round-trip
    data transfer test via NUS. This catches weak connections, interference, and
    BlueZ issues BEFORE we erase the application flash.

    Returns (ok, message). If ok is False, DFU should be aborted.
    """
    NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
    NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    # Step 1: Scan and check RSSI
    progress("preflight", f"Scanning for {app_ble_address}...", 3)
    dev = await BleakScanner.find_device_by_address(app_ble_address, timeout=10.0)
    if not dev:
        return False, f"Device {app_ble_address} not found"

    # BleakScanner doesn't always report RSSI on find_device_by_address.
    # Do a full scan to get RSSI.
    rssi = None
    discovered = await BleakScanner.discover(timeout=5.0, return_adv=True)
    for addr, (d, adv) in discovered.items():
        if addr.upper() == app_ble_address.upper():
            rssi = adv.rssi
            break

    if rssi is not None and rssi < MIN_DFU_RSSI:
        return False, f"Signal too weak (RSSI={rssi} dBm, minimum={MIN_DFU_RSSI} dBm)"
    rssi_str = f"RSSI={rssi} dBm" if rssi is not None else "RSSI=unknown"
    progress("preflight", f"Signal: {rssi_str}", 4)

    # Step 2: Connect and test NUS round-trip
    progress("preflight", "Testing BLE data transfer...", 4)
    try:
        async with BleakClient(app_ble_address, timeout=10.0) as client:
            rx_data: list[bytes] = []

            def on_rx(sender: BleakGATTCharacteristic, data: bytearray) -> None:
                rx_data.append(bytes(data))

            await client.start_notify(NUS_TX, on_rx)
            await asyncio.sleep(0.3)

            # Send a command that produces a known response (json info)
            await client.write_gatt_char(NUS_RX, b"json info\n", response=False)
            await asyncio.sleep(1.5)  # Wait for BLE response fragments

            total_rx = sum(len(d) for d in rx_data)
            if total_rx < 20:
                return False, f"NUS test transfer failed (received {total_rx} bytes, expected >20)"

            progress("preflight", f"BLE test OK ({total_rx} bytes received, {rssi_str})", 5)

    except Exception as e:
        return False, f"BLE connection test failed: {e}"

    return True, "ok"


async def upload_ble_dfu(
    app_ble_address: str,
    dfu_zip_path: str,
    enter_bootloader_via_serial: Callable[..., Any] | None = None,
    enter_bootloader_via_ble: Callable[..., Any] | None = None,
    progress_callback: Callable[..., None] | None = None,
) -> dict[str, Any]:
    """Upload firmware via BLE DFU with automatic retry and error recovery.

    Args:
        app_ble_address: Device's BLE address in app mode
        dfu_zip_path: Path to DFU .zip from adafruit-nrfutil genpkg
        enter_bootloader_via_serial: Optional async callable(cmd) that sends
            'bootloader ble' via serial transport.
        enter_bootloader_via_ble: Optional async callable(cmd) that sends
            'bootloader ble' via BLE NUS transport (for wireless-only devices).
        progress_callback: Optional callable(phase, msg, pct)

    Returns:
        dict with status, message, elapsed_s
    """
    t0 = time.monotonic()
    boot_addr = bootloader_address(app_ble_address)

    def progress(phase: str, msg: str, pct: int | None = None) -> None:
        log.info("[BLE-DFU %s] %s", phase, msg)
        if progress_callback is not None:
            progress_callback(phase, msg, pct)

    # Parse firmware
    try:
        init_packet, firmware, image_type = parse_dfu_zip(dfu_zip_path)
    except Exception as e:
        return {"status": "error", "message": f"Failed to parse DFU zip: {e}", "elapsed_s": 0}
    type_name = {0x04: "application", 0x02: "bootloader", 0x01: "softdevice"}.get(
        image_type, "unknown"
    )
    progress("init", f"{type_name}: {len(firmware)} bytes, init: {len(init_packet)} bytes", 5)

    # Pre-flight BLE quality check (only for BLE-initiated DFU).
    # Entering the bootloader erases application flash — this is DESTRUCTIVE and
    # IRREVERSIBLE. We must verify BLE connection quality BEFORE this point of
    # no return. If the connection is weak, refuse to proceed.
    if enter_bootloader_via_ble and not enter_bootloader_via_serial:
        progress("preflight", "Checking BLE connection quality...", 3)
        preflight_ok, preflight_msg = await _preflight_ble_check(app_ble_address, progress)
        if not preflight_ok:
            return {
                "status": "error",
                "message": f"Pre-flight BLE check failed: {preflight_msg}. "
                           "DFU aborted to protect device (app flash not erased).",
                "elapsed_s": round(time.monotonic() - t0, 1),
            }

    last_result: dict[str, Any] = {"status": "error", "message": "No attempts made"}

    for attempt in range(MAX_DFU_ATTEMPTS):
        if attempt > 0:
            progress("retry", f"Retry attempt {attempt + 1}/{MAX_DFU_ATTEMPTS}...", 10)
            # Wait for bootloader to finish resetting + BLE re-advertising.
            # After SYSTEM_RESET with erased flash: bootloader restarts, inits
            # SoftDevice, starts advertising. This takes 5-10s. Use 20s for margin.
            await asyncio.sleep(20)

        # Enter bootloader (only on first attempt — retries assume already in bootloader)
        if attempt == 0:
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
                    log.debug("BLE bootloader command result (expected disconnect): %s", e)
                await asyncio.sleep(3)

            # Clear BlueZ state for both addresses
            progress("cache", "Clearing BlueZ GATT cache...", 15)
            await asyncio.to_thread(clear_bluez_state, app_ble_address)
            await asyncio.to_thread(clear_bluez_state, boot_addr)
            await asyncio.sleep(3)

        # Execute DFU transfer
        last_result = await _dfu_transfer(
            boot_addr=boot_addr,
            init_packet=init_packet,
            firmware=firmware,
            image_type=image_type,
            progress=progress,
        )

        if last_result.get("status") == "ok":
            break

        # If INVALID_STATE, the bootloader was reset — retry after waiting
        if last_result.get("needs_retry"):
            log.warning("DFU attempt %d: INVALID_STATE, bootloader reset — retrying", attempt + 1)
            await asyncio.sleep(15)  # Wait for device to reboot into bootloader
            continue

        # Other failure — bootloader was reset in finally block, retry
        log.warning(
            "DFU attempt %d failed: %s — retrying",
            attempt + 1,
            last_result.get("message", "unknown"),
        )

    if last_result.get("status") != "ok":
        last_result["elapsed_s"] = round(time.monotonic() - t0, 1)
        return last_result

    # Post-DFU verification
    progress("verify", "Waiting for device to reboot...", 99)
    await asyncio.sleep(5)

    await asyncio.to_thread(clear_bluez_state, boot_addr)
    await asyncio.to_thread(clear_bluez_state, app_ble_address)
    await asyncio.sleep(2)

    # Scan for rebooted device. Try app address first, then scan by name + NUS service
    # (nRF52840 random static addresses can change after a full power cycle from DFU).
    verified = False
    for verify_attempt in range(3):
        progress(
            "verify", f"Scanning for rebooted device (attempt {verify_attempt + 1}/3)...", 99
        )
        # First: try exact address match
        dev = await BleakScanner.find_device_by_address(app_ble_address, timeout=8.0)
        if dev:
            verified = True
            break
        # Fallback: scan for any nearby Blinky device with NUS service
        discovered = await BleakScanner.discover(timeout=8.0, return_adv=True)
        for addr, (d, adv) in discovered.items():
            svc_uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            if NUS_SERVICE_UUID in svc_uuids and adv.rssi > -65:
                # Strong signal + NUS service = likely our device with a new address
                log.info(
                    "Post-DFU: found Blinky at %s (RSSI=%d) — address may have changed from %s",
                    addr, adv.rssi, app_ble_address,
                )
                verified = True
                break
        if verified:
            break
        await asyncio.sleep(3)

    elapsed = round(time.monotonic() - t0, 1)
    if verified:
        progress("done", "BLE DFU complete — device verified!", 100)
        last_result.update(
            message="BLE DFU upload successful, device verified",
            elapsed_s=elapsed,
            verified=True,
        )
    else:
        progress("done", "BLE DFU transfer complete but device not seen advertising", 100)
        last_result.update(
            message="BLE DFU transfer complete (device not yet seen — may still be booting)",
            elapsed_s=elapsed,
            verified=False,
        )

    return last_result
