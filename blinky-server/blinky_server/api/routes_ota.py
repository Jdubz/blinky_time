"""OTA firmware update routes.

Provides endpoints for compiling firmware and fleet-wide updates.
Device-specific OTA is in routes_devices.py (POST /devices/{id}/ota).
"""
from __future__ import annotations

import asyncio
import logging

from fastapi import APIRouter, HTTPException

from ..device.device import DeviceState
from .deps import get_fleet
from .models import OtaRequest, OtaResponse, StatusResponse

log = logging.getLogger(__name__)

router = APIRouter(tags=["ota"])


@router.post("/ota/compile")
async def compile_firmware(platform: str = "nrf52840") -> dict:
    """Compile firmware for a platform. Returns path to hex file."""
    from ..ota.compile import compile_firmware as _compile
    result = await asyncio.to_thread(_compile, platform)
    if result["status"] != "ok":
        raise HTTPException(500, result["message"])
    return result


@router.post("/ota/compile-dfu")
async def compile_dfu_package(platform: str = "nrf52840") -> dict:
    """Compile firmware and generate DFU zip package."""
    from ..ota.compile import compile_firmware as _compile, generate_dfu_package

    compile_result = await asyncio.to_thread(_compile, platform)
    if compile_result["status"] != "ok":
        raise HTTPException(500, compile_result["message"])

    dfu_result = await asyncio.to_thread(
        generate_dfu_package, compile_result["hex_path"]
    )
    if dfu_result["status"] != "ok":
        raise HTTPException(500, dfu_result["message"])

    return {
        "status": "ok",
        "hex_path": compile_result["hex_path"],
        "zip_path": dfu_result["zip_path"],
        "message": "Compiled and packaged",
    }


@router.post("/fleet/ota")
async def fleet_ota(body: OtaRequest) -> dict:
    """Upload firmware to ALL connected nRF52840 devices sequentially.

    Flashes each device one at a time to avoid USB contention.
    Returns per-device results.
    """
    from pathlib import Path

    # Validate firmware path — restrict to allowed directories
    firmware = Path(body.firmware_path).resolve()
    allowed_dirs = [Path("/tmp"), Path.home()]
    if not any(firmware.is_relative_to(d) for d in allowed_dirs):
        raise HTTPException(400, f"Firmware path not in allowed directory: {firmware}")
    if not firmware.is_file():
        raise HTTPException(400, f"Firmware file not found: {firmware}")

    fleet = get_fleet()
    devices = [d for d in fleet.get_all_devices()
               if d.platform == "nrf52840" and d.state == DeviceState.CONNECTED]

    if not devices:
        raise HTTPException(404, "No connected nRF52840 devices")

    results = {}
    for device in devices:
        log.info("Fleet OTA: flashing %s (%s via %s)...",
                 device.id[:12], device.port, device.transport.transport_type)

        transport_type = device.transport.transport_type
        fleet.hold_reconnect(device.id, 120)

        try:
            if transport_type == "serial":
                from ..ota.uf2_upload import upload_uf2
                result = await upload_uf2(
                    serial_port=device.port,
                    firmware_path=str(firmware),
                    transport=device.transport,
                    protocol=device.protocol,
                )
            elif transport_type == "ble":
                from ..ota.ble_dfu import upload_ble_dfu
                result = await upload_ble_dfu(
                    app_ble_address=device.port,
                    dfu_zip_path=str(firmware),
                )
            else:
                result = {"status": "skip", "message": f"Unsupported transport: {transport_type}"}

            results[device.id[:12]] = result
        except Exception as e:
            results[device.id[:12]] = {"status": "error", "message": str(e)}
        finally:
            fleet.resume_reconnect(device.id)

        # Brief pause between devices for USB stability
        await asyncio.sleep(3)

    ok_count = sum(1 for r in results.values() if r.get("status") == "ok")
    total = len(results)
    return {
        "status": "ok" if ok_count == total else "partial",
        "message": f"{ok_count}/{total} devices updated",
        "per_device": results,
    }
