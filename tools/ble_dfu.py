#!/usr/bin/env python3
"""BLE DFU (Device Firmware Update) tool for nRF52840 devices.

Uploads firmware to nRF52840 devices over BLE using the Nordic Legacy DFU
protocol (SDK v11 opcodes). The Adafruit nRF52 bootloader (v0.6.2) implements
this legacy protocol, NOT the newer Secure DFU (SDK v12+). The device must be
running firmware with the BLEDfu service enabled (Adafruit Bluefruit52Lib).

Protocol:
1. Connect to device via BLE NUS/DFU service
2. Write START_DFU (0x01) to DFU Control characteristic
3. Device reboots into Adafruit nRF52 bootloader (BLE DFU mode)
4. Reconnect to bootloader's DFU service
5. Transfer firmware using Nordic Legacy DFU protocol
6. Device reboots into new firmware

Usage:
    python3 ble_dfu.py --address F4:15:6D:FA:4D:93 --firmware firmware.zip
    python3 ble_dfu.py --scan  # Discover DFU-capable devices

Requires: bleak >= 0.22, adafruit-nrfutil (for genpkg)
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import struct
import sys
import zipfile
import json
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("ERROR: bleak not installed. Run: pip install bleak")
    sys.exit(1)

log = logging.getLogger(__name__)

# Nordic DFU Service UUIDs (used by both app BLEDfu and bootloader)
DFU_SERVICE_UUID = "00001530-1212-efde-1523-785feabcd123"
DFU_CONTROL_UUID = "00001531-1212-efde-1523-785feabcd123"
DFU_PACKET_UUID = "00001532-1212-efde-1523-785feabcd123"
# Nordic NUS UUID (for discovering app-mode devices)
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"


# Secure DFU v2 opcodes (nRF SDK v12+, revision 0x0008)
# Confirmed by reading DFU Revision characteristic from Adafruit bootloader v0.6.2
class DfuOpcode:
    CREATE = 0x01
    SET_PRN = 0x02      # Set Packet Receipt Notification interval
    CALC_CRC = 0x03
    EXECUTE = 0x04
    SELECT = 0x06
    RESPONSE = 0x60

class DfuObjType:
    COMMAND = 0x01      # Init packet (manifest)
    DATA = 0x02         # Firmware binary

# Packet Receipt Notification interval (0 = disabled)
NUM_PACKETS_BETWEEN_NOTIF = 0  # Disabled for simplicity; enable if transfer fails

# Secure DFU v2 result codes
class DfuResult:
    INVALID = 0x00
    SUCCESS = 0x01
    OP_NOT_SUPPORTED = 0x02
    INVALID_PARAM = 0x03
    INSUFFICIENT_RESOURCES = 0x04
    INVALID_OBJECT = 0x05
    UNSUPPORTED_TYPE = 0x07
    OPERATION_NOT_PERMITTED = 0x08
    OPERATION_FAILED = 0x0A

    _NAMES = {
        0x00: "INVALID", 0x01: "SUCCESS", 0x02: "OP_NOT_SUPPORTED",
        0x03: "INVALID_PARAM", 0x04: "INSUFFICIENT_RESOURCES",
        0x05: "INVALID_OBJECT", 0x07: "UNSUPPORTED_TYPE",
        0x08: "OPERATION_NOT_PERMITTED", 0x0A: "OPERATION_FAILED",
    }

    @staticmethod
    def lookup(code: int) -> str:
        return DfuResult._NAMES.get(code, f"UNKNOWN(0x{code:02x})")


class BleDfu:
    """BLE DFU client for Adafruit nRF52 bootloader (Secure DFU v2)."""

    def __init__(self, address: str):
        self.address = address
        self._client: BleakClient | None = None
        self._response_event = asyncio.Event()
        self._response_data: bytearray = bytearray()
        self._mtu = 20

    async def upload(self, dfu_zip_path: str) -> bool:
        """Upload a DFU zip package to the device.

        Args:
            dfu_zip_path: Path to .zip file created by adafruit-nrfutil genpkg

        Returns:
            True if successful
        """
        init_packet, firmware_bin = self._parse_dfu_zip(dfu_zip_path)
        log.info("Firmware: %d bytes, init packet: %d bytes",
                 len(firmware_bin), len(init_packet))

        # Phase 1: Connect and trigger DFU mode
        log.info("Connecting to %s in app mode...", self.address)
        await self._enter_dfu_mode()

        # Phase 2: Reconnect to bootloader
        log.info("Reconnecting to bootloader...")
        await asyncio.sleep(3)  # Wait for bootloader to start advertising
        await self._connect_bootloader()

        try:
            # Read DFU Revision to confirm protocol version
            try:
                DFU_REVISION_UUID = "00001534-1212-efde-1523-785feabcd123"
                rev_data = await self._client.read_gatt_char(DFU_REVISION_UUID)
                rev = int.from_bytes(rev_data[:2], 'little') if len(rev_data) >= 2 else 0
                log.info("DFU Revision: 0x%04x (%s)",
                         rev, "Legacy DFU" if rev <= 1 else "Secure DFU v2")
            except Exception as e:
                log.warning("Could not read DFU Revision: %s", e)

            # Phase 3: Transfer init packet (command object)
            log.info("Sending init packet (%d bytes)...", len(init_packet))
            await self._transfer_object(DfuObjType.COMMAND, init_packet)

            # Phase 4: Transfer firmware (data object)
            log.info("Sending firmware (%d bytes)...", len(firmware_bin))
            await self._transfer_object(DfuObjType.DATA, firmware_bin)

            log.info("DFU complete! Device rebooting into new firmware.")
        finally:
            await self._disconnect()

        return True

    def _parse_dfu_zip(self, zip_path: str) -> tuple[bytes, bytes]:
        """Extract init packet and firmware binary from DFU zip."""
        with zipfile.ZipFile(zip_path, 'r') as zf:
            manifest = json.loads(zf.read('manifest.json'))
            app = manifest.get('manifest', {}).get('application', {})
            if not app:
                raise ValueError("No application entry in DFU manifest")

            dat_file = app.get('dat_file', '')
            bin_file = app.get('bin_file', '')
            if not dat_file or not bin_file:
                raise ValueError(f"Missing dat_file or bin_file in manifest: {app}")

            init_packet = zf.read(dat_file)
            firmware_bin = zf.read(bin_file)
            return init_packet, firmware_bin

    async def _enter_dfu_mode(self):
        """Connect to app-mode device and trigger reboot into DFU bootloader."""
        self._client = BleakClient(self.address, timeout=10.0)
        await self._client.connect()
        log.info("Connected (app mode), MTU=%d", self._client.mtu_size)

        # Must subscribe to DFU Control notifications BEFORE writing START_DFU.
        # The BLEDfu authorization callback rejects writes if notifications aren't enabled.
        await self._client.start_notify(DFU_CONTROL_UUID, self._on_notification)
        await asyncio.sleep(0.5)  # Let subscription propagate

        # Write START_DFU (0x01) to trigger bootloader entry
        log.info("Sending START_DFU command...")
        try:
            await self._client.write_gatt_char(
                DFU_CONTROL_UUID, bytes([0x01]), response=True
            )
        except Exception as e:
            # Device may disconnect immediately after processing START_DFU
            log.debug("Write result (may disconnect): %s", e)

        # Device will disconnect and reboot into bootloader
        await asyncio.sleep(2)
        try:
            await self._client.disconnect()
        except Exception:
            pass  # Already disconnected
        self._client = None

    async def _connect_bootloader(self):
        """Connect to device in bootloader DFU mode."""
        # Clear BlueZ GATT cache — the bootloader has a different GATT table
        # than the app, but BlueZ reuses cached handles for the same address.
        import subprocess
        cache_dir = f"/var/lib/bluetooth/*/cache/{self.address.replace(':', '_')}"
        subprocess.run(f"sudo rm -rf {cache_dir}", shell=True,
                       capture_output=True, timeout=5)
        # Also try without sudo (some systems allow user access)
        subprocess.run(f"rm -rf {cache_dir}", shell=True,
                       capture_output=True, timeout=5)
        # Restart bluetooth to pick up cache removal
        subprocess.run(["sudo", "systemctl", "restart", "bluetooth"],
                       capture_output=True, timeout=10)
        await asyncio.sleep(2)  # Let BlueZ restart

        # The bootloader re-advertises with the same address
        for attempt in range(10):
            try:
                self._client = BleakClient(self.address, timeout=5.0)
                await self._client.connect()
                self._mtu = min(self._client.mtu_size - 3, 240)
                log.info("Connected to bootloader, MTU=%d (payload=%d)",
                         self._client.mtu_size, self._mtu)

                # Enable DFU Control notifications.
                # The Adafruit bootloader requires CCCD to be properly written
                # before accepting DFU commands (returns Unlikely Error otherwise).
                # Try multiple approaches to ensure CCCD is set:

                # Approach 1: Write CCCD descriptor directly via handle
                cccd_written = False
                try:
                    ctrl_char = self._client.services.get_characteristic(
                        DFU_CONTROL_UUID
                    )
                    if ctrl_char:
                        for desc in ctrl_char.descriptors:
                            if "2902" in str(desc.uuid):
                                log.debug("Writing CCCD at handle %d", desc.handle)
                                await self._client.write_gatt_descriptor(
                                    desc.handle, b'\x01\x00'
                                )
                                cccd_written = True
                                log.info("CCCD enabled via direct descriptor write")
                                break
                except Exception as e:
                    log.debug("Direct CCCD write failed: %s", e)

                # Approach 2: Use start_notify as backup
                if not cccd_written:
                    log.debug("Falling back to start_notify for CCCD")
                await self._client.start_notify(
                    DFU_CONTROL_UUID, self._on_notification,
                    bluez={"use_start_notify": True},
                )
                await asyncio.sleep(1.0)  # Let subscription propagate

                # List services for debugging
                for svc in self._client.services:
                    log.debug("  Bootloader service: %s", svc.uuid)
                    for char in svc.characteristics:
                        log.debug("    Char: %s [%s]", char.uuid,
                                  ','.join(char.properties))
                return
            except Exception as e:
                log.debug("Bootloader connect attempt %d: %s", attempt + 1, e)
                await asyncio.sleep(2)

        raise ConnectionError(f"Failed to connect to bootloader at {self.address}")

    # ---- Secure DFU v2 transfer ----

    async def _set_prn(self, interval: int):
        """Set Packet Receipt Notification interval (0 = disabled)."""
        cmd = bytes([DfuOpcode.SET_PRN]) + struct.pack('<H', interval)
        await self._send_command(DfuOpcode.SET_PRN, cmd)
        log.info("PRN interval set to %d", interval)

    async def _transfer_object(self, obj_type: int, data: bytes):
        """Transfer a DFU object (init packet or firmware) using Secure DFU v2.

        For each max_size chunk:
        1. SELECT to get max object size and current state
        2. CREATE object with chunk size
        3. Write data in MTU-sized packets via PACKET characteristic
        4. CALC_CRC to verify
        5. EXECUTE to finalize
        """
        # SELECT to get max object size
        resp = await self._send_command(
            DfuOpcode.SELECT,
            bytes([DfuOpcode.SELECT, obj_type]),
        )
        max_size, offset, crc32 = struct.unpack('<III', resp)
        log.info("SELECT type=%d: max_size=%d, offset=%d, crc32=0x%08x",
                 obj_type, max_size, offset, crc32)

        # Transfer in chunks of max_size
        pos = 0
        while pos < len(data):
            chunk = data[pos:pos + max_size]

            # CREATE object
            create_cmd = bytes([DfuOpcode.CREATE, obj_type]) + \
                struct.pack('<I', len(chunk))
            await self._send_command(DfuOpcode.CREATE, create_cmd)

            # Write data in MTU-sized packets
            for i in range(0, len(chunk), self._mtu):
                pkt = chunk[i:i + self._mtu]
                await self._client.write_gatt_char(
                    DFU_PACKET_UUID, pkt, response=False
                )
                # Small delay for BLE flow control
                if i > 0 and i % (self._mtu * 8) == 0:
                    await asyncio.sleep(0.01)

            # Verify CRC
            crc_resp = await self._send_command(
                DfuOpcode.CALC_CRC,
                bytes([DfuOpcode.CALC_CRC]),
            )
            reported_offset, reported_crc = struct.unpack('<II', crc_resp)
            expected_crc = self._crc32(data[:pos + len(chunk)])
            if reported_crc != expected_crc:
                raise RuntimeError(
                    f"CRC mismatch at offset {pos + len(chunk)}: "
                    f"expected 0x{expected_crc:08x}, got 0x{reported_crc:08x}"
                )
            log.debug("CRC OK at offset %d: 0x%08x", reported_offset, reported_crc)

            # EXECUTE to finalize this chunk
            await self._send_command(
                DfuOpcode.EXECUTE,
                bytes([DfuOpcode.EXECUTE]),
            )

            pos += len(chunk)
            pct = (pos * 100) // len(data)
            log.info("Progress: %d%% (%d/%d bytes)", pct, pos, len(data))

    # ---- Command / response handling ----

    async def _send_command(self, expected_opcode: int, cmd: bytes,
                            timeout: float = 30.0) -> bytes:
        """Send a DFU command and wait for response notification.

        Returns the response payload (bytes after the 3-byte header).
        """
        self._response_event.clear()
        self._response_data = bytearray()

        log.debug("DFU cmd: %s (expecting 0x%02x)", cmd.hex(), expected_opcode)
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID, cmd, response=True
        )
        log.debug("DFU cmd acknowledged")

        try:
            await asyncio.wait_for(self._response_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(
                f"Timeout ({timeout}s) waiting for response to "
                f"opcode 0x{expected_opcode:02x}"
            )

        resp = self._response_data
        if len(resp) < 3:
            raise RuntimeError(f"Short response ({len(resp)} bytes): {resp.hex()}")

        if resp[0] != DfuOpcode.RESPONSE:
            raise RuntimeError(
                f"Unexpected response type: 0x{resp[0]:02x} "
                f"(expected 0x{DfuOpcode.RESPONSE:02x})"
            )
        if resp[1] != expected_opcode:
            raise RuntimeError(
                f"Response opcode mismatch: expected 0x{expected_opcode:02x}, "
                f"got 0x{resp[1]:02x}"
            )
        if resp[2] != DfuResult.SUCCESS:
            raise RuntimeError(
                f"DFU error: {DfuResult.lookup(resp[2])} "
                f"for opcode 0x{expected_opcode:02x}"
            )

        return bytes(resp[3:])

    def _on_notification(self, sender, data: bytearray):
        """Handle DFU Control notification."""
        log.debug("DFU notification: %s", data.hex())
        self._response_data = data
        self._response_event.set()

    async def _disconnect(self):
        if self._client:
            try:
                await self._client.disconnect()
            except Exception:
                pass
            self._client = None

    @staticmethod
    def _crc32(data: bytes) -> int:
        """Calculate CRC32 matching Nordic DFU protocol."""
        import binascii
        return binascii.crc32(data) & 0xFFFFFFFF


async def scan_devices(timeout: float = 5.0):
    """Scan for BLE devices with DFU or NUS services."""
    print(f"Scanning for BLE devices ({timeout}s)...")
    devices = await BleakScanner.discover(
        timeout=timeout,
        return_adv=True,
    )

    dfu_devices = []
    for addr, (dev, adv) in devices.items():
        service_uuids = [str(u).lower() for u in (adv.service_uuids or [])]
        has_dfu = DFU_SERVICE_UUID in service_uuids
        has_nus = NUS_SERVICE_UUID in service_uuids
        if has_dfu or has_nus:
            mode = ("DFU bootloader" if has_dfu and not has_nus
                    else "App (NUS+DFU)" if has_dfu
                    else "App (NUS only)")
            dfu_devices.append((addr, dev.name, adv.rssi, mode))
            print(f"  {addr}  {dev.name or 'Unknown':20s}  "
                  f"RSSI={adv.rssi}  {mode}")

    if not dfu_devices:
        print("No DFU-capable devices found.")
    return dfu_devices


def generate_dfu_package(firmware_bin: str, output_zip: str,
                         sd_req: str = "0xCB"):
    """Generate a DFU zip package from a firmware binary.

    Uses adafruit-nrfutil genpkg. sd_req=0xCB is SoftDevice S140 v7.3.0.
    """
    import subprocess
    cmd = [
        "adafruit-nrfutil", "dfu", "genpkg",
        "--application", firmware_bin,
        "--sd-req", sd_req,
        output_zip,
    ]
    log.info("Running: %s", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"genpkg failed: {result.stderr}")
    log.info("Generated DFU package: %s", output_zip)


async def main():
    parser = argparse.ArgumentParser(
        description="BLE DFU for nRF52840 devices (Secure DFU v2)"
    )
    parser.add_argument("--scan", action="store_true",
                        help="Scan for DFU-capable devices")
    parser.add_argument("--address", "-a",
                        help="BLE address of target device")
    parser.add_argument("--firmware", "-f",
                        help="Path to DFU .zip package")
    parser.add_argument("--bin",
                        help="Path to raw .bin firmware (auto-generates .zip)")
    parser.add_argument("--sd-req", default="0xCB",
                        help="SoftDevice requirement (default: 0xCB for S140 7.3.0)")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-5s %(message)s",
        datefmt="%H:%M:%S",
    )

    if args.scan:
        await scan_devices()
        return

    if not args.address:
        parser.error("--address required (use --scan to find devices)")

    # Determine firmware path
    dfu_zip = args.firmware
    if args.bin:
        # Generate DFU zip from raw binary
        dfu_zip = args.bin.replace('.bin', '_dfu.zip')
        generate_dfu_package(args.bin, dfu_zip, args.sd_req)

    if not dfu_zip:
        parser.error("--firmware or --bin required")

    if not Path(dfu_zip).exists():
        parser.error(f"File not found: {dfu_zip}")

    # Perform DFU
    dfu = BleDfu(args.address)
    try:
        success = await dfu.upload(dfu_zip)
        if success:
            print("\nDFU successful! Device is rebooting with new firmware.")
        else:
            print("\nDFU failed.")
            sys.exit(1)
    except Exception as e:
        log.error("DFU failed: %s", e)
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
