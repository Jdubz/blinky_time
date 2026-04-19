"""Firmware upload, flash, and fleet management routes.

Upload and flash are separate operations:
  POST /api/fleet/upload  — accept .hex file, store it, return path (instant)
  POST /api/fleet/flash   — flash stored firmware to devices (background job)
  GET  /api/fleet/jobs/{id} — poll flash job progress
  GET  /api/fleet/jobs      — list all flash jobs

Device-specific flash is in routes_devices.py (POST /devices/{id}/flash).
"""

from __future__ import annotations

import asyncio
import logging
import tempfile
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, UploadFile

from ..device.device import DeviceState
from .deps import get_fleet, require_api_key
from .models import FlashRequest

log = logging.getLogger(__name__)
router = APIRouter()


# ── Upload (instant) ─────────────────────────────────────────────────


@router.post("/fleet/upload", dependencies=[Depends(require_api_key)])
async def fleet_upload(firmware: UploadFile) -> dict[str, Any]:
    """Upload a firmware .hex file to the server. Does NOT flash.

    Returns the stored firmware path for use with POST /api/fleet/flash.
    """
    if not firmware.filename or not firmware.filename.endswith(".hex"):
        raise HTTPException(400, "Firmware must be a .hex file")

    content = await firmware.read()
    if len(content) < 1000:
        raise HTTPException(400, f"Firmware file too small ({len(content)} bytes)")
    if len(content) > 5_000_000:
        raise HTTPException(400, f"Firmware file too large ({len(content)} bytes)")

    safe_name = Path(firmware.filename).name
    hex_path = Path(tempfile.gettempdir()) / "blinky-upload" / safe_name
    hex_path.parent.mkdir(parents=True, exist_ok=True)
    hex_path.write_bytes(content)

    log.info("Fleet upload: stored %s (%d bytes) at %s", safe_name, len(content), hex_path)

    return {
        "status": "ok",
        "firmware_path": str(hex_path),
        "firmware_size": len(content),
        "message": f"Firmware stored ({len(content)} bytes). Use POST /api/fleet/flash to deploy.",
    }


# ── Flash (background job) ───────────────────────────────────────────


@router.post("/fleet/flash", dependencies=[Depends(require_api_key)])
async def fleet_flash(body: FlashRequest) -> dict[str, Any]:
    """Flash firmware to all connected nRF52840 devices.

    Returns immediately with a job_id. Poll GET /api/fleet/jobs/{job_id}
    for progress. Each device is flashed sequentially (UF2 bootloader
    entry causes USB bus resets that disconnect siblings).
    """
    firmware = Path(body.firmware_path).resolve()
    allowed_dirs = [Path("/tmp"), Path.home()]
    if not any(firmware.is_relative_to(d) for d in allowed_dirs):
        raise HTTPException(400, f"Firmware path not in allowed directory: {firmware}")
    if not firmware.is_file():
        raise HTTPException(400, f"Firmware file not found: {firmware}")

    fleet = get_fleet()
    flashable_states = (DeviceState.CONNECTED, DeviceState.DFU_RECOVERY)
    devices = [
        d
        for d in fleet.get_all_devices()
        if d.platform == "nrf52840" and d.state in flashable_states
    ]
    if not devices:
        raise HTTPException(404, "No flashable nRF52840 devices")

    fleet.set_recovery_firmware(str(firmware))
    device_ids = [d.id for d in devices]

    jm = _get_flash_job_manager()

    async def _run(job: Any) -> dict[str, Any]:
        return await _flash_fleet_background(fleet, device_ids, firmware, job)

    job = jm.submit("fleet-flash", _run)

    return {
        "job_id": job.id,
        "status": "submitted",
        "devices": len(devices),
        "message": f"Flashing {len(devices)} device(s) in background",
    }


# ── Job status ────────────────────────────────────────────────────────


@router.get("/fleet/jobs/{job_id}")
async def get_flash_job(job_id: str) -> dict[str, Any]:
    """Get the status of a fleet flash job."""
    jm = _get_flash_job_manager()
    job = jm.get(job_id)
    if not job:
        raise HTTPException(404, f"Job {job_id} not found")
    return dict(job.to_dict())


@router.get("/fleet/jobs")
async def list_flash_jobs() -> list[dict[str, Any]]:
    """List all fleet flash jobs."""
    jm = _get_flash_job_manager()
    return [j.to_dict() for j in jm.list_jobs()]


# ── Flash job manager singleton ───────────────────────────────────────

_flash_jm: Any = None


def _get_flash_job_manager() -> Any:
    global _flash_jm
    if _flash_jm is None:
        from ..testing.job_manager import JobManager

        _flash_jm = JobManager(jobs_dir=Path("/tmp/blinky-flash-jobs"))
    return _flash_jm


# ── Background flash implementation ──────────────────────────────────


async def _flash_fleet_background(
    fleet: Any,
    device_ids: list[str],
    firmware: Path,
    job: Any,
) -> dict[str, Any]:
    """Flash all devices sequentially. Runs as a background job with progress."""
    from ..firmware import upload_firmware

    results: dict[str, dict[str, Any]] = {}
    total = len(device_ids)

    fleet.pause_discovery()
    all_serial_ids = [
        d.id for d in fleet.get_all_devices() if d.transport.transport_type == "serial"
    ]
    for sid in all_serial_ids:
        fleet.hold_reconnect(sid, 600)

    try:
        for i, dev_id in enumerate(device_ids):
            device = fleet.get_device(dev_id)
            if not device:
                results[dev_id[:12]] = {"status": "error", "message": "Device not found"}
                continue

            dev_label = f"{device.id[:12]} ({device.device_name or 'unknown'})"
            job.progress = int((i / total) * 90)
            job.progress_message = f"Flashing {dev_label} ({i + 1}/{total})"
            log.info("Fleet flash: %s", job.progress_message)

            fleet.hold_reconnect(device.id, 120)

            try:
                result = await upload_firmware(device, str(firmware))
                results[device.id[:12]] = result
                if result.get("status") != "ok":
                    log.error(
                        "Fleet flash STOPPED: %s failed (%s)", dev_label, result.get("message")
                    )
                    break
            except Exception as e:
                results[device.id[:12]] = {"status": "error", "message": str(e)}
                log.error("Fleet flash STOPPED: %s exception: %s", dev_label, e)
                break
            finally:
                device.state = DeviceState.DISCONNECTED

            await asyncio.sleep(3)
    finally:
        job.progress_message = "Waiting for USB stabilization"
        log.info("Fleet flash: waiting 10s for USB stabilization...")
        await asyncio.sleep(10)
        for sid in all_serial_ids:
            fleet.resume_reconnect(sid)
        fleet.resume_discovery()

    # Verify firmware versions
    job.progress = 90
    job.progress_message = "Verifying firmware versions"
    await asyncio.sleep(10)

    verified: dict[str, dict[str, Any]] = {}
    for _ in range(6):
        all_done = True
        for d in fleet.get_all_devices():
            short_id = d.id[:12]
            if short_id not in results:
                continue
            if d.state == DeviceState.CONNECTED and d.version:
                verified[short_id] = {"version": d.version, "device_name": d.device_name}
            else:
                all_done = False
        if all_done or len(verified) == len(results):
            break
        await asyncio.sleep(5)

    ok_count = sum(1 for r in results.values() if r.get("status") == "ok")
    per_device = {}
    for short_id, flash_result in results.items():
        v = verified.get(short_id, {})
        per_device[short_id] = {
            "flash": flash_result.get("status", "error"),
            "version": v.get("version"),
            "device_name": v.get("device_name"),
        }

    job.progress = 100
    return {
        "status": "ok" if ok_count == total else "error",
        "message": f"{ok_count}/{total} devices flashed",
        "per_device": per_device,
    }


# ── Compile + flash (convenience) ────────────────────────────────────


@router.post("/fleet/deploy", dependencies=[Depends(require_api_key)])
async def fleet_deploy(platform: str = "nrf52840") -> dict[str, Any]:
    """Compile firmware and flash all connected devices.

    Synchronous compile, then submits flash as background job.
    Returns compilation info + job_id for flash progress polling.
    """
    from ..firmware.compile import compile_firmware as _compile
    from ..firmware.compile import generate_dfu_package

    log.info("Fleet deploy: compiling %s firmware...", platform)
    compile_result = await asyncio.to_thread(_compile, platform)
    if compile_result["status"] != "ok":
        raise HTTPException(500, f"Compilation failed: {compile_result['message']}")
    hex_path = compile_result["hex_path"]

    log.info("Fleet deploy: generating DFU package...")
    dfu_result = await asyncio.to_thread(generate_dfu_package, hex_path)
    if dfu_result["status"] != "ok":
        raise HTTPException(500, f"DFU package failed: {dfu_result['message']}")

    # Submit flash as background job
    body = FlashRequest(firmware_path=hex_path)
    flash_response = await fleet_flash(body)

    return {
        "status": "submitted",
        "hex_path": hex_path,
        "zip_path": dfu_result["zip_path"],
        "job_id": flash_response.get("job_id"),
        "devices": flash_response.get("devices"),
    }
