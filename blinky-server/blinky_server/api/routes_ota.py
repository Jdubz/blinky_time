"""OTA firmware update routes.

Provides endpoints for compiling firmware and fleet-wide updates.
Device-specific OTA is in routes_devices.py (POST /devices/{id}/ota).
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from fastapi import APIRouter, HTTPException

from ..device.device import DeviceState
from .deps import get_fleet
from .models import OtaRequest

log = logging.getLogger(__name__)

router = APIRouter(tags=["ota"])


@router.post("/ota/compile")
async def compile_firmware(platform: str = "nrf52840") -> dict[str, Any]:
    """Compile firmware for a platform. Returns path to hex file."""
    from ..ota.compile import compile_firmware as _compile

    result = await asyncio.to_thread(_compile, platform)
    if result["status"] != "ok":
        raise HTTPException(500, result["message"])
    return result


@router.post("/ota/compile-dfu")
async def compile_dfu_package(platform: str = "nrf52840") -> dict[str, Any]:
    """Compile firmware and generate DFU zip package."""
    from ..ota.compile import compile_firmware as _compile
    from ..ota.compile import generate_dfu_package

    compile_result = await asyncio.to_thread(_compile, platform)
    if compile_result["status"] != "ok":
        raise HTTPException(500, compile_result["message"])

    dfu_result = await asyncio.to_thread(generate_dfu_package, compile_result["hex_path"])
    if dfu_result["status"] != "ok":
        raise HTTPException(500, dfu_result["message"])

    return {
        "status": "ok",
        "hex_path": compile_result["hex_path"],
        "zip_path": dfu_result["zip_path"],
        "message": "Compiled and packaged",
    }


@router.post("/fleet/ota")
async def fleet_ota(body: OtaRequest) -> dict[str, Any]:
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
    # Include CONNECTED devices and DFU_RECOVERY devices (stuck in bootloader)
    flashable_states = (DeviceState.CONNECTED, DeviceState.DFU_RECOVERY)
    devices = [
        d
        for d in fleet.get_all_devices()
        if d.platform == "nrf52840" and d.state in flashable_states
    ]

    if not devices:
        raise HTTPException(404, "No flashable nRF52840 devices")

    # Set recovery firmware early so auto-recovery works if server crashes mid-DFU
    fleet.set_recovery_firmware(str(firmware))

    results = {}
    fleet.pause_discovery()  # Prevent BleakScanner conflict during fleet DFU
    try:
        for device in devices:
            log.info(
                "Fleet OTA: flashing %s (%s, state=%s)...",
                device.id[:12],
                device.port,
                device.state.value,
            )

            # BLE DFU takes ~5.5min (330s) — use longer hold for DFU recovery devices
            hold_secs = 360 if device.state == DeviceState.DFU_RECOVERY else 120
            fleet.hold_reconnect(device.id, hold_secs)

            try:
                # Device in DFU bootloader (SafeBoot crash recovery) —
                # push firmware directly, no bootloader entry needed.
                if device.state == DeviceState.DFU_RECOVERY:
                    from ..ota.ble_dfu import upload_ble_dfu
                    from ..ota.compile import ensure_dfu_zip

                    app_ble_addr = device.ble_address
                    if not app_ble_addr:
                        result = {
                            "status": "error",
                            "message": "DFU recovery device has no BLE app address",
                        }
                        results[device.id[:12]] = result
                        continue

                    try:
                        dfu_zip = await asyncio.to_thread(ensure_dfu_zip, str(firmware))
                    except ValueError as e:
                        result = {"status": "error", "message": str(e)}
                        results[device.id[:12]] = result
                        continue

                    result = await upload_ble_dfu(
                        app_ble_address=app_ble_addr,
                        dfu_zip_path=dfu_zip,
                    )

                elif device.transport.transport_type == "serial":
                    from ..ota import upload_with_ble_fallback

                    result = await upload_with_ble_fallback(
                        serial_port=device.port,
                        firmware_path=str(firmware),
                        transport=device.transport,
                        ble_address=device.ble_address,
                    )
                elif device.transport.transport_type == "ble":
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

                    async def enter_bootloader_via_ble(cmd: str, _t: Any = ble_transport) -> None:
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
                    result = {
                        "status": "skip",
                        "message": f"Unsupported transport: {device.transport.transport_type}",
                    }

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
async def fleet_deploy(platform: str = "nrf52840") -> dict[str, Any]:
    """One-shot compile + flash all connected devices.

    Compiles firmware, generates DFU zip, then flashes every connected
    nRF52840 device sequentially. Returns compilation info + per-device
    results. This is the primary fleet management endpoint.
    """
    from ..ota.compile import compile_firmware as _compile
    from ..ota.compile import generate_dfu_package

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
        raise HTTPException(e.status_code, f"Fleet flash failed: {e.detail}") from e

    return {
        "status": ota_result["status"],
        "message": ota_result["message"],
        "hex_path": hex_path,
        "zip_path": zip_path,
        "per_device": ota_result.get("per_device", {}),
    }
