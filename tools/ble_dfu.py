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


# Legacy Nordic DFU opcodes (SDK v11)
# Reference: nordicsemi/dfu/dfu_transport_ble.py (DfuOpcodesBle)
class DfuOpcode:
    START_DFU = 1
    INITIALIZE_DFU = 2
    RECEIVE_FIRMWARE_IMAGE = 3
    VALIDATE_FIRMWARE_IMAGE = 4
    ACTIVATE_FIRMWARE_AND_RESET = 5
    SYSTEM_RESET = 6
    REQ_PKT_RCPT_NOTIFICATION = 8
    RESPONSE = 16           # 0x10
    PKT_RCPT_NOTIF = 17     # 0x11


# Program mode for START_DFU (from nordicsemi.dfu.model.HexType)
DFU_MODE_APPLICATION = 4

# Packet Receipt Notification interval (0 = disabled)
NUM_PACKETS_BETWEEN_NOTIF = 10


# Legacy DFU result codes
# Reference: nordicsemi/dfu/dfu_transport_ble.py (DfuErrorCodeBle)
class DfuResult:
    SUCCESS = 1
    INVALID_STATE = 2
    NOT_SUPPORTED = 3
    DATA_SIZE_EXCEEDS_LIMIT = 4
    CRC_ERROR = 5
    OPERATION_FAILED = 6

    _NAMES = {
        1: "SUCCESS", 2: "INVALID_STATE", 3: "NOT_SUPPORTED",
        4: "DATA_SIZE_EXCEEDS_LIMIT", 5: "CRC_ERROR", 6: "OPERATION_FAILED",
    }

    @staticmethod
    def lookup(code: int) -> str:
        return DfuResult._NAMES.get(code, f"UNKNOWN(0x{code:02x})")


class BleDfu:
    """BLE DFU client for Adafruit nRF52 bootloader (Legacy Nordic DFU)."""

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
            # Phase 3: START_DFU + image size
            await self._send_start_dfu(len(firmware_bin))

            # Phase 4: INITIALIZE_DFU + init packet (.dat)
            await self._send_init_packet(init_packet)

            # Phase 5: Set up PRN + RECEIVE_FIRMWARE_IMAGE + firmware data
            await self._send_firmware(firmware_bin)

            # Phase 6: VALIDATE_FIRMWARE_IMAGE
            await self._send_validate()

            # Phase 7: ACTIVATE_FIRMWARE_AND_RESET
            await self._send_activate()

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
        # The bootloader re-advertises with the same address
        for attempt in range(10):
            try:
                self._client = BleakClient(self.address, timeout=5.0)
                await self._client.connect()
                self._mtu = min(self._client.mtu_size - 3, 240)
                log.info("Connected to bootloader, MTU=%d (payload=%d)",
                         self._client.mtu_size, self._mtu)

                # Subscribe to DFU Control notifications (required before any write)
                await self._client.start_notify(
                    DFU_CONTROL_UUID, self._on_notification
                )
                await asyncio.sleep(1.0)  # Let subscription propagate
                return
            except Exception as e:
                log.debug("Bootloader connect attempt %d: %s", attempt + 1, e)
                await asyncio.sleep(2)

        raise ConnectionError(f"Failed to connect to bootloader at {self.address}")

    # ---- Legacy DFU transfer phases ----

    async def _send_start_dfu(self, app_size: int):
        """Phase 3: Send START_DFU + 12-byte image size packet."""
        log.info("Sending START_DFU (app_size=%d)...", app_size)

        # Write START_DFU opcode + mode byte to CONTROL
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.START_DFU, DFU_MODE_APPLICATION]),
            response=True,
        )

        # Write 12-byte image size packet to PACKET:
        #   SD size (u32 LE) = 0, BL size (u32 LE) = 0, App size (u32 LE)
        size_packet = struct.pack('<III', 0, 0, app_size)
        await self._client.write_gatt_char(
            DFU_PACKET_UUID, size_packet, response=False
        )

        # Wait for response: [0x10, 0x01, 0x01] = RESPONSE, START_DFU, SUCCESS
        await self._wait_for_response(DfuOpcode.START_DFU)
        log.info("START_DFU accepted.")

    async def _send_init_packet(self, init_data: bytes):
        """Phase 4: Send init packet (.dat file) using INITIALIZE_DFU."""
        log.info("Sending INITIALIZE_DFU (init packet, %d bytes)...", len(init_data))

        # Signal start of init packet: INITIALIZE_DFU + 0x00
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.INITIALIZE_DFU, 0x00]),
            response=True,
        )

        # Write init packet data in 20-byte chunks on PACKET
        # (Legacy DFU uses fixed 20-byte chunks for init, not MTU-negotiated)
        chunk_size = 20
        for i in range(0, len(init_data), chunk_size):
            pkt = init_data[i:i + chunk_size]
            await self._client.write_gatt_char(
                DFU_PACKET_UUID, pkt, response=False
            )

        # Signal end of init packet: INITIALIZE_DFU + 0x01
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.INITIALIZE_DFU, 0x01]),
            response=True,
        )

        # Wait for response: [0x10, 0x02, 0x01]
        await self._wait_for_response(DfuOpcode.INITIALIZE_DFU, timeout=60.0)
        log.info("Init packet accepted.")

    async def _send_firmware(self, firmware: bytes):
        """Phase 5: Send firmware binary using RECEIVE_FIRMWARE_IMAGE + PRN."""
        # Request packet receipt notification every N packets
        if NUM_PACKETS_BETWEEN_NOTIF:
            log.info("Setting PRN interval: every %d packets",
                     NUM_PACKETS_BETWEEN_NOTIF)
            prn_data = struct.pack('<H', NUM_PACKETS_BETWEEN_NOTIF)
            await self._client.write_gatt_char(
                DFU_CONTROL_UUID,
                bytes([DfuOpcode.REQ_PKT_RCPT_NOTIFICATION]) + prn_data,
                response=True,
            )

        # Signal RECEIVE_FIRMWARE_IMAGE
        log.info("Sending firmware (%d bytes, MTU payload=%d)...",
                 len(firmware), self._mtu)
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.RECEIVE_FIRMWARE_IMAGE]),
            response=True,
        )

        # Stream firmware in MTU-sized chunks on PACKET
        chunk_size = self._mtu
        packets_sent = 0
        total = len(firmware)
        last_pct = -1

        for i in range(0, total, chunk_size):
            # Wait for PRN before sending next batch
            if NUM_PACKETS_BETWEEN_NOTIF and packets_sent > 0:
                if (packets_sent % NUM_PACKETS_BETWEEN_NOTIF) == 0:
                    await self._wait_for_prn()

            pkt = firmware[i:i + chunk_size]
            await self._client.write_gatt_char(
                DFU_PACKET_UUID, pkt, response=False
            )
            packets_sent += 1

            # Progress reporting
            pct = min(100, ((i + chunk_size) * 100) // total)
            if pct >= last_pct + 5:  # Report every 5%
                log.info("Progress: %d%% (%d/%d bytes)",
                         pct, min(i + chunk_size, total), total)
                last_pct = pct

        # Wait for final response: [0x10, 0x03, 0x01]
        await self._wait_for_response(DfuOpcode.RECEIVE_FIRMWARE_IMAGE)
        log.info("Firmware transfer complete.")

    async def _send_validate(self):
        """Phase 6: Send VALIDATE_FIRMWARE_IMAGE."""
        log.info("Validating firmware...")
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.VALIDATE_FIRMWARE_IMAGE]),
            response=True,
        )
        await self._wait_for_response(DfuOpcode.VALIDATE_FIRMWARE_IMAGE)
        log.info("Firmware validated OK.")

    async def _send_activate(self):
        """Phase 7: Send ACTIVATE_FIRMWARE_AND_RESET. Device will reboot."""
        log.info("Activating firmware and resetting device...")
        try:
            await self._client.write_gatt_char(
                DFU_CONTROL_UUID,
                bytes([DfuOpcode.ACTIVATE_FIRMWARE_AND_RESET]),
                response=True,
            )
        except Exception as e:
            # Device may reset immediately, disconnecting before ATT response
            log.debug("Activate result (device may reset): %s", e)

    # ---- Response handling ----

    async def _wait_for_response(self, expected_opcode: int,
                                 timeout: float = 30.0):
        """Wait for a RESPONSE (0x10) notification from the bootloader."""
        self._response_event.clear()

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

        resp_type = resp[0]
        resp_opcode = resp[1]
        resp_result = resp[2]

        if resp_type != DfuOpcode.RESPONSE:
            raise RuntimeError(
                f"Unexpected response type: 0x{resp_type:02x} "
                f"(expected RESPONSE=0x{DfuOpcode.RESPONSE:02x})"
            )
        if resp_opcode != expected_opcode:
            raise RuntimeError(
                f"Response opcode mismatch: expected 0x{expected_opcode:02x}, "
                f"got 0x{resp_opcode:02x}"
            )
        if resp_result != DfuResult.SUCCESS:
            raise RuntimeError(
                f"DFU error: {DfuResult.lookup(resp_result)} "
                f"for opcode 0x{expected_opcode:02x}"
            )

    async def _wait_for_prn(self, timeout: float = 30.0):
        """Wait for Packet Receipt Notification (0x11) during firmware transfer."""
        self._response_event.clear()

        try:
            await asyncio.wait_for(self._response_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError("Timeout waiting for packet receipt notification")

        resp = self._response_data
        if len(resp) >= 1 and resp[0] == DfuOpcode.PKT_RCPT_NOTIF:
            return  # PRN received, continue sending
        elif len(resp) >= 3 and resp[0] == DfuOpcode.RESPONSE:
            # Got an error response instead of PRN
            raise RuntimeError(
                f"Error during firmware transfer: "
                f"{DfuResult.lookup(resp[2])} (opcode 0x{resp[1]:02x})"
            )
        else:
            log.warning("Unexpected notification during PRN wait: %s", resp.hex())

    def _on_notification(self, sender, data: bytearray):
        """Handle DFU Control notification."""
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
        description="BLE DFU for nRF52840 devices (Legacy Nordic DFU)"
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
