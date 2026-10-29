"""REST endpoints for test orchestration.

All test endpoints return a job_id immediately. Each test runs as a durable workflow
(``testing/workflows.py``, #325) that survives a server restart: poll GET /api/test/jobs/{id}
for progress and results. While the server is restarting, submissions answer 503.
"""

from __future__ import annotations

import contextlib
import os
from typing import Any

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field, model_validator

from ..testing.audio_lock import LOCK_PATH, is_audio_locked, release_audio_lock
from ..testing.track_discovery import discover_tracks
from ..work import durable, jobs
from .deps import get_fleet

router = APIRouter(prefix="/test", tags=["testing"])


async def _submit(fn: Any, *args: Any) -> dict[str, Any]:
    """Start a durable test job; 503 (with Retry-After) while the server drains for a restart."""
    try:
        job_id = await durable.submit(fn, *args)
    except durable.Draining as e:
        raise HTTPException(503, str(e), headers={"Retry-After": "30"}) from e
    except durable.NotRunning as e:
        raise HTTPException(503, str(e)) from e
    return {"job_id": job_id, "status": "submitted"}


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
    # When true, the per-device score JSON also carries the raw capture
    # (signal_frames + transients) needed for gate (b) TP-vs-FP analysis.
    # Adds ~20-50 KB per device per track to the result payload -- don't
    # enable for routine validation runs. See ml-training/analysis/run_gate_b.py.
    persist_raw: bool = False


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


class TuneThresholdRequest(BaseModel):
    device_id: str = Field(..., min_length=1)
    param_name: str = Field("odfgate", min_length=1)
    low: float = 0.05
    high: float = 0.80
    track_dir: str = Field(..., description="Directory with audio + .beats.json files")
    track_names: list[str] | None = None
    duration_ms: float = 35000
    settle_ms: float = 12000
    max_steps: int = Field(
        8,
        ge=4,
        le=20,
        description="Step budget: ~60% coarse sweep, ~40% refinement. Actual evals may be slightly higher.",
    )
    # The drum metrics are the primary score (#294, CLAUDE.md); onsetF1 counts hats and other
    # non-targets, so tuning on it can trade kicks for hats.
    target_metric: str = "drumF"
    commands: list[str] | None = None

    @model_validator(mode="after")
    def _check_low_lt_high(self) -> TuneThresholdRequest:
        if self.low >= self.high:
            msg = f"low ({self.low}) must be less than high ({self.high})"
            raise ValueError(msg)
        return self


class NnCaptureRequest(BaseModel):
    duration_ms: float = Field(30000, ge=1000, le=300000)
    output_path: str | None = None


# ── Test Endpoints ──


@router.post("/validate")
async def validate(body: ValidateRequest) -> dict[str, Any]:
    """Run validation suite — play tracks, score onset + PLP metrics.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    from ..testing.workflows import validate as wf

    return await _submit(wf, body.model_dump())


@router.post("/param-sweep")
async def param_sweep(body: ParamSweepRequest) -> dict[str, Any]:
    """Run multi-device parameter sweep.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    from ..testing.workflows import param_sweep as wf

    return await _submit(wf, body.model_dump())


@router.post("/tune-threshold")
async def tune_threshold(body: TuneThresholdRequest) -> dict[str, Any]:
    """Binary search for optimal onset threshold using real music.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    from ..testing.workflows import tune_threshold as wf

    return await _submit(wf, body.model_dump())


# ── NN Capture ──


@router.post("/capture-nn/{device_id}")
async def capture_nn(device_id: str, body: NnCaptureRequest) -> dict[str, Any]:
    """Capture NN diagnostic stream (mel bands + onset activation).

    Enables firmware `stream nn` mode, captures frames for the specified
    duration, saves JSONL output. Used for offline mel feature parity
    validation and NN inference verification.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    from pathlib import Path

    fleet = get_fleet()
    device = fleet.get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")

    from ..device.device import DeviceState

    if device.state != DeviceState.CONNECTED:
        raise HTTPException(409, f"Device not connected (state={device.state.value})")

    import re
    import uuid

    safe_id = re.sub(r"[^a-zA-Z0-9_-]", "_", device_id[:12])
    output = body.output_path or f"/tmp/nn-capture-{safe_id}-{uuid.uuid4().hex[:8]}.jsonl"

    # Validate output path is under /tmp or home directory
    resolved = Path(output).resolve()
    allowed = [Path.home(), Path("/tmp")]
    if not any(resolved.is_relative_to(d) for d in allowed):
        raise HTTPException(
            400, f"Output path must be under /tmp or home directory, got: {resolved}"
        )

    from ..testing.workflows import capture_nn as wf

    return await _submit(wf, device_id, body.duration_ms, output)


# ── Job Management ──


def _require_store() -> None:
    if not durable.launched():
        raise HTTPException(503, "the durable work store isn't running")


@router.get("/jobs")
async def list_jobs() -> list[dict[str, Any]]:
    """List recent test jobs."""
    _require_store()
    from ..testing.workflows import TEST_JOBS

    return await jobs.list_recent(TEST_JOBS)


@router.get("/jobs/{job_id}")
async def get_job(job_id: str) -> dict[str, Any]:
    """Get status/progress/result for a test job (raw captures inlined)."""
    _require_store()
    from ..testing.workflows import TEST_JOBS, inline_raw

    job = await jobs.get(TEST_JOBS, job_id)
    if not job:
        raise HTTPException(404, f"Job not found: {job_id}")
    return inline_raw(job)


# ── Track Discovery ──


@router.get("/tracks")
async def list_tracks(directory: str) -> list[dict[str, str]]:
    """Discover available test tracks in a directory.

    Directory must be under the user's home or /tmp to prevent traversal.
    """
    from pathlib import Path

    resolved = Path(directory).resolve()
    allowed = [Path.home(), Path("/tmp")]
    if not any(resolved.is_relative_to(d) for d in allowed):
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
