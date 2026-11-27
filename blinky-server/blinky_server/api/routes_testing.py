"""REST endpoints for test orchestration.

All test endpoints return a job_id immediately. Each test runs as a durable workflow
(``testing/workflows.py``, #325) that survives a server restart: poll GET /api/test/jobs/{id}
for progress and results. While the server is restarting, submissions answer 503.
"""

from __future__ import annotations

import contextlib
import os
from typing import Any, Literal

from blinky_scoring.track_discovery import discover_tracks
from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field, model_validator

from ..testing.audio_lock import LOCK_PATH, is_audio_locked, release_audio_lock
from ..work import durable, jobs
from .deps import assert_test_box_rules, get_fleet, require_api_key

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


def _check_commands(
    commands: list[str] | None, per_device: dict[str, list[str]] | None = None
) -> None:
    """A test job's command lists follow the command routes' test-box rules: one line each, and
    no box-only command (``leds``) off the box (deps.assert_test_box_rules)."""
    test_box = get_fleet().staging_enabled
    for cmd in [*(commands or []), *(c for cmds in (per_device or {}).values() for c in cmds)]:
        assert_test_box_rules(cmd, test_box=test_box)


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
    # On the test box, the chips are lit for every run (testing/leds.py). True: the run needs
    # the light (a camera review), so it fails if a chip can't be lit (firmware without
    # `leds`) instead of going on dark.
    needs_light: bool = False


class StagingSuiteRequest(BaseModel):
    sha: str = Field(..., pattern=r"^[0-9a-f]{40}$", description="The staging commit (full sha).")
    device_ids: list[str] = Field(..., min_length=1, description="Explicit TEST device IDs.")
    track_dir: str = Field(..., description="The validation corpus (corpus/edm)")
    track_names: list[str] | None = None
    limits: dict[str, float] | None = None
    build_of: str | None = Field(
        None,
        pattern=r"^[0-9a-f]{40}$",
        description="The PR head whose tree this commit has (the same firmware build): the "
        "strict rules' readiness counts the two as one build",
    )


class HardwareChecksRequest(BaseModel):
    sha: str = Field(..., pattern=r"^[0-9a-f]{40}$", description="The commit (full sha).")
    device_ids: list[str] = Field(..., min_length=1, description="Explicit TEST device IDs.")
    source: Literal["dev_build", "staging"] = Field(
        ..., description="dev_build: a build-service build (a PR head); staging: its artifact"
    )
    build_id: str | None = Field(None, pattern=r"^[A-Za-z0-9_-]{1,64}$")
    pr: int | None = Field(None, ge=1, description="The PR's number (display only)")
    runner: Literal["pr", "staging"] | None = Field(
        None, description="who submits it (box queue): default pr for dev_build, else staging"
    )

    @model_validator(mode="after")
    def _build(self) -> HardwareChecksRequest:
        if self.source == "dev_build" and not self.build_id:
            raise ValueError("a dev_build source names its build_id")
        return self


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
    _check_commands(body.commands, body.per_device_commands)
    from ..testing.workflows import validate as wf

    return await _submit(wf, body.model_dump())


@router.post("/staging", dependencies=[Depends(require_api_key)])
async def staging(body: StagingSuiteRequest) -> dict[str, Any]:
    """The staging hardware suite (#349, testing/staging.py): deploy the staging build of
    ``sha`` to the test devices, then health, music, beat pace, validation and timing.

    Refused unless the server runs with --staging (the test box). Returns a job_id; poll
    /test/jobs/{id}. The result is the report, ``passed`` its verdict.
    """
    from ..firmware import deploy_pipeline
    from ..testing.staging import DEFAULTS, staging_suite

    if not get_fleet().staging_enabled:
        raise HTTPException(
            403, "staging runs are off on this server (start it with --staging: the test box)"
        )
    unknown = sorted(set(body.limits or {}) - set(DEFAULTS))
    if unknown:
        raise HTTPException(422, f"unknown limits: {unknown}")
    from ..testing import box_mode
    from . import routes_dev, routes_firmware

    # One deploy-bearing job at a time, the suite included (routes_firmware._deploy_active):
    # the same lock makes the check and the submit one step.
    async with routes_firmware._deploy_lock:
        if box_mode.holds_queue():
            # The runner holds back in dev mode (docs/DEV_LOOP.md); this closes the race of a
            # runner that read the mode just before a switch. Retried on its next pass.
            raise HTTPException(409, "the box is in dev mode: staging suites wait for queue mode")
        if await routes_firmware._deploy_active():
            raise HTTPException(409, "A deploy or staging suite is already running")
        try:
            devices = deploy_pipeline.resolve_targets(get_fleet(), body.device_ids)
            deploy_pipeline.require_test_devices(get_fleet(), devices)
        except deploy_pipeline.DeployError as e:
            raise HTTPException(400, str(e)) from e
        # The suite starts with a deploy: a flash, so a reset (a dev flash just before the box
        # went back to queue mode, or checks cancelled mid-flash, may be seconds old).
        routes_dev.require_reset_gap(devices)
        return await _submit(staging_suite, body.model_dump())


@router.post("/hardware-checks", dependencies=[Depends(require_api_key)])
async def hardware_checks(body: HardwareChecksRequest) -> dict[str, Any]:
    """The hardware checks (testing/hardware_checks.py; docs/TEST_BOX.md): deploy the commit's
    build (a build-service build of a PR head, or staging's artifact) to the test chips, then
    the blocking yes/no checks in order: upload, lights, audio, response, interfaces. Queue mode
    only, on a server started with --staging; the deploy lock and the 60 s rule between resets
    as /dev/deploy. Returns a job_id; poll /test/jobs/{id}. The result's ``passed`` is the
    verdict; ``rig_error`` set means no verdict (a rig fault: try again)."""
    import asyncio

    from ..firmware import build_service, deploy_pipeline
    from ..testing import box_mode
    from ..testing.hardware_checks import hardware_checks as wf
    from . import routes_dev, routes_firmware

    if not get_fleet().staging_enabled:
        raise HTTPException(
            403, "hardware checks are off on this server (start it with --staging: the test box)"
        )
    dev_mode = "the box is in dev mode: hardware checks wait for queue mode"
    if box_mode.holds_queue():
        raise HTTPException(409, dev_mode)
    source: dict[str, Any] = {"kind": body.source, "sha": body.sha}
    if body.source == "dev_build":
        if build_service.config() is None:
            raise HTTPException(503, build_service.disabled_reason())
        try:
            rec = await asyncio.to_thread(build_service.build, str(body.build_id))
        except build_service.BuildServiceError as e:
            if e.code == 404:
                # Pruned or lost by the service: the runner builds the head again (410, not a
                # 5xx it would retry with the same id forever).
                raise HTTPException(410, f"the build service has no build {body.build_id}") from e
            raise HTTPException(502, str(e)) from e
        if rec.get("sha") != body.sha:
            raise HTTPException(400, f"build {body.build_id} is not of {body.sha[:12]}")
        if rec.get("state") != "done":
            raise HTTPException(409, f"build {body.build_id} is {rec.get('state')!r}, not done")
        source["build_id"] = body.build_id
    async with routes_firmware._deploy_lock:
        if box_mode.holds_queue():
            # Again under the lock, as the staging route: a switch to dev may have won it.
            raise HTTPException(409, dev_mode)
        if await routes_firmware._deploy_active():
            raise HTTPException(409, "A deploy or hardware test is already running")
        try:
            devices = deploy_pipeline.resolve_targets(get_fleet(), body.device_ids)
            deploy_pipeline.require_test_devices(get_fleet(), devices)
        except deploy_pipeline.DeployError as e:
            raise HTTPException(400, str(e)) from e
        routes_dev.require_reset_gap(devices)
        req: dict[str, Any] = {
            "sha": body.sha,
            "device_ids": body.device_ids,
            "source": source,
            "pr": body.pr,
            "runner": body.runner or ("pr" if body.source == "dev_build" else "staging"),
        }
        return await _submit(wf, req)


@router.delete("/hardware-checks/{job_id}", dependencies=[Depends(require_api_key)])
async def cancel_hardware_checks(job_id: str) -> dict[str, Any]:
    """Cancel hardware checks (their PR was closed, merged or superseded). They stop at the next
    step boundary (a flash in flight finishes); the LEDs go dark and the lock is released as
    on any other end. Only hardware-checks jobs: nothing else is cancelled here."""
    from dbos import DBOS

    from ..testing.workflows import TEST_JOBS

    _require_store()
    job = await jobs.get(TEST_JOBS, job_id)
    if not job or job["kind"] != "hardware-checks":
        raise HTTPException(404, f"no hardware-checks job {job_id}")
    if job["status"] in ("complete", "error", "cancelled"):
        return {"job_id": job_id, "status": job["status"]}
    await DBOS.cancel_workflow_async(job_id)
    return {"job_id": job_id, "status": "cancelled"}


@router.post("/param-sweep")
async def param_sweep(body: ParamSweepRequest) -> dict[str, Any]:
    """Run multi-device parameter sweep.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    _check_commands(body.commands)
    from ..testing.workflows import param_sweep as wf

    return await _submit(wf, body.model_dump())


@router.post("/tune-threshold")
async def tune_threshold(body: TuneThresholdRequest) -> dict[str, Any]:
    """Binary search for optimal onset threshold using real music.

    Returns immediately with a job_id. Poll /test/jobs/{id} for results.
    """
    _check_commands(body.commands)
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


@router.get("/strict-rules")
async def strict_rules_status() -> dict[str, Any]:
    """The strict rules (testing/strict_rules.py): the table (each rule's threshold, mode and
    the blinky-firmware issues it waits on), and each rule's readiness: how many distinct
    firmware builds in a row it has passed on, and whether that makes it ready to block."""
    import asyncio

    from ..testing import strict_rules

    return {
        "rules": strict_rules.table(),
        "ready_after": strict_rules.READY_AFTER,
        "readiness": await asyncio.to_thread(strict_rules.readiness),
    }


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
