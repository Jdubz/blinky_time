"""Firmware upload, flash, and fleet management routes.

Upload and flash are separate operations:
  POST /api/fleet/upload  — accept .hex file, store it, return path (instant)
  POST /api/fleet/flash   — flash stored firmware to devices (background job)
  POST /api/fleet/deploy  — the whole hardened pipeline server-side: stable
                            release -> verify -> store -> flash -> restore +
                            save -> post-deploy assertion (firmware/deploy_pipeline.py)
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
from ..firmware import deploy_pipeline, images
from ..paths import firmware_dir
from ..work import durable, jobs
from .deps import get_fleet, require_api_key, require_deploy_tool
from .models import DeployRequest, FlashRequest

log = logging.getLogger(__name__)
router = APIRouter()


# Firmware + metadata live in persistent storage so auto-recovery still
# works after a reboot. The previous /tmp paths vanished on every power
# cycle, leaving any device stuck in DFU bootloader permanently dark.
def _firmware_meta_path() -> Path:
    return firmware_dir() / "firmware-meta.json"


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

    #251: the image is stored immutably (by sha256). If ``version`` names a
    stable release and the bytes match that release's hex (verified here,
    against GitHub), the image is marked stable and may arm auto-recovery.
    Otherwise it can still be flashed explicitly, but never auto-recovers.
    """
    import re

    if not firmware.filename or not firmware.filename.endswith(".hex"):
        raise HTTPException(400, "Firmware must be a .hex file")
    if version is not None and not re.match(r"^b\d+$", version):
        raise HTTPException(400, f"Invalid version format '{version}' (expected: b<number>)")

    content = await firmware.read()
    _check_size(content)
    if version:
        stable, reason = await asyncio.to_thread(
            images.verify_stable, content, version, firmware_dir() / "release-verify"
        )
    else:
        stable, reason = False, "no version given"
    if not stable:
        log.warning(
            "Uploaded firmware is not a verified stable release (%s): auto-recovery won't use it",
            reason,
        )
    image = _store_firmware(content, version, stable=stable, source="upload")

    return {
        "status": "ok",
        "firmware_path": str(image.path),
        "firmware_size": len(content),
        "version": version,
        "sha256": image.sha256,
        "stable_verified": image.stable,
        "stable_reason": reason,
        "message": f"Firmware stored ({len(content)} bytes). Use POST /api/fleet/flash to deploy.",
    }


def _check_size(content: bytes) -> None:
    if len(content) < 1000:
        raise HTTPException(400, f"Firmware file too small ({len(content)} bytes)")
    if len(content) > 5_000_000:
        raise HTTPException(400, f"Firmware file too large ({len(content)} bytes)")


def _store_firmware(
    content: bytes, version: str | None, *, stable: bool, source: str
) -> images.FirmwareImage:
    """Store `content` immutably (#251) and make it the server's current firmware.

    Shared by /api/fleet/upload and /api/fleet/deploy. The image is never
    overwritten by a later upload, so what auto-recovery is armed with can't
    change underneath it; ``stable`` must come from this server's own check.
    """
    import datetime

    _check_size(content)
    image = images.store(
        content,
        version=version,
        stable=stable,
        source=source,
        keep=get_fleet().recovery_firmware_path,
    )
    log.info(
        "Fleet firmware stored: %s (%d bytes, version=%s, stable=%s)",
        image.path.name,
        len(content),
        version,
        image.stable,
    )
    _save_firmware_meta(
        {
            "version": version,
            "firmware_path": str(image.path),
            "sha256": image.sha256,
            "stable": image.stable,
            "uploaded_at": datetime.datetime.now(datetime.UTC).isoformat(),
            "size_bytes": len(content),
        }
    )
    return image


def _arm_recovery_if_stable(fleet: Any, firmware: Path, device_ids: list[str]) -> dict[str, Any]:
    """Arm auto-recovery for ``device_ids`` only if ``firmware`` is a
    verified stable image (#251). Otherwise leave the previous arming as it
    is, and say why, loudly."""
    image = images.load(firmware)
    if image is None or not image.stable:
        why = (
            "not a verified stable-release image"
            if image is not None
            else "not an image in the server's store"
        )
        log.warning("Auto-recovery NOT re-armed for %s: %s is %s (#251)", device_ids, firmware, why)
        return {"recovery_armed": False, "recovery_note": f"{firmware.name}: {why} (#251)"}
    fleet.set_recovery_firmware(str(firmware), device_ids)
    return {"recovery_armed": True, "recovery_version": image.version}


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
    flashable_states = deploy_pipeline.FLASHABLE_STATES

    if body.device_ids is not None:
        # Explicit whitelist: only flash THESE devices. The same resolver
        # /api/fleet/deploy uses: every ID must resolve and be flashable,
        # failing loud on a typo or stale ID rather than silently dropping it.
        if not body.device_ids:
            raise HTTPException(400, "device_ids is empty: name the devices to flash")
        try:
            devices = deploy_pipeline.resolve_targets(fleet, body.device_ids)
        except deploy_pipeline.DeployError as e:
            raise HTTPException(400, str(e)) from e
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
    recovery: dict[str, Any] = {"recovery_armed": False}
    if body.device_ids is not None:
        # Only arm auto-recovery for explicitly-scoped deploys, and only
        # with a verified stable image (#251). An unscoped fleet flash
        # skips arming so we don't auto-flash a dev unit on the bench.
        recovery = _arm_recovery_if_stable(fleet, firmware, device_ids)
    else:
        log.warning(
            "Fleet flash without device_ids whitelist — auto-recovery NOT armed "
            "(pass device_ids in FlashRequest to opt in)"
        )

    from ..firmware.fleet_workflows import fleet_flash as wf

    job_id = await _submit(wf, device_ids, str(firmware))

    return {
        "job_id": job_id,
        "status": "submitted",
        "devices": len(devices),
        **recovery,
        "message": f"Flashing {len(devices)} device(s) in background",
    }


# ── Job status ────────────────────────────────────────────────────────


@router.get("/fleet/jobs/{job_id}")
async def get_flash_job(job_id: str) -> dict[str, Any]:
    """Get the status of a fleet flash or deploy job (durable: survives restarts, #325)."""
    from ..firmware.fleet_workflows import FLEET_JOBS

    _require_store()
    job = await jobs.get(FLEET_JOBS, job_id)
    if not job:
        raise HTTPException(404, f"Job {job_id} not found")
    return job


@router.get("/fleet/jobs")
async def list_flash_jobs() -> list[dict[str, Any]]:
    """List fleet flash and deploy jobs."""
    from ..firmware.fleet_workflows import FLEET_JOBS

    _require_store()
    return await jobs.list_recent(FLEET_JOBS)


def _require_store() -> None:
    if not durable.launched():
        raise HTTPException(503, "the durable work store isn't running")


async def _submit(fn: Any, *args: Any) -> str:
    """Start a durable fleet job; 503 (with Retry-After) while the server drains."""
    try:
        return await durable.submit(fn, *args)
    except durable.Draining as e:
        raise HTTPException(503, str(e), headers={"Retry-After": "30"}) from e
    except durable.NotRunning as e:
        raise HTTPException(503, str(e)) from e


# ── Deploy (the hardened pipeline, server-side) ───────────────────────

_deploy_lock = asyncio.Lock()


async def _deploy_active() -> bool:
    """Whether a fleet deploy hasn't finished (running, or held for a restart). With no store
    there can be none, and ``_submit`` then refuses the new one with a 503 anyway."""
    from ..firmware.fleet_workflows import FLEET_JOBS

    if not durable.launched():
        return False
    return bool(await jobs.active(FLEET_JOBS, "deploy"))


def _store_release_hex(hex_path: Path, tag: str) -> Path:
    # Stable by construction: fetch_stable_release refused anything that isn't
    # a promoted stable release, and verified every byte against its manifest.
    try:
        return _store_firmware(hex_path.read_bytes(), tag, stable=True, source="deploy").path
    except (HTTPException, OSError) as e:
        detail = e.detail if isinstance(e, HTTPException) else str(e)
        raise deploy_pipeline.DeployError("store", str(detail)) from e


@router.post("/fleet/deploy", dependencies=[Depends(require_api_key)])
async def fleet_deploy(body: DeployRequest) -> dict[str, Any]:
    """Deploy a STABLE release to explicitly named devices, server-side.

    Runs deploy.sh's hardened pipeline with the modules deploy.sh relies on
    (firmware/deploy_pipeline.py): release download + sha256 verify (stable
    only), store, flash through the single orchestrator, restore runtime
    settings + save, post-deploy assertion. It never compiles, and never
    flashes a local build (#269).

    No X-Deploy-Tool header: that header stops callers from skipping the
    pipeline by calling the low-level /fleet/upload + /fleet/flash directly.
    This route IS the pipeline. Needs the API key and an explicit scope
    (device_ids, or "all").

    Returns a job_id; poll GET /api/fleet/jobs/{job_id}. The result says
    which step failed, if any.
    """
    from ..firmware.fleet_workflows import fleet_deploy as wf

    # One deploy at a time: two would race on the stored firmware and the fleet. The lock
    # makes the check and the submit one step; a deploy held for a restart still counts.
    async with _deploy_lock:
        if await _deploy_active():
            raise HTTPException(409, "A fleet deploy is already running")
        # Validate the scope now so a typo fails the request, not a job later.
        try:
            deploy_pipeline.resolve_targets(get_fleet(), body.device_ids)
        except deploy_pipeline.DeployError as e:
            raise HTTPException(400, str(e)) from e
        job_id = await _submit(
            wf, body.release, body.device_ids, str(firmware_dir() / "release-download")
        )
    scope = (
        body.device_ids if isinstance(body.device_ids, str) else f"{len(body.device_ids)} device(s)"
    )
    return {
        "job_id": job_id,
        "status": "submitted",
        "release": body.release or "latest stable",
        "scope": scope,
        "message": f"Deploying {body.release or 'the latest stable release'} to {scope} in background",
    }
