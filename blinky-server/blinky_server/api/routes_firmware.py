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

    # Single-radio adapters (BCM43455 on Pi 4) refuse to act as central
    # (GATT connect) while advertising. BLE DFU + UF2-flash-of-BLE-devices
    # both need a central connection, so stop the broadcaster before any
    # device-level flash and restart it after the whole job.
    broadcaster_was_running = fleet.broadcaster is not None and fleet.broadcaster.is_running
    if broadcaster_was_running:
        log.info("Fleet flash: pausing broadcaster for the duration of the job")
        try:
            await fleet.broadcaster.stop()
        except Exception:
            log.exception("Failed to pause broadcaster cleanly; flash will still try")
        # P3 guard: refuse to proceed if the broadcaster did NOT actually
        # stop. Starting BLE DFU with the radio still advertising risks a
        # bluez "Failed (0x03)" mid-flash. Better to fail loud here than
        # corrupt a device.
        if fleet.broadcaster is not None and fleet.broadcaster.is_running:
            log.error(
                "Fleet flash aborted: broadcaster did not stop cleanly. "
                "Refusing to enter DFU under radio contention."
            )
            return {
                "status": "error",
                "message": "broadcaster did not stop; flash aborted to avoid radio contention",
                "per_device": {},
            }

    try:
        for i, dev_id in enumerate(device_ids):
            device = fleet.get_device(dev_id)
            if not device:
                results[dev_id[:12]] = {"status": "error", "message": "Device not found"}
                continue

            dev_label = f"{device.id[:12]} ({device.device_name or 'unknown'})"
            job.progress = int((i / total) * 90)
            base_msg = f"Flashing {dev_label} ({i + 1}/{total})"
            job.progress_message = base_msg
            log.info("Fleet flash: %s", job.progress_message)

            fleet.hold_reconnect(device.id, 120)

            # Per-device progress callback: surface upload phase
            # ("Releasing port", "USB-resetting", "Running uf2_upload.py", ...)
            # so the client polling job.progress_message can see what's
            # actually happening instead of a static "Flashing X (3/4)".
            # Default-arg bindings (B023): capture base_msg + job by value
            # so a stale closure from a prior iteration cannot fire after
            # the loop advances.
            def _per_phase_progress(
                phase: str,
                msg: str,
                pct: int | None = None,
                _base: str = base_msg,
                _job: Any = job,
            ) -> None:
                # Include pct in the rendered message when the phase has
                # one — uf2_upload.py emits 10/15/20/100 at known stages,
                # which is genuine progress information; dropping it
                # would be the soft silent-fallback pattern (accepting
                # an arg and silently ignoring it).
                pct_str = f" [{pct}%]" if pct is not None else ""
                _job.progress_message = f"{_base} — {phase}: {msg}{pct_str}"

            try:
                # P5: hard 600s per-device timeout. Observed BLE DFU rate
                # is ~21s per 10% of a 542KB image so transfer is ~3.5
                # minutes, plus init/scan/cache overhead. 600s leaves ~2x
                # margin without giving up on a slow but functional flash.
                # Without this, a hung flash would run until systemd's
                # watchdog or some other backstop tore us down — exactly
                # the failure mode that bricked cart_inner.
                result = await asyncio.wait_for(
                    upload_firmware(device, str(firmware), progress_callback=_per_phase_progress),
                    timeout=600.0,
                )
                results[device.id[:12]] = result
                if result.get("status") != "ok":
                    # Continue-on-error: one flaky USB port should not block
                    # the rest of the fleet. Record the failure and proceed
                    # to the next device. The aggregate result.status is
                    # still "error" if any device failed (computed below).
                    log.error(
                        "Fleet flash: %s failed (%s) — continuing to next device",
                        dev_label,
                        result.get("message"),
                    )
            except TimeoutError:
                results[device.id[:12]] = {
                    "status": "error",
                    "message": "flash exceeded 600s timeout",
                }
                log.error(
                    "Fleet flash: %s exceeded 600s — aborted, continuing to next device",
                    dev_label,
                )
            except Exception as e:
                results[device.id[:12]] = {"status": "error", "message": str(e)}
                log.error(
                    "Fleet flash: %s exception: %s — continuing to next device",
                    dev_label,
                    e,
                )
            finally:
                device.state = DeviceState.DISCONNECTED

            # Inter-device settle: 3s was too short on the 2026-05-01 deploy
            # where devices 3+4 hit `_wait_for_uf2_drive` 5s timeouts because
            # the host USB stack hadn't processed the previous device's reset
            # yet. Bumping to 8s + an explicit `udevadm settle` lets the kernel
            # finish queued USB events before we trigger the next DFU reset.
            # `udevadm settle --timeout=10` blocks until the udev event queue
            # is empty (or 10s, whichever comes first) — usually returns in
            # <1s when healthy, much longer when the stack is jammed (which
            # is the early-warning we want).
            await asyncio.sleep(8)
            try:
                proc = await asyncio.create_subprocess_exec(
                    "udevadm",
                    "settle",
                    "--timeout=10",
                    stdout=asyncio.subprocess.DEVNULL,
                    stderr=asyncio.subprocess.DEVNULL,
                )
                await proc.wait()
                # Exit code 1 means the udev event queue was still non-empty
                # after 10 s — exactly the host-USB-jam pattern from
                # 2026-05-01. Don't fail the deploy on it (we already slept
                # 8 s and the next device's flash will retry independently),
                # but DO log loudly so the operator sees the early-warning.
                # Pre-fix this was silently treated as success, defeating the
                # purpose of calling settle in the first place. Per PR 138
                # review (claude bot HIGH).
                if proc.returncode != 0:
                    log.warning(
                        "udevadm settle exited rc=%d after 10s — USB event "
                        "queue still busy. Next device's flash may hit the "
                        "_wait_for_uf2_drive 5s timeout (see "
                        "feedback_brick_diagnosis_first_rule).",
                        proc.returncode,
                    )
            except (FileNotFoundError, OSError) as e:
                # udevadm not on PATH or other fork issue — log and continue.
                # Not a deploy-stopper; the 8s sleep alone usually suffices.
                log.warning("udevadm settle skipped: %s", e)
    finally:
        job.progress_message = "Waiting for USB stabilization"
        log.info("Fleet flash: waiting 10s for USB stabilization...")
        await asyncio.sleep(10)
        for sid in all_serial_ids:
            fleet.resume_reconnect(sid)
        fleet.resume_discovery()
        if broadcaster_was_running and fleet.broadcaster is not None:
            log.info("Fleet flash: restarting broadcaster")
            try:
                await fleet.broadcaster.start()
            except Exception:
                log.exception(
                    "Failed to restart broadcaster after flash — fleet commands will "
                    "not be delivered over BLE until the server is restarted"
                )

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
        per_device_entry: dict[str, Any] = {
            "flash": flash_result.get("status", "error"),
            "version": v.get("version"),
            "device_name": v.get("device_name"),
        }
        # Surface the failure detail so deploy.sh (and any other client
        # rendering the per-device result) can print *why* a device
        # failed, not just that it did. Only included for failures to
        # keep the success path noise-free.
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
