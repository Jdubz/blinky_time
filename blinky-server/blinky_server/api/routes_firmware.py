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
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, Form, HTTPException, UploadFile

from ..device.device import DeviceState
from ..firmware.flash_job import FlashJob, FlashJobState
from ..paths import firmware_dir
from .deps import get_fleet, require_api_key, require_deploy_tool
from .models import FlashRequest

log = logging.getLogger(__name__)
router = APIRouter()


# Firmware + metadata live in persistent storage so auto-recovery still
# works after a reboot. The previous /tmp paths vanished on every power
# cycle, leaving any device stuck in DFU bootloader permanently dark.
def _firmware_meta_path() -> Path:
    return firmware_dir() / "firmware-meta.json"


def _firmware_hex_path() -> Path:
    return firmware_dir() / "firmware.hex"


def _save_firmware_meta(meta: dict[str, Any]) -> None:
    """Persist current firmware metadata to disk."""
    import json

    path = _firmware_meta_path()
    try:
        path.write_text(json.dumps(meta))
    except OSError:
        log.warning("Failed to persist firmware metadata to %s", path)


def _load_firmware_meta() -> dict[str, Any] | None:
    """Load persisted firmware metadata, or None if not available."""
    import json

    path = _firmware_meta_path()
    try:
        if path.is_file():
            return json.loads(path.read_text())  # type: ignore[no-any-return]
    except (OSError, json.JSONDecodeError):
        pass
    return None


# ── Upload (instant) ─────────────────────────────────────────────────


@router.post(
    "/fleet/upload",
    dependencies=[Depends(require_api_key), Depends(require_deploy_tool)],
)
async def fleet_upload(
    firmware: UploadFile,
    version: str | None = Form(None),
) -> dict[str, Any]:
    """Upload a firmware .hex file to the server. Does NOT flash.

    Accepts an optional `version` form field (e.g., "b130") to record
    the build number. If omitted, firmware is stored without version
    tracking (flash still works, but version comparison is unavailable).

    Returns the stored firmware path for use with POST /api/fleet/flash.
    """
    import re

    if not firmware.filename or not firmware.filename.endswith(".hex"):
        raise HTTPException(400, "Firmware must be a .hex file")
    if version is not None and not re.match(r"^b\d+$", version):
        raise HTTPException(400, f"Invalid version format '{version}' (expected: b<number>)")

    content = await firmware.read()
    if len(content) < 1000:
        raise HTTPException(400, f"Firmware file too small ({len(content)} bytes)")
    if len(content) > 5_000_000:
        raise HTTPException(400, f"Firmware file too large ({len(content)} bytes)")

    # Use a stable filename (not user-controlled) to avoid path issues.
    # Overwriting is intentional — only the latest upload matters.
    hex_path = _firmware_hex_path()
    hex_path.write_bytes(content)

    log.info("Fleet upload: stored firmware.hex (%d bytes, version=%s)", len(content), version)

    # Persist firmware metadata for version tracking
    import datetime

    meta = {
        "version": version,
        "firmware_path": str(hex_path),
        "uploaded_at": datetime.datetime.now(datetime.UTC).isoformat(),
        "size_bytes": len(content),
    }
    _save_firmware_meta(meta)

    return {
        "status": "ok",
        "firmware_path": str(hex_path),
        "firmware_size": len(content),
        "version": version,
        "message": f"Firmware stored ({len(content)} bytes). Use POST /api/fleet/flash to deploy.",
    }


# ── Firmware status ──────────────────────────────────────────────────


@router.get("/fleet/firmware")
async def fleet_firmware_status() -> dict[str, Any]:
    """Get current firmware version and per-device update status.

    Returns the uploaded firmware version (if known) and each connected
    device's version with an `up_to_date` flag.
    """
    meta = _load_firmware_meta()
    current_version = meta.get("version") if meta else None
    firmware_available = meta is not None and Path(meta.get("firmware_path", "")).is_file()

    fleet = get_fleet()
    devices = []
    out_of_date = 0
    for d in fleet.get_all_devices():
        if d.state != DeviceState.CONNECTED:
            continue
        is_current = (
            current_version is not None and d.version is not None and d.version == current_version
        )
        if not is_current and current_version:
            out_of_date += 1
        devices.append(
            {
                "id": d.id,
                "device_name": d.device_name,
                "version": d.version,
                "up_to_date": is_current,
            }
        )

    return {
        "current_version": current_version,
        "firmware_path": meta.get("firmware_path") if meta else None,
        "firmware_available": firmware_available,
        "uploaded_at": meta.get("uploaded_at") if meta else None,
        "devices": devices,
        "out_of_date_count": out_of_date,
    }


# ── Flash (background job) ───────────────────────────────────────────


@router.post(
    "/fleet/flash",
    dependencies=[Depends(require_api_key), Depends(require_deploy_tool)],
)
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
    # PRESENT BLE devices are flashable too — we just-in-time connect for
    # the flash and disconnect after. The broadcaster gets paused around
    # the flash (BCM43455 can't both advertise and act as central).
    flashable_states = (
        DeviceState.CONNECTED,
        DeviceState.DFU_RECOVERY,
        DeviceState.PRESENT,
    )

    if body.device_ids is not None:
        # Explicit whitelist: only flash THESE devices. Validate every ID
        # resolves and is flashable — fail loud on a typo or stale ID rather
        # than silently dropping it (operator thought they were deploying
        # to N but actually deployed to N-1).
        devices = []
        missing: list[str] = []
        wrong_state: list[str] = []
        for device_id in body.device_ids:
            d = fleet.get_device(device_id)
            if d is None:
                missing.append(device_id)
                continue
            if d.platform != "nrf52840" or d.state not in flashable_states:
                wrong_state.append(f"{device_id}({d.platform},{d.state.value})")
                continue
            devices.append(d)
        if missing or wrong_state:
            raise HTTPException(
                400,
                "device_ids whitelist had bad entries — "
                f"unknown: {missing or '[]'}, "
                f"not flashable: {wrong_state or '[]'}",
            )
    else:
        # No whitelist provided — back-compat: flash all eligible. Auto-
        # recovery WILL NOT be armed in this branch (see below).
        devices = [
            d
            for d in fleet.get_all_devices()
            if d.platform == "nrf52840" and d.state in flashable_states
        ]
    if not devices:
        raise HTTPException(404, "No flashable nRF52840 devices")

    device_ids = [d.id for d in devices]
    if body.device_ids is not None:
        # Only arm auto-recovery for explicitly-scoped deploys. An unscoped
        # fleet flash skips arming so we don't auto-flash a dev unit on the
        # bench during the next cycle.
        fleet.set_recovery_firmware(str(firmware), device_ids)
    else:
        log.warning(
            "Fleet flash without device_ids whitelist — auto-recovery NOT armed "
            "(pass device_ids in FlashRequest to opt in)"
        )

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
    return [dict(j.to_dict()) for j in jm.list_recent()]


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
    """L3b: thin wrapper around ``FleetManager.flash_fleet()``.

    Drives the JobManager super-job's progress display from the strictly-
    serial per-device iterations the orchestrator runs. The actual flash
    work (per-device transport selection, write, verify, BLE radio
    coordination, sibling serial holds, udevadm settle between
    iterations) lives inside ``flash_fleet()`` / ``_run_flash_job`` —
    NOT here. The legacy ``firmware.upload_firmware`` import is gone.

    deploy.sh + UI clients poll
    ``GET /api/fleet/jobs/{id}`` and read ``progressMessage`` —
    formatting that string is the route's responsibility, since
    flash_fleet's contract is data (per-iteration results) not
    presentation.
    """
    results: dict[str, dict[str, Any]] = {}
    total = len(device_ids)

    def _label(device_id: str, fjob: FlashJob | None) -> str:
        d = fleet.get_device(device_id)
        name = (d.device_name if d is not None else None) or "unknown"
        return f"{device_id[:12]} ({name})"

    def _on_iteration(
        index: int, count: int, device_id: str, fjob: FlashJob | None, err: str | None
    ) -> None:
        # Per-iteration progress: report the NEXT device about to flash,
        # or "Verifying" once the last one's done. `index` is 0-based and
        # already reflects the device that just finished.
        if index + 1 < count:
            job.progress = int(((index + 1) / count) * 90)
            job.progress_message = f"Flashing device {index + 2}/{count}"
        else:
            job.progress = 90
            job.progress_message = "Verifying firmware versions"

    # `flash_fleet` is an async generator that yields one tuple per
    # device as that device's flash reaches a terminal state. We pull
    # them as they come; strict-serial ordering is the generator's
    # invariant.
    job.progress = 0
    job.progress_message = f"Flashing device 1/{total}"

    async for device_id, fjob, err in fleet.flash_fleet(
        device_ids=device_ids,
        firmware_path=firmware,
        per_device_timeout=600.0,
        on_iteration=_on_iteration,
    ):
        short_id = device_id[:12]
        dev_label = _label(device_id, fjob)

        if fjob is None:
            # Device couldn't be scheduled (not in fleet, etc.)
            results[short_id] = {
                "status": "error",
                "message": err or "device not scheduled",
            }
            log.error("Fleet flash: %s — %s", dev_label, err)
            continue

        if err == "timeout":
            results[short_id] = {
                "status": "error",
                "message": "flash exceeded per-device 600s timeout",
            }
            log.error("Fleet flash: %s timeout", dev_label)
            continue

        if fjob.state is FlashJobState.COMPLETED:
            results[short_id] = {
                "status": "ok",
                "elapsed_s": round(fjob.duration_s, 1),
                "transport": fjob.transport.value if fjob.transport else None,
            }
        else:
            # FAILED or ABANDONED
            results[short_id] = {
                "status": "error",
                "message": fjob.error or f"flash {fjob.state.value} without specific error",
            }
            log.error(
                "Fleet flash: %s ended in %s — continuing to next device",
                dev_label,
                fjob.state.value,
            )

    # Post-iteration version sweep: the orchestrator's verify already
    # observed each device's app boot, but device.version / device_name
    # may take an extra discovery cycle to reach the in-memory device
    # entry. 10s gives the manager's loop time to refresh those fields
    # before we build the response.
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
    per_device: dict[str, dict[str, Any]] = {}
    for short_id, flash_result in results.items():
        v = verified.get(short_id, {})
        per_device_entry: dict[str, Any] = {
            "flash": flash_result.get("status", "error"),
            "version": v.get("version"),
            "device_name": v.get("device_name"),
        }
        # Surface the failure detail so deploy.sh + any other client
        # rendering per-device results can print *why* a device failed,
        # not just that it did. Only included for failures, to keep the
        # success path noise-free.
        if flash_result.get("status") != "ok" and flash_result.get("message"):
            per_device_entry["message"] = flash_result["message"]
        per_device[short_id] = per_device_entry

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
