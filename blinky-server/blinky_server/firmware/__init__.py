"""Firmware upload orchestration.

Single dispatch: upload_firmware() picks the best method per device.

Upload method is determined by transport type — NO FALLBACK:
- Serial (USB) → UF2 mass storage bootloader
- BLE → BLE DFU (Legacy Nordic protocol)
- DFU recovery → BLE DFU direct (device already in bootloader)

If the chosen method fails, the error is returned immediately.
The operator must investigate and fix the issue.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from ..transport.base import Transport

log = logging.getLogger(__name__)


async def upload_via_uf2(
    serial_port: str,
    firmware_path: str,
    transport: Transport,
) -> dict[str, Any]:
    """Upload firmware via UF2 mass storage bootloader.

    For serial (USB) connected devices only. No fallback to BLE DFU —
    if UF2 fails, the error is returned and must be investigated.
    """
    from .uf2_upload import upload_uf2

    return await upload_uf2(
        serial_port=serial_port,
        firmware_path=firmware_path,
        transport=transport,
    )


async def upload_firmware(
    device: Any,
    firmware_path: str,
) -> dict[str, Any]:
    """Upload firmware using the method determined by device transport.

    NO FALLBACK between methods. If the chosen method fails, the error
    is returned immediately for investigation.

    Dispatch:
      1. DFU recovery (device stuck in bootloader) → BLE DFU direct
      2. Serial (USB) transport → UF2 mass storage
      3. BLE transport → BLE DFU with bootloader entry over BLE
    """
    from ..device.device import DeviceState

    # DFU recovery: device is stuck in bootloader, push firmware directly
    if device.state == DeviceState.DFU_RECOVERY:
        from .ble_dfu import upload_ble_dfu
        from .compile import ensure_dfu_zip

        if not device.ble_address:
            return {"status": "error", "message": "DFU recovery but BLE address unknown"}
        dfu_zip = await asyncio.to_thread(ensure_dfu_zip, firmware_path)
        return await upload_ble_dfu(
            app_ble_address=device.ble_address,
            dfu_zip_path=dfu_zip,
        )

    transport_type = device.transport.transport_type

    # Serial (USB) devices: UF2 only, no fallback
    if transport_type == "serial":
        return await upload_via_uf2(
            serial_port=device.port,
            firmware_path=firmware_path,
            transport=device.transport,
        )

    # BLE-only devices: BLE DFU with bootloader entry over BLE
    if transport_type == "ble":
        from .ble_dfu import upload_ble_dfu
        from .compile import ensure_dfu_zip

        dfu_zip = await asyncio.to_thread(ensure_dfu_zip, firmware_path)
        ble_transport = device.transport

        async def enter_bootloader_via_ble(cmd: str) -> None:
            await ble_transport.write_line(cmd)
            await asyncio.sleep(0.5)
            await ble_transport.disconnect()

        return await upload_ble_dfu(
            app_ble_address=device.port,
            dfu_zip_path=dfu_zip,
            enter_bootloader_via_ble=enter_bootloader_via_ble,
        )

    return {"status": "error", "message": f"No upload method for transport: {transport_type}"}
