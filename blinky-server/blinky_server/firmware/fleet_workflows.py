"""Fleet flash and fleet deploy as durable workflows (#325).

Each device's flash is one step: through ``FleetManager.flash_fleet`` for that device alone (the
single flash entry point, with its BLE-broadcaster pause and sibling reconnect holds), then the
USB settle before the next device. A restart lets the flash in flight finish (the drain waits
for it), and the job carries on with the next device when the server is back. A step that a
crash cut off runs again: it waits for discovery to find the device, and if the device already
reports the target version, it isn't flashed a second time.

``fleet.deploy`` is ``deploy_pipeline.run_deploy`` (every deploy.sh step) with these per-device
steps as its flash stage, so a deploy resumes after its last finished stage or device.
"""

from __future__ import annotations

import asyncio
import logging
from pathlib import Path
from typing import Any

from ..device.device import DeviceState
from ..work import durable, jobs
from . import deploy_pipeline, images
from .flash_job import FlashJobState

log = logging.getLogger(__name__)

FLEET_JOBS = jobs.Family(prefix="fleet.", kinds=("flash", "deploy"), result_steps=("flash_one",))
PER_DEVICE_TIMEOUT_S = 600.0
# After a restart, discovery needs a moment to find the device again.
DEVICE_WAIT_S = 120.0
# Before reading versions back: discovery refreshes device.version a cycle after the flash.
VERSION_SETTLE_S = 10.0
VERSION_POLL_S = 5.0


def _fleet() -> Any:
    from ..api.deps import get_fleet

    return get_fleet()


async def _wait_for_device(fleet: Any, device_id: str) -> Any:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + DEVICE_WAIT_S
    while (d := fleet.get_device(device_id)) is None and loop.time() < deadline:
        await asyncio.sleep(2)
    return d


@durable.step
async def flash_one(device_id: str, firmware: str, settle_after: bool) -> dict[str, Any]:
    """One device: flash and verify, then the USB settle if another device follows."""
    fleet = _fleet()
    image = images.load(firmware)
    expected_version = image.version if image is not None else None
    device = await _wait_for_device(fleet, device_id)
    short = device_id[:12]
    # Cut off by a restart, the write may have landed: check before acting (rule 2). The
    # rerun marker is set on the first run, so it is checked first, always.
    rerun = durable.rerun(f"flash-{device_id}")
    if (
        rerun
        and device is not None
        and expected_version is not None
        and device.state == DeviceState.CONNECTED
        and device.version == expected_version
    ):
        log.info(
            "fleet flash: %s already runs %s after the restart; not flashing it again",
            short,
            expected_version,
        )
        return {"device": short, "status": "ok", "note": "already at version (resumed)"}
    serial = device is not None and device.transport.transport_type == "serial"
    result: dict[str, Any] = {"device": short, "status": "error", "message": "not scheduled"}
    async for _did, fjob, err in fleet.flash_fleet(
        device_ids=[device_id],
        firmware_path=Path(firmware),
        per_device_timeout=PER_DEVICE_TIMEOUT_S,
    ):
        if fjob is None:
            result = {"device": short, "status": "error", "message": err or "device not scheduled"}
        elif err == "timeout":
            result = {
                "device": short,
                "status": "error",
                "message": f"flash exceeded per-device {PER_DEVICE_TIMEOUT_S:.0f}s timeout",
            }
        elif fjob.state is FlashJobState.COMPLETED:
            result = {
                "device": short,
                "status": "ok",
                "elapsed_s": round(fjob.duration_s, 1),
                "transport": fjob.transport.value if fjob.transport else None,
            }
        else:
            result = {
                "device": short,
                "status": "error",
                "message": fjob.error or f"flash {fjob.state.value} without specific error",
            }
    if result["status"] != "ok":
        log.error("Fleet flash: %s — %s; continuing to the next device", short, result["message"])
    if serial and settle_after:
        await fleet.usb_settle()
    return result


@durable.step
async def versions(short_ids: list[str]) -> dict[str, dict[str, Any]]:
    """Each flashed device's version and name, once discovery has refreshed them."""
    fleet = _fleet()
    await asyncio.sleep(VERSION_SETTLE_S)
    seen: dict[str, dict[str, Any]] = {}
    for _ in range(6):
        for d in fleet.get_all_devices():
            if d.id[:12] in short_ids and d.state == DeviceState.CONNECTED and d.version:
                seen[d.id[:12]] = {"version": d.version, "device_name": d.device_name}
        if len(seen) == len(short_ids):
            break
        await asyncio.sleep(VERSION_POLL_S)
    return seen


async def flash_devices(
    fleet: Any, device_ids: list[str], firmware: Path, job: Any
) -> dict[str, Any]:
    """The workflow body's flash stage: one step per device, strictly in order (one device at
    a time, finished before the next), then the version sweep. ``fleet`` and ``job`` are
    deploy_pipeline's flash-callback signature; progress goes through ``jobs.report``."""
    total = len(device_ids)
    results: dict[str, dict[str, Any]] = {}
    for i, device_id in enumerate(device_ids):
        jobs.report(int(i / total * 90), f"Flashing device {i + 1}/{total}")
        r = await flash_one(device_id, str(firmware), i < total - 1)
        results[r.pop("device")] = r
    jobs.report(90, "Verifying firmware versions")
    seen = await versions(list(results))
    per_device: dict[str, dict[str, Any]] = {}
    for short, r in results.items():
        v = seen.get(short, {})
        entry: dict[str, Any] = {
            "flash": r.get("status", "error"),
            "version": v.get("version"),
            "device_name": v.get("device_name"),
        }
        if r.get("status") != "ok" and r.get("message"):
            entry["message"] = r["message"]
        per_device[short] = entry
    ok = sum(1 for r in results.values() if r.get("status") == "ok")
    jobs.report(100, f"{ok}/{total} devices flashed")
    return {
        "status": "ok" if ok == total else "error",
        "message": f"{ok}/{total} devices flashed",
        "per_device": per_device,
    }


@durable.workflow("fleet.flash")
async def fleet_flash(device_ids: list[str], firmware: str) -> dict[str, Any]:
    return await flash_devices(_fleet(), device_ids, Path(firmware), None)


@durable.workflow("fleet.deploy")
async def fleet_deploy(
    release: str | None, device_ids: list[str] | str, download_dir: str
) -> dict[str, Any]:
    from ..api.routes_firmware import _store_release_hex

    return await deploy_pipeline.run_deploy(
        _fleet(),
        jobs.Progress(),
        release_tag=release,
        device_ids=device_ids,
        download_dir=Path(download_dir),
        store_firmware=_store_release_hex,
        flash=flash_devices,
    )
