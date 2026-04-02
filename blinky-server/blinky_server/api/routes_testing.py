"""REST endpoints for test orchestration.

All test endpoints return a job_id immediately. Long-running tests
execute as async background tasks. Poll GET /api/test/jobs/{id} for
progress and results.
"""

from __future__ import annotations

import contextlib
import os
from typing import Any

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from ..testing.audio_lock import LOCK_PATH, is_audio_locked, release_audio_lock
from ..testing.job_manager import Job, JobManager
from ..testing.track_discovery import discover_tracks
from .deps import get_fleet

router = APIRouter(prefix="/test", tags=["testing"])

# Singleton job manager (created on first use)
_job_manager: JobManager | None = None


def _get_job_manager() -> JobManager:
    global _job_manager
    if _job_manager is None:
        _job_manager = JobManager()
    return _job_manager


# ── Request Models ──


class ValidateRequest(BaseModel):
    device_ids: list[str] = Field(..., min_length=1)
    track_dir: str = Field(..., description="Directory with audio + .beats.json files")
    track_names: list[str] | None = None
    duration_ms: float = 35000
    seek_sec: float | None = None
    settle_ms: float = 0
    num_runs: int = Field(1, ge=1, le=10)
    commands: list[str] | None = None
    per_device_commands: dict[str, list[str]] | None = None


class ParamSweepRequest(BaseModel):
    device_ids: list[str] = Field(..., min_length=1)
    param_name: str = Field(..., min_length=1)
    values: list[float] = Field(..., min_length=1)
    track_dir: str
    track_names: list[str] | None = None
    duration_ms: float = 35000
    settle_ms: float = 12000
    num_runs: int = Field(1, ge=1, le=10)
    commands: list[str] | None = None


# ── Test Endpoints ──


@router.post("/validate")
async def validate(body: ValidateRequest) -> dict[str, Any]:
    """Run validation suite — play tracks, score onset + PLP metrics.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    from ..testing.test_runner import run_validation

    fleet = get_fleet()
    jm = _get_job_manager()

    async def _run(job: Job) -> dict[str, Any]:
        return await run_validation(
            fleet,
            body.device_ids,
            track_dir=body.track_dir,
            track_names=body.track_names,
            duration_ms=body.duration_ms,
            seek_sec=body.seek_sec,
            settle_ms=body.settle_ms,
            num_runs=body.num_runs,
            commands=body.commands,
            per_device_commands=body.per_device_commands,
            job=job,
        )

    job = jm.submit("validate", _run)
    return {"job_id": job.id, "status": "submitted"}


@router.post("/param-sweep")
async def param_sweep(body: ParamSweepRequest) -> dict[str, Any]:
    """Run multi-device parameter sweep.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    from ..testing.test_runner import run_param_sweep

    fleet = get_fleet()
    jm = _get_job_manager()

    async def _run(job: Job) -> dict[str, Any]:
        return await run_param_sweep(
            fleet,
            body.device_ids,
            param_name=body.param_name,
            values=body.values,
            track_dir=body.track_dir,
            track_names=body.track_names,
            duration_ms=body.duration_ms,
            settle_ms=body.settle_ms,
            num_runs=body.num_runs,
            commands=body.commands,
            job=job,
        )

    job = jm.submit("param-sweep", _run)
    return {"job_id": job.id, "status": "submitted"}


# ── Job Management ──


@router.get("/jobs")
async def list_jobs() -> list[dict[str, Any]]:
    """List recent test jobs."""
    return [j.to_dict() for j in _get_job_manager().list_recent()]


@router.get("/jobs/{job_id}")
async def get_job(job_id: str) -> dict[str, Any]:
    """Get status/progress/result for a test job."""
    job = _get_job_manager().get(job_id)
    if not job:
        raise HTTPException(404, f"Job not found: {job_id}")
    return job.to_dict()


# ── Track Discovery ──


@router.get("/tracks")
async def list_tracks(directory: str) -> list[dict[str, str]]:
    """Discover available test tracks in a directory.

    Directory must be under the user's home or /tmp to prevent traversal.
    """
    from pathlib import Path

    resolved = Path(directory).resolve()
    allowed = [Path.home(), Path("/tmp")]
    if not any(str(resolved).startswith(str(d)) for d in allowed):
        raise HTTPException(400, f"Directory not in allowed path: {resolved}")
    try:
        return discover_tracks(directory)
    except FileNotFoundError as e:
        raise HTTPException(404, str(e)) from e


# ── Audio Lock ──


@router.get("/audio-lock")
async def check_audio_lock() -> dict[str, Any]:
    """Check audio lock status."""
    locked, info = is_audio_locked()
    return {"locked": locked, "holder": info}


@router.delete("/audio-lock")
async def force_release_audio_lock() -> dict[str, Any]:
    """Force-release a stuck audio lock.

    Checks if the holding process is still alive before removing.
    If the holder is alive, returns a warning instead of force-removing
    to avoid corrupting an in-progress audio playback.
    """
    import errno

    release_audio_lock()  # Release if this process holds it

    # Check if another process holds the lock
    locked, info = is_audio_locked()
    if not locked:
        return {"status": "released"}

    # Lock exists — check if holder is alive
    holder_pid = info.get("pid") if info else None
    if holder_pid:
        try:
            os.kill(holder_pid, 0)
            # Process is alive — warn instead of force-removing
            return {
                "status": "warning",
                "message": f"Lock held by live process PID {holder_pid}",
                "holder": info,
            }
        except OSError as e:
            if e.errno != errno.ESRCH:
                # EPERM = process exists but owned by another user
                return {
                    "status": "warning",
                    "message": f"Lock held by PID {holder_pid} (permission denied)",
                    "holder": info,
                }

    # Stale lock (dead process or no PID) — safe to remove
    with contextlib.suppress(OSError):
        os.unlink(LOCK_PATH)
    return {"status": "released", "was_stale": True}
