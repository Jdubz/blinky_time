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
    fleet.pause_discovery()  # Prevent BleakScanner conflict during fleet DFU
    try:
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
                    from ..ota.compile import ensure_dfu_zip

                    # Auto-convert .hex → .dfu.zip if needed
                    try:
                        dfu_zip = await asyncio.to_thread(ensure_dfu_zip, str(firmware))
                    except ValueError as e:
                        result = {"status": "error", "message": str(e)}
                        results[device.id[:12]] = result
                        continue

                    ble_transport = device.transport

                    async def enter_bootloader_via_ble(cmd: str, _t=ble_transport):
                        """Send bootloader command over BLE NUS, then disconnect."""
                        await _t.write_line(cmd)
                        await asyncio.sleep(0.5)
                        await _t.disconnect()

                    result = await upload_ble_dfu(
                        app_ble_address=device.port,
                        dfu_zip_path=dfu_zip,
                        enter_bootloader_via_ble=enter_bootloader_via_ble,
                    )
                else:
                    result = {"status": "skip", "message": f"Unsupported transport: {transport_type}"}

                results[device.id[:12]] = result
            except Exception as e:
                results[device.id[:12]] = {"status": "error", "message": str(e)}
            finally:
                # Mark device as disconnected for auto-reconnect after DFU
                device.state = DeviceState.DISCONNECTED
                fleet.resume_reconnect(device.id)

            # Brief pause between devices
            await asyncio.sleep(3)
    finally:
        fleet.resume_discovery()

    ok_count = sum(1 for r in results.values() if r.get("status") == "ok")
    total = len(results)
    return {
        "status": "ok" if ok_count == total else "partial",
        "message": f"{ok_count}/{total} devices updated",
        "per_device": results,
    }


@router.post("/fleet/deploy")
async def fleet_deploy(platform: str = "nrf52840") -> dict:
    """One-shot compile + flash all connected devices.

    Compiles firmware, generates DFU zip, then flashes every connected
    nRF52840 device sequentially. Returns compilation info + per-device
    results. This is the primary fleet management endpoint.
    """
    from ..ota.compile import compile_firmware as _compile, generate_dfu_package

    # 1. Compile firmware
    log.info("Fleet deploy: compiling %s firmware...", platform)
    compile_result = await asyncio.to_thread(_compile, platform)
    if compile_result["status"] != "ok":
        raise HTTPException(500, f"Compilation failed: {compile_result['message']}")
    hex_path = compile_result["hex_path"]

    # 2. Generate DFU zip
    log.info("Fleet deploy: generating DFU package...")
    dfu_result = await asyncio.to_thread(generate_dfu_package, hex_path)
    if dfu_result["status"] != "ok":
        raise HTTPException(500, f"DFU package failed: {dfu_result['message']}")
    zip_path = dfu_result["zip_path"]

    # 3. Flash all connected devices (reuse fleet_ota logic)
    from .models import OtaRequest
    body = OtaRequest(firmware_path=zip_path)
    try:
        ota_result = await fleet_ota(body)
    except HTTPException as e:
        raise HTTPException(e.status_code, f"Fleet flash failed: {e.detail}")

    return {
        "status": ota_result["status"],
        "message": ota_result["message"],
        "hex_path": hex_path,
        "zip_path": zip_path,
        "per_device": ota_result.get("per_device", {}),
    }
