"""The test box's modes and dev endpoints (docs/DEV_LOOP.md, phase 1).

  GET  /api/box/mode            the mode, since when, the note, and the work waiting behind it
  PUT  /api/box/mode            {"mode": "dev"|"queue", "note"?, "preempt"?}   (API key)
  PUT  /api/box/queue/{runner}  {"waiting": [{"sha", "since", ...}]}: what a runner holds back
  POST /api/dev/deploy          {"build_id", "device_ids"}: a build-service build to test chips
  POST /api/dev/run             {"device_ids", "checks": [...]}: targeted checks

The dev endpoints answer 409 unless the box is in dev mode, and only on a server started with
--staging (the box). A dev deploy is the staging deploy pipeline (deploy_pipeline.run_deploy)
with a build-service package in place of the staging artifact: test chips only, a verified
package, the post-deploy version assertion, the image stored non-stable so auto-recovery stays
on the stable image (#251). It is the box's relaxed path, as /fleet/deploy-staging: the API key
and box chips only, never the X-Deploy-Tool header. Poll a deploy at /api/fleet/jobs/{id} and a
run at /api/test/jobs/{id}.
"""

from __future__ import annotations

import re
import time
from pathlib import Path
from typing import Any, Literal

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field, model_validator

from ..firmware import build_service, deploy_pipeline
from ..paths import firmware_dir, hosttools_dir
from ..testing import box_mode
from .deps import assert_test_box_rules, get_fleet, is_deploy_gated_command, require_api_key

router = APIRouter(tags=["dev"])

SHA = r"^[0-9a-f]{40}$"
RUNNER_RE = re.compile(r"[a-z][a-z0-9-]{0,31}")
# The firmware's rule: 60 s between resets of a chip (a flash is one). Its reset counter reads
# quicker ones as a boot loop.
RESET_GAP_S = 60.0


class BoxModeRequest(BaseModel):
    mode: Literal["dev", "queue"]
    note: str | None = Field(None, max_length=200)
    preempt: bool = Field(False, description="dev: cancel a running staging suite and requeue")


class QueueItem(BaseModel):
    sha: str = Field(..., pattern=SHA)
    since: float = Field(..., description="when it started waiting (epoch seconds)")
    what: str | None = Field(None, max_length=200)
    preempted: bool = False


class QueueRequest(BaseModel):
    waiting: list[QueueItem] = Field(default_factory=list, max_length=box_mode.MAX_WAITING)


class DevDeployRequest(BaseModel):
    build_id: str = Field(..., pattern=build_service.BUILD_ID_RE.pattern)
    device_ids: list[str] = Field(..., min_length=1, description="Explicit TEST device IDs.")


class DevCheck(BaseModel):
    kind: Literal["info", "health", "validate", "pace", "commands"]
    minutes: float | None = Field(None, gt=0, le=60, description="health: how long")
    tracks: list[str] | None = Field(None, description="validate (required), pace (optional)")
    commands: list[str] | None = Field(None, description="commands: lines sent in order")

    @model_validator(mode="after")
    def _fields(self) -> DevCheck:
        if self.kind == "validate" and not self.tracks:
            raise ValueError("a validate check names its tracks")
        if self.kind == "commands" and not self.commands:
            raise ValueError("a commands check lists its commands")
        return self


class DevRunRequest(BaseModel):
    device_ids: list[str] = Field(..., min_length=1)
    checks: list[DevCheck] = Field(..., min_length=1, max_length=20)
    track_dir: str | None = Field(None, description="default: the server's corpus/edm")


def _require_box() -> None:
    if not get_fleet().staging_enabled:
        raise HTTPException(
            403, "dev mode is the test box's (a server started with --staging); not here"
        )


def _require_dev() -> None:
    if box_mode.in_dev():
        return
    st = box_mode.snapshot()
    if st["switching"]:
        raise HTTPException(
            409,
            "the box is switching to dev mode, waiting for staging suite "
            f"{', '.join(st['switching']['waiting_for'])} to end (or PUT /api/box/mode with "
            "preempt)",
        )
    raise HTTPException(
        409, "the box is in queue mode: switch it to dev first (dev.py mode dev, PUT /api/box/mode)"
    )


def _targets(device_ids: list[str]) -> list[Any]:
    try:
        devices = deploy_pipeline.resolve_targets(get_fleet(), device_ids)
        deploy_pipeline.require_test_devices(get_fleet(), devices)
    except deploy_pipeline.DeployError as e:
        raise HTTPException(400, str(e)) from e
    return devices


# Beyond the deploy gate: a dev run's commands are runtime-only. `save` writes settings to
# flash, `bootloader` resets the chip, `ota ...` stages or commits an image.
_DEV_RUN_REFUSED = ("save", "bootloader", "ota")


def _persists_or_resets(cmd: str) -> bool:
    words = cmd.strip().lower().split(maxsplit=1)
    return bool(words) and words[0] in _DEV_RUN_REFUSED


def reset_wait(device: Any, now: float | None = None) -> float:
    """Seconds until ``device`` has run RESET_GAP_S since its last reset (0: it has), from the
    uptime in its last json info. Unknown uptime: 0 (the flash path has its own guards)."""
    info = getattr(device, "last_info", None) or {}
    at = getattr(device, "last_info_at", None)
    if "millis" not in info or at is None:
        return 0.0
    now = time.monotonic() if now is None else now
    uptime = float(info["millis"]) / 1000 + (now - float(at))
    return max(0.0, RESET_GAP_S - uptime)


def default_track_dir() -> Path:
    """The corpus beside the server's tree (corpus/edm), as the staging runner passes it."""
    return hosttools_dir().parent.parent / "corpus" / "edm"


# -- the mode ----------------------------------------------------------------------------------


@router.get("/box/mode")
async def get_mode() -> dict[str, Any]:
    """The box's mode (``dev`` or ``queue``), since when, its note, a pending switch, the work
    waiting behind dev mode (oldest first) and the staging suites dev mode preempted."""
    st = await box_mode.current()
    return {**st, "summary": box_mode.describe(st)}


@router.put("/box/mode", dependencies=[Depends(require_api_key)])
async def put_mode(body: BoxModeRequest) -> dict[str, Any]:
    """Switch the box. No expiry: it stays in dev mode until switched back. To dev while a
    staging suite runs: waits for it (``switching``), or ``preempt`` cancels it and queues its
    commit again."""
    from . import routes_firmware

    if body.mode == "dev":
        _require_box()
    # The suite route checks the mode under the same lock: no suite starts mid-switch.
    async with routes_firmware._deploy_lock:
        try:
            st = await box_mode.set_mode(body.mode, body.note, body.preempt)
        except box_mode.ModeError as e:
            raise HTTPException(409, str(e)) from e
    return {**st, "summary": box_mode.describe(st)}


@router.put("/box/queue/{runner}", dependencies=[Depends(require_api_key)])
async def put_queue(runner: str, body: QueueRequest) -> dict[str, Any]:
    """What a hardware runner holds back while the box is in dev mode (replaces its list)."""
    if not RUNNER_RE.fullmatch(runner):
        raise HTTPException(422, f"runner must be a short lowercase name, not {runner!r}")
    st = box_mode.set_waiting(runner, [i.model_dump(exclude_none=True) for i in body.waiting])
    return {**st, "summary": box_mode.describe(st)}


# -- dev flash and targeted tests --------------------------------------------------------------


@router.post("/dev/deploy", dependencies=[Depends(require_api_key)])
async def dev_deploy(body: DevDeployRequest) -> dict[str, Any]:
    """Deploy a build-service build to explicitly named test chips (dev mode only): fetch its
    package, verify it as a staging artifact is verified (firmware/build_service.py), then the
    staging deploy pipeline. Returns a job_id; poll /api/fleet/jobs/{id}."""
    from ..firmware.fleet_workflows import fleet_deploy_dev as wf
    from . import routes_firmware

    _require_box()
    _require_dev()
    if build_service.config() is None:
        raise HTTPException(503, build_service.disabled_reason())
    async with routes_firmware._deploy_lock:
        _require_dev()  # again under the lock: a switch to queue may have won it meanwhile
        if await routes_firmware._deploy_active():
            raise HTTPException(409, "A deploy or staging suite is already running")
        devices = _targets(body.device_ids)
        wait = max(reset_wait(d) for d in devices)
        if wait > 0:
            raise HTTPException(
                409,
                f"a chip reset {RESET_GAP_S - wait:.0f} s ago; the firmware's rule is "
                f"{RESET_GAP_S:.0f} s between resets: retry in {wait:.0f} s",
                headers={"Retry-After": str(int(wait) + 1)},
            )
        job_id = await routes_firmware._submit(
            wf, body.build_id, body.device_ids, str(firmware_dir() / "dev-download")
        )
    return {
        "job_id": job_id,
        "status": "submitted",
        "build_id": body.build_id,
        "message": f"Deploying build {body.build_id} to {len(body.device_ids)} test device(s)",
    }


@router.post("/dev/run", dependencies=[Depends(require_api_key)])
async def dev_run(body: DevRunRequest) -> dict[str, Any]:
    """Targeted checks on test chips, in order, in one test session (dev mode only;
    testing/dev_run.py). Health and the final asserts set pass/fail; scores are reported.
    Returns a job_id; poll /api/test/jobs/{id}."""
    from blinky_scoring.track_discovery import discover_tracks

    from ..testing.dev_run import dev_run as wf
    from .routes_testing import _submit

    _require_box()
    _require_dev()
    _targets(body.device_ids)
    track_dir = Path(body.track_dir) if body.track_dir else default_track_dir()
    if body.track_dir:
        resolved = track_dir.resolve()
        if not any(resolved.is_relative_to(d) for d in (Path.home(), Path("/tmp"))):
            raise HTTPException(400, f"track_dir must be under home or /tmp: {resolved}")
    wanted = {t for c in body.checks for t in (c.tracks or [])}
    if wanted or any(c.kind == "pace" for c in body.checks):
        try:
            have = {t["name"] for t in discover_tracks(str(track_dir))}
        except FileNotFoundError as e:
            raise HTTPException(400, f"no tracks: {e}") from e
        if missing := sorted(wanted - have):
            raise HTTPException(400, f"unknown tracks in {track_dir}: {missing}")
    for c in body.checks:
        for cmd in c.commands or []:
            assert_test_box_rules(cmd, test_box=True)
            if is_deploy_gated_command(cmd) or _persists_or_resets(cmd):
                # A reboot or a config write is not a check: the firmware's 60 s rule between
                # resets, and no config/reboot loops (CLAUDE.md).
                raise HTTPException(400, f"{cmd.strip()[:60]!r} isn't allowed in a dev run")
    req = {
        "device_ids": body.device_ids,
        "checks": [c.model_dump(exclude_none=True) for c in body.checks],
        "track_dir": str(track_dir),
    }
    return await _submit(wf, req)
