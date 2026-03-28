#!/usr/bin/env python3
"""BLE DFU (Device Firmware Update) tool for nRF52840 devices.

Uploads firmware to nRF52840 devices over BLE using the Nordic Legacy DFU
protocol (SDK v11 opcodes). The Adafruit nRF52 bootloader (v0.6.2) implements
this legacy protocol. Despite reporting DFU Revision 0x0008, this is just the
bootloader version number, NOT Secure DFU v2.

Protocol:
1. Connect to device via BLE (app mode with BLEDfu service)
2. Write 0x01 to DFU Control characteristic to trigger bootloader entry
3. Device reboots into Adafruit nRF52 bootloader (BLE DFU mode)
4. Reconnect to bootloader's DFU service
5. Transfer firmware using Nordic Legacy DFU protocol:
   START_DFU -> INIT_DFU -> RECEIVE_FIRMWARE -> VALIDATE -> ACTIVATE_AND_RESET
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
DFU_REVISION_UUID = "00001534-1212-efde-1523-785feabcd123"
# Nordic NUS UUID (for discovering app-mode devices)
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"


# Legacy DFU opcodes (Nordic SDK v11)
# Reference: Adafruit_nRF52_Bootloader/lib/sdk11/components/libraries/bootloader_dfu/dfu_transport_ble.c
class DfuOpcode:
    START_DFU = 0x01            # Start DFU, param: image type
    INIT_DFU = 0x02             # Init DFU, param: 0x00=start, 0x01=complete
    RECEIVE_FIRMWARE = 0x03     # Begin receiving firmware data
    VALIDATE = 0x04             # Validate received firmware
    ACTIVATE_AND_RESET = 0x05   # Apply firmware and reset
    RESET = 0x06                # Reset without applying
    PKT_RCPT_NOTIF_REQ = 0x08  # Set packet receipt notification interval
    RESPONSE = 0x10             # Response notification opcode
    PKT_RCPT_NOTIF = 0x11      # Packet receipt notification


# Legacy DFU image types
class DfuImageType:
    SOFTDEVICE = 0x01
    BOOTLOADER = 0x02
    SD_AND_BL = 0x03
    APPLICATION = 0x04


# Legacy DFU result codes
class DfuResult:
    SUCCESS = 0x01
    INVALID_STATE = 0x02
    NOT_SUPPORTED = 0x03
    DATA_SIZE_EXCEEDS_LIMIT = 0x04
    CRC_ERROR = 0x05
    OPERATION_FAILED = 0x06

    _NAMES = {
        0x01: "SUCCESS",
        0x02: "INVALID_STATE",
        0x03: "NOT_SUPPORTED",
        0x04: "DATA_SIZE_EXCEEDS_LIMIT",
        0x05: "CRC_ERROR",
        0x06: "OPERATION_FAILED",
    }

    @staticmethod
    def lookup(code: int) -> str:
        return DfuResult._NAMES.get(code, f"UNKNOWN(0x{code:02x})")


# Packet Receipt Notification interval (number of packets between notifications).
# Must be <= 8 for the Adafruit bootloader (limited notification buffer).
# Set to 0 to disable PRN (simpler but no flow control).
PRN_INTERVAL = 8


def bootloader_address(app_address: str) -> str:
    """Compute the bootloader's BLE address from the app address.

    The Adafruit nRF52 bootloader advertises with the app address + 1
    (last octet incremented). This is a SoftDevice behavior for random
    static addresses when the bootloader re-initializes the SD.
    """
    parts = app_address.split(':')
    last = int(parts[-1], 16)
    parts[-1] = f"{(last + 1) & 0xFF:02X}"
    return ':'.join(parts)


class BleDfu:
    """BLE DFU client for Adafruit nRF52 bootloader (Legacy DFU, SDK v11)."""

    def __init__(self, address: str):
        self.address = address
        self.bootloader_addr = bootloader_address(address)
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
            # Read DFU Revision for informational purposes
            try:
                rev_data = await self._client.read_gatt_char(DFU_REVISION_UUID)
                rev = int.from_bytes(rev_data[:2], 'little') if len(rev_data) >= 2 else 0
                log.info("DFU Revision: 0x%04x (bootloader version, Legacy DFU protocol)",
                         rev)
            except Exception as e:
                log.warning("Could not read DFU Revision: %s", e)

            # Phase 3: START_DFU
            log.info("Phase 3: START_DFU (application image, %d bytes)...",
                     len(firmware_bin))
            await self._start_dfu(len(firmware_bin))

            # Phase 4: INIT_DFU (send init packet)
            log.info("Phase 4: INIT_DFU (%d bytes)...", len(init_packet))
            await self._init_dfu(init_packet)

            # Phase 5: Set PRN interval
            if PRN_INTERVAL > 0:
                log.info("Phase 5: Set PRN interval to %d...", PRN_INTERVAL)
                await self._set_prn(PRN_INTERVAL)

            # Phase 6: RECEIVE_FIRMWARE_IMAGE (send firmware data)
            log.info("Phase 6: RECEIVE_FIRMWARE_IMAGE (%d bytes)...",
                     len(firmware_bin))
            await self._send_firmware(firmware_bin)

            # Phase 7: VALIDATE
            log.info("Phase 7: VALIDATE firmware...")
            await self._validate()

            # Phase 8: ACTIVATE_AND_RESET
            log.info("Phase 8: ACTIVATE_AND_RESET...")
            await self._activate_and_reset()

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

    def _clear_bluez_state(self):
        """Thoroughly clear ALL BlueZ state for this device.

        CRITICAL: The bootloader has a completely different GATT table than
        the app firmware. If BlueZ uses cached app-mode GATT handles when
        talking to the bootloader, writes go to the wrong characteristics
        and the SoftDevice returns ATT error 0x0E (Unlikely Error).

        This clears:
        1. BlueZ device entry (via bluetoothctl remove)
        2. GATT cache files (/var/lib/bluetooth/*/cache/<addr>)
        3. Device info files (/var/lib/bluetooth/*/<addr>)
        4. Restarts BlueZ to ensure clean state
        """
        import subprocess
        addr_underscore = self.address.replace(':', '_')

        # Remove device via bluetoothctl (clears bonding, pairing, etc.)
        subprocess.run(
            f"echo 'remove {self.address}' | bluetoothctl",
            shell=True, capture_output=True, timeout=5
        )
        # Remove GATT cache
        subprocess.run(
            f"sudo rm -rf /var/lib/bluetooth/*/cache/{addr_underscore}",
            shell=True, capture_output=True, timeout=5
        )
        # Also try without sudo
        subprocess.run(
            f"rm -rf /var/lib/bluetooth/*/cache/{addr_underscore}",
            shell=True, capture_output=True, timeout=5
        )
        # Remove device info directory (contains stored GATT handles)
        subprocess.run(
            f"sudo rm -rf /var/lib/bluetooth/*/{addr_underscore}",
            shell=True, capture_output=True, timeout=5
        )
        # Restart bluetooth to pick up cache removal
        subprocess.run(
            ["sudo", "systemctl", "restart", "bluetooth"],
            capture_output=True, timeout=10
        )
        log.info("Cleared BlueZ state for %s", self.address)

    async def _enter_dfu_mode(self):
        """Connect to app-mode device and trigger reboot into DFU bootloader."""
        # Clear BlueZ state BEFORE connecting to app mode.
        # This ensures fresh GATT discovery for the app connection too.
        self._clear_bluez_state()
        await asyncio.sleep(3)  # Let BlueZ restart

        self._client = BleakClient(self.address, timeout=10.0)
        await self._client.connect()
        log.info("Connected (app mode), MTU=%d", self._client.mtu_size)

        # Must subscribe to DFU Control notifications BEFORE writing START_DFU.
        # The BLEDfu authorization callback rejects writes if notifications aren't enabled.
        # Use StartNotify (not AcquireNotify) — bleak 3.x defaults to
        # AcquireNotify which uses FD-based notification routing and can lose
        # notifications from some peripherals. StartNotify uses D-Bus signals.
        await self._client.start_notify(
            DFU_CONTROL_UUID, self._on_notification,
            bluez={"use_start_notify": True},
        )
        await asyncio.sleep(0.5)  # Let subscription propagate

        # Write 0x01 to DFU Control to trigger bootloader entry
        # (In app mode, this is the BLEDfu service's control characteristic,
        #  NOT the same as START_DFU opcode in bootloader mode)
        log.info("Triggering bootloader entry...")
        try:
            await self._client.write_gatt_char(
                DFU_CONTROL_UUID, bytes([0x01]), response=True
            )
        except Exception as e:
            # Device may disconnect immediately after processing.
            # GATT error 0x0E here is OK -- the device still reboots.
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
        # CRITICAL: Clear BlueZ state again before connecting to bootloader.
        # The bootloader has a completely different GATT table (different
        # handles, no NUS service, different DFU revision value). If BlueZ
        # reuses app-mode cached handles, all writes will fail with 0x0E.
        self._clear_bluez_state()
        await asyncio.sleep(4)  # Let BlueZ restart + bootloader start advertising

        # The bootloader advertises with address+1 (SoftDevice re-init behavior)
        log.info("Bootloader address: %s (app was %s)",
                 self.bootloader_addr, self.address)
        for attempt in range(10):
            try:
                # Scan first so BlueZ discovers the bootloader
                dev = await BleakScanner.find_device_by_address(
                    self.bootloader_addr, timeout=5.0
                )
                if not dev:
                    log.debug("Bootloader not found in scan, attempt %d",
                              attempt + 1)
                    await asyncio.sleep(2)
                    continue
                log.info("Found bootloader: %s", dev.name)
                self._client = BleakClient(dev, timeout=5.0)
                await self._client.connect()
                self._mtu = min(self._client.mtu_size - 3, 20)
                log.info("Connected to bootloader, MTU=%d (payload=%d)",
                         self._client.mtu_size, self._mtu)

                # Verify we're actually talking to the bootloader, not app
                # Bootloader DFU revision = 0x0008, app = 0x0001
                try:
                    rev_data = await self._client.read_gatt_char(
                        DFU_REVISION_UUID
                    )
                    rev = int.from_bytes(rev_data[:2], 'little')
                    if rev == 0x0001:
                        log.warning(
                            "DFU revision 0x0001 = app mode! "
                            "Stale BlueZ cache. Retrying..."
                        )
                        await self._client.disconnect()
                        self._clear_bluez_state()
                        await asyncio.sleep(4)
                        continue
                    log.info("DFU Revision: 0x%04x (bootloader)", rev)
                except Exception as e:
                    log.debug("Could not read DFU revision: %s", e)

                # Wait for SoftDevice sys_attr processing before enabling
                # notifications. The SoftDevice needs time to process the
                # BLE_GATTS_EVT_SYS_ATTR_MISSING event and set up internal
                # GATT state. Without this, CCCD writes may not take effect.
                await asyncio.sleep(2)

                # Enable DFU Control notifications via StartNotify (not
                # AcquireNotify). AcquireNotify uses FD-based routing which
                # loses notifications from the bootloader's DFU service.
                await self._client.start_notify(
                    DFU_CONTROL_UUID, self._on_notification,
                    bluez={"use_start_notify": True},
                )
                await asyncio.sleep(1)  # Let subscription propagate

                # Verify no NUS service (bootloader doesn't have it)
                has_nus = any(
                    "6e400001" in str(svc.uuid).lower()
                    for svc in self._client.services
                )
                if has_nus:
                    log.warning("NUS service found - still in app mode!")
                    await self._client.disconnect()
                    self._clear_bluez_state()
                    await asyncio.sleep(4)
                    continue

                # Log discovered services for debugging
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

    # ---- Legacy DFU transfer (SDK v11) ----

    async def _start_dfu(self, firmware_size: int):
        """Send START_DFU command with image type and size.

        1. Write START_DFU (0x01) + image type to Control Point
        2. Write 12-byte image size to Packet characteristic
        3. Wait for response notification
        """
        # Write START_DFU with APPLICATION image type.
        # CRITICAL: Must use write-without-response. The Adafruit bootloader's
        # DFU Control characteristic supports both write and write-without-response,
        # but only processes DFU commands via the write-without-response path.
        # Write-with-response succeeds at the GATT level but doesn't trigger
        # DFU command processing (no notification sent).
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.START_DFU, DfuImageType.APPLICATION]),
            response=False,
        )

        # Write image sizes: [softdevice_size, bootloader_size, app_size] as u32 LE
        # For application-only DFU, softdevice and bootloader sizes are 0
        size_data = struct.pack('<III', 0, 0, firmware_size)
        await self._client.write_gatt_char(
            DFU_PACKET_UUID, size_data, response=False
        )

        # Wait for START_DFU response
        await self._wait_for_response(DfuOpcode.START_DFU)
        log.info("START_DFU acknowledged")

    async def _init_dfu(self, init_packet: bytes):
        """Send init packet (.dat file from DFU zip).

        1. Write INIT_DFU start (0x02, 0x00) to Control Point
        2. Write init packet data to Packet characteristic
        3. Write INIT_DFU complete (0x02, 0x01) to Control Point
        4. Wait for response notification
        """
        # INIT_DFU start
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.INIT_DFU, 0x00]),
            response=False,
        )

        # Write init packet data
        await self._client.write_gatt_char(
            DFU_PACKET_UUID, init_packet, response=False
        )

        # INIT_DFU complete
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.INIT_DFU, 0x01]),
            response=False,
        )

        # Wait for INIT_DFU response
        await self._wait_for_response(DfuOpcode.INIT_DFU)
        log.info("INIT_DFU acknowledged")

    async def _set_prn(self, interval: int):
        """Set Packet Receipt Notification interval.

        When set to N > 0, the bootloader sends a notification every N data
        packets with the total bytes received so far. Used for flow control.
        """
        cmd = bytes([DfuOpcode.PKT_RCPT_NOTIF_REQ]) + \
            struct.pack('<H', interval)
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID, cmd, response=False
        )
        # PRN_REQ doesn't generate a response notification per the SDK v11 spec

    async def _send_firmware(self, firmware: bytes):
        """Send firmware data via RECEIVE_FIRMWARE_IMAGE.

        1. Write RECEIVE_FIRMWARE_IMAGE (0x03) to Control Point
        2. Stream firmware in MTU-sized packets to Packet characteristic
        3. Handle PRN notifications every PRN_INTERVAL packets
        4. Wait for final completion notification
        """
        # Signal start of firmware receive
        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.RECEIVE_FIRMWARE]),
            response=False,
        )

        total = len(firmware)
        sent = 0
        pkt_count = 0

        while sent < total:
            chunk = firmware[sent:sent + self._mtu]
            await self._client.write_gatt_char(
                DFU_PACKET_UUID, chunk, response=False
            )
            sent += len(chunk)
            pkt_count += 1

            # Handle PRN: wait for notification every PRN_INTERVAL packets
            if PRN_INTERVAL > 0 and pkt_count % PRN_INTERVAL == 0:
                await self._wait_for_prn(sent)

            # Progress reporting every ~10%
            pct = (sent * 100) // total
            prev_pct = ((sent - len(chunk)) * 100) // total
            if pct // 10 > prev_pct // 10:
                log.info("Progress: %d%% (%d/%d bytes)", pct, sent, total)

        # Wait for final completion notification (RECEIVE_FIRMWARE response)
        log.info("Firmware data sent, waiting for completion notification...")
        await self._wait_for_response(DfuOpcode.RECEIVE_FIRMWARE, timeout=60.0)
        log.info("RECEIVE_FIRMWARE complete (%d bytes)", total)

    async def _validate(self):
        """Send VALIDATE command and wait for response."""
        self._response_event.clear()
        self._response_data = bytearray()

        await self._client.write_gatt_char(
            DFU_CONTROL_UUID,
            bytes([DfuOpcode.VALIDATE]),
            response=False,
        )

        await self._wait_for_response(DfuOpcode.VALIDATE)
        log.info("Firmware validated successfully")

    async def _activate_and_reset(self):
        """Send ACTIVATE_AND_RESET to apply firmware and reboot."""
        try:
            await self._client.write_gatt_char(
                DFU_CONTROL_UUID,
                bytes([DfuOpcode.ACTIVATE_AND_RESET]),
                response=False,
            )
        except Exception as e:
            # Device may disconnect immediately after reset
            log.debug("Activate result (may disconnect): %s", e)

    # ---- Notification handling ----

    async def _wait_for_response(self, expected_procedure: int,
                                 timeout: float = 30.0):
        """Wait for a response notification (opcode 0x10).

        Response format: [0x10, procedure_opcode, result_code]
        """
        self._response_event.clear()
        self._response_data = bytearray()

        try:
            await asyncio.wait_for(self._response_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(
                f"Timeout ({timeout}s) waiting for response to "
                f"opcode 0x{expected_procedure:02x}"
            )

        resp = self._response_data
        if len(resp) < 3:
            raise RuntimeError(f"Short response ({len(resp)} bytes): {resp.hex()}")

        if resp[0] != DfuOpcode.RESPONSE:
            # Could be a PRN notification (0x11) arriving out of order
            if resp[0] == DfuOpcode.PKT_RCPT_NOTIF:
                log.debug("Got PRN while waiting for response, retrying...")
                return await self._wait_for_response(expected_procedure, timeout)
            raise RuntimeError(
                f"Unexpected notification type: 0x{resp[0]:02x} "
                f"(expected RESPONSE 0x{DfuOpcode.RESPONSE:02x}), "
                f"data: {resp.hex()}"
            )

        if resp[1] != expected_procedure:
            raise RuntimeError(
                f"Response procedure mismatch: expected 0x{expected_procedure:02x}, "
                f"got 0x{resp[1]:02x}"
            )

        if resp[2] != DfuResult.SUCCESS:
            raise RuntimeError(
                f"DFU error: {DfuResult.lookup(resp[2])} "
                f"for procedure 0x{expected_procedure:02x}"
            )

    async def _wait_for_prn(self, expected_bytes: int, timeout: float = 10.0):
        """Wait for a Packet Receipt Notification (opcode 0x11).

        PRN format: [0x11, bytes_received (u32 LE)]
        """
        self._response_event.clear()
        self._response_data = bytearray()

        try:
            await asyncio.wait_for(self._response_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(
                f"Timeout ({timeout}s) waiting for PRN "
                f"(expected {expected_bytes} bytes received)"
            )

        resp = self._response_data
        if len(resp) < 5:
            # Might be a response notification instead of PRN
            if len(resp) >= 3 and resp[0] == DfuOpcode.RESPONSE:
                if resp[2] != DfuResult.SUCCESS:
                    raise RuntimeError(
                        f"DFU error during transfer: "
                        f"{DfuResult.lookup(resp[2])} "
                        f"for procedure 0x{resp[1]:02x}"
                    )
                log.debug("Got response notification during PRN wait: %s",
                          resp.hex())
                return
            raise RuntimeError(f"Short PRN ({len(resp)} bytes): {resp.hex()}")

        if resp[0] != DfuOpcode.PKT_RCPT_NOTIF:
            log.debug("Expected PRN (0x11), got 0x%02x: %s", resp[0], resp.hex())
            return

        received = struct.unpack('<I', resp[1:5])[0]
        log.debug("PRN: %d bytes received (expected %d)", received, expected_bytes)

    def _on_notification(self, sender, data: bytearray):
        """Handle DFU Control Point notification."""
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
        description="BLE DFU for nRF52840 devices (Legacy DFU, SDK v11)"
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
    parser.add_argument("--prn", type=int, default=None,
                        help="Packet receipt notification interval (default: 8, 0=disable)")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-5s %(message)s",
        datefmt="%H:%M:%S",
    )

    # Allow overriding PRN interval
    global PRN_INTERVAL
    if args.prn is not None:
        PRN_INTERVAL = args.prn

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
