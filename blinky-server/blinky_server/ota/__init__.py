"""OTA firmware update orchestration.

Provides upload_with_ble_fallback() for seamless UF2 → BLE DFU fallback.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
from typing import Any

from ..transport.base import Transport

log = logging.getLogger(__name__)


async def upload_with_ble_fallback(
    serial_port: str,
    firmware_path: str,
    transport: Transport,
    ble_address: str | None = None,
) -> dict[str, Any]:
    """Upload firmware via UF2, falling back to BLE DFU if UF2 fails.

    Args:
        serial_port: Serial port path (e.g., /dev/ttyACM0)
        firmware_path: Path to .hex firmware file
        transport: SerialTransport instance (disconnected by upload_uf2)
        ble_address: Device's BLE address for DFU fallback (None = no fallback)

    Returns:
        dict with status, message, elapsed_s, and optional fallback info
    """
    from .uf2_upload import upload_uf2

    result = await upload_uf2(
        serial_port=serial_port,
        firmware_path=firmware_path,
        transport=transport,
    )

    if result["status"] == "ok" or not ble_address:
        return result

    # UF2 failed — try BLE DFU as fallback
    log.warning(
        "UF2 upload failed on %s (%s), attempting BLE DFU fallback via %s",
        serial_port,
        result.get("message", ""),
        ble_address,
    )

    from ..transport.serial_transport import SerialTransport
    from .ble_dfu import upload_ble_dfu
    from .compile import ensure_dfu_zip

    # Generate DFU zip from hex
    try:
        dfu_zip = await asyncio.to_thread(ensure_dfu_zip, firmware_path)
    except ValueError as e:
        result["message"] += f" (BLE DFU fallback also failed: {e})"
        return result

    # Open a fresh serial connection to send bootloader command.
    # The old transport was disconnected by upload_uf2. The serial port
    # may be in an unknown state, so we wrap this in try/except.
    async def enter_bootloader_via_serial(cmd: str) -> None:
        tmp_transport = SerialTransport(serial_port)
        try:
            await asyncio.wait_for(tmp_transport.connect(), timeout=5.0)
            await tmp_transport.write_line(cmd)
            await asyncio.sleep(0.5)
        finally:
            with contextlib.suppress(Exception):
                await tmp_transport.disconnect()

    try:
        dfu_result = await upload_ble_dfu(
            app_ble_address=ble_address,
            dfu_zip_path=dfu_zip,
            enter_bootloader_via_serial=enter_bootloader_via_serial,
        )
    except Exception as e:
        result["message"] += f" (BLE DFU fallback error: {e})"
        return result

    if dfu_result["status"] == "ok":
        dfu_result["message"] = "BLE DFU fallback succeeded (UF2 failed)"
        dfu_result["fallback"] = True
        return dfu_result

    result["message"] += f" (BLE DFU fallback also failed: {dfu_result.get('message', '')})"
    return result
