"""New-style flash job API (Phase 4 of the flash-job rewrite).

This is the API surface that Phase 11's deploy.sh will poll against. The
URLs intentionally use ``/flash-jobs/`` (not ``/jobs/`` or ``/fleet/jobs/``)
to avoid collision with the existing test-runner job endpoints
(``routes_testing.py``) and the legacy fleet flash-job endpoints
(``routes_firmware.py``). Both legacy surfaces stay unchanged for now.

While Phase 7+ live wiring is incomplete, ``POST /flash-jobs`` returns a
job that the stub orchestrator transitions to FAILED with an explicit
"transport not yet wired" error — so external callers can already
exercise the API shape against the test fleet without risking real
hardware writes.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from ..device.manager import FleetManager
from ..paths import firmware_dir
from .deps import get_fleet, require_api_key, require_deploy_tool

log = logging.getLogger(__name__)
router = APIRouter()


class FlashJobCreate(BaseModel):
    """Request body for ``POST /api/flash-jobs``.

    ``firmware_path`` is server-local — the caller must have already
    uploaded the firmware (via ``POST /api/fleet/upload``) and is
    pointing at the resulting path. Phase 11's deploy.sh chains the
    upload + create-job calls.
    """

    device_id: str = Field(..., min_length=1)
    firmware_path: str = Field(..., min_length=1)
    # Optional — if set, the verify state machine waits for the device's
    # ``json info`` response to match this string before completing.
    expected_version: str | None = None


@router.post(
    "/flash-jobs",
    status_code=202,
    dependencies=[Depends(require_api_key), Depends(require_deploy_tool)],
)
async def create_flash_job(
    body: FlashJobCreate,
    fleet: FleetManager = Depends(get_fleet),
) -> dict[str, Any]:
    """Create a flash job. Returns immediately (202 Accepted) with the
    job snapshot — the actual write runs in the background.

    Idempotent under concurrent calls: if a flash for the same device
    is already in flight, returns that job rather than creating a second.
    This is the structural guarantee against the duplicate-write cascade.

    Deploy-gated for the same reason ``/api/fleet/flash`` is — flashing
    is a device-mutating, hard-to-reverse operation; the
    ``X-Deploy-Tool`` header acts as the operator-intent check.
    """
    fw_path = Path(body.firmware_path).resolve()
    # Constrain `firmware_path` to the upload directory that
    # ``POST /api/fleet/upload`` writes to. Without this guard, any
    # API-key holder can probe arbitrary filesystem paths by observing
    # the 400 response (file exists vs. doesn't). Not exploitable
    # beyond what an enrolled operator already has — they still hold
    # the deploy-tool gate — but the constraint matches the principle
    # that firmware comes from the upload pipeline, not from random
    # paths the operator typed.
    upload_dir = firmware_dir().resolve()
    if not fw_path.is_relative_to(upload_dir):
        raise HTTPException(
            400,
            f"firmware_path must be under the upload directory ({upload_dir}); "
            f"got {body.firmware_path!r} (resolved: {fw_path})",
        )
    if not fw_path.is_file():
        # Surface the absolute resolved path alongside the operator-
        # supplied value so the operator doesn't have to guess which
        # working dir the server is interpreting a relative path
        # against.
        raise HTTPException(
            400,
            f"firmware_path does not exist on server: {body.firmware_path!r} (resolved: {fw_path})",
        )
    job = await fleet.flash_device(
        body.device_id,
        fw_path,
        expected_version=body.expected_version,
    )
    return job.to_dict()


@router.get(
    "/flash-jobs/{job_id}",
    dependencies=[Depends(require_api_key)],
)
async def get_flash_job(
    job_id: str,
    fleet: FleetManager = Depends(get_fleet),
) -> dict[str, Any]:
    """Return the current snapshot of one flash job.

    For long-polling: pass ``seq`` from a previous snapshot in your
    client; this endpoint always returns immediately (no server-side
    waiting). If you need to *wait* for the next change, add a small
    sleep + re-poll on the client side. Server-side long-poll (if we
    want it) would be a separate endpoint.

    Auth: requires ``X-API-Key``. Job metadata (firmware path, device
    ID, timestamps, anomalies) leaks operational detail, so the GET
    surface is gated like the POST.

    TODO: ``_flash_jobs`` is keyed by ``device_id``, so this is an O(n)
    scan over the table. Fine for small fleets; if a per-id long-poll
    endpoint ever lands, add a secondary ``_flash_jobs_by_id`` index.
    """
    for job in fleet.list_flash_jobs():
        if job.job_id == job_id:
            return job.to_dict()
    raise HTTPException(404, f"flash job not found: {job_id}")


@router.get(
    "/flash-jobs",
    dependencies=[Depends(require_api_key)],
)
async def list_flash_jobs(
    active: bool = False,
    fleet: FleetManager = Depends(get_fleet),
) -> dict[str, Any]:
    """List flash jobs. ``?active=true`` filters to in-flight only.

    Order: most-recently-created first, regardless of state. Useful for
    a dashboard view of "what's the flashing system been doing lately."

    Note: ``_flash_jobs`` retains exactly one ``FlashJob`` per
    ``device_id`` (the latest one), so multi-step history of a single
    device is NOT visible here — earlier jobs for the same device are
    overwritten. If multi-flash history matters, the L4 persistent
    audit log spec'd in ``docs/FLASH_LOCKDOWN_PLAN.md`` is the right
    home for it.

    Auth: requires ``X-API-Key``. Job metadata (firmware path, device
    ID, timestamps, anomalies) leaks operational detail, so the GET
    surface is gated like the POST.
    """
    jobs = fleet.list_flash_jobs(active_only=active)
    jobs.sort(key=lambda j: j.created_at, reverse=True)
    return {"jobs": [j.to_dict() for j in jobs], "count": len(jobs)}
