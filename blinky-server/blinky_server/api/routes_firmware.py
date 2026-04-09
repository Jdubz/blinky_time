"""Firmware compilation and fleet-wide flash routes.

Device-specific flash is in routes_devices.py (POST /devices/{id}/flash).
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from fastapi import APIRouter, HTTPException

from ..device.device import DeviceState
from .deps import get_fleet
from .models import FlashRequest

log = logging.getLogger(__name__)

router = APIRouter(tags=["firmware"])


@router.post("/firmware/compile")
async def compile_firmware(platform: str = "nrf52840") -> dict[str, Any]:
    """Compile firmware for a platform. Returns path to hex file."""
    from ..firmware.compile import compile_firmware as _compile

    result = await asyncio.to_thread(_compile, platform)
    if result["status"] != "ok":
        raise HTTPException(500, result["message"])
    return result


@router.post("/firmware/compile-dfu")
async def compile_dfu_package(platform: str = "nrf52840") -> dict[str, Any]:
    """Compile firmware and generate DFU zip package."""
    from ..firmware.compile import compile_firmware as _compile
    from ..firmware.compile import generate_dfu_package

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


@router.post("/fleet/flash")
async def fleet_flash(body: FlashRequest) -> dict[str, Any]:
    """Flash firmware to ALL connected nRF52840 devices sequentially.

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

    # Hold reconnect on ALL serial devices for the entire fleet flash.
    # UF2 bootloader entry causes USB bus resets that disrupt sibling
    # devices on the same hub — prevent reconnect attempts during instability.
    all_serial_ids = [
        d.id for d in fleet.get_all_devices() if d.transport.transport_type == "serial"
    ]
    for sid in all_serial_ids:
        fleet.hold_reconnect(sid, 600)

    try:
        for device in devices:
            log.info(
                "Fleet flash: flashing %s (%s, state=%s)...",
                device.id[:12],
                device.port,
                device.state.value,
            )

            hold_secs = 360 if device.state == DeviceState.DFU_RECOVERY else 120
            fleet.hold_reconnect(device.id, hold_secs)

            try:
                # Use central dispatch — no fallback, no duplicated logic
                from ..firmware import upload_firmware

                result = await upload_firmware(device, str(firmware))
                results[device.id[:12]] = result

                # STOP ON FIRST FAILURE — do not flash remaining devices
                if result.get("status") != "ok":
                    log.error(
                        "Fleet flash STOPPED: %s failed (%s). Remaining devices not flashed.",
                        device.id[:12],
                        result.get("message", "unknown"),
                    )
                    break
            except Exception as e:
                results[device.id[:12]] = {"status": "error", "message": str(e)}
                log.error("Fleet flash STOPPED: %s exception: %s", device.id[:12], e)
                break
            finally:
                device.state = DeviceState.DISCONNECTED
                # Don't resume_reconnect here — wait until ALL devices are
                # flashed and USB has stabilized (see below).

            await asyncio.sleep(3)
    finally:
        # Wait for USB CDC to stabilize after all flashes before reconnecting.
        # Without this delay, reconnection attempts fail with "Broken pipe"
        # because the kernel's CDC state is stale from device reboots.
        log.info("Fleet flash complete. Waiting 10s for USB stabilization...")
        await asyncio.sleep(10)

        # Resume reconnection for all serial devices (not just flashed ones —
        # siblings were held too to survive USB bus resets)
        for sid in all_serial_ids:
            fleet.resume_reconnect(sid)
        fleet.resume_discovery()

    ok_count = sum(1 for r in results.values() if r.get("status") == "ok")
    total_attempted = len(results)
    total_devices = len(devices)
    return {
        "status": "ok" if ok_count == total_devices else "error",
        "message": f"{ok_count}/{total_devices} devices updated"
        + (f" (stopped after {total_attempted})" if total_attempted < total_devices else ""),
        "per_device": results,
    }


@router.post("/fleet/deploy")
async def fleet_deploy(platform: str = "nrf52840") -> dict[str, Any]:
    """One-shot compile + flash all connected devices.

    Compiles firmware, generates DFU zip, then flashes every connected
    nRF52840 device sequentially. Returns compilation info + per-device
    results. This is the primary fleet management endpoint.
    """
    from ..firmware.compile import compile_firmware as _compile
    from ..firmware.compile import generate_dfu_package

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

    # 3. Flash all connected devices
    from .models import FlashRequest

    body = FlashRequest(firmware_path=zip_path)
    try:
        flash_result = await fleet_flash(body)
    except HTTPException as e:
        raise HTTPException(e.status_code, f"Fleet flash failed: {e.detail}") from e

    return {
        "status": flash_result["status"],
        "message": flash_result["message"],
        "hex_path": hex_path,
        "zip_path": zip_path,
        "per_device": flash_result.get("per_device", {}),
    }
