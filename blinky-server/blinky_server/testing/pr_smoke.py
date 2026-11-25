"""The PR hardware queue's smoke test (docs/DEV_LOOP.md, "PR hardware queue"):
``POST /api/test/pr-smoke``, one durable job, queue mode only.

The PR runner (scripts/testbox/pr_queue.py) builds a PR's head on the build service and submits
this job with the build id. It is the staging suite's and the dev run's own functions, in the
suite's order, on a shorter list:

1. deploy: the build-service dev package through ``deploy_pipeline.run_deploy`` with
   ``dev_build``, the path ``/api/dev/deploy`` takes (test chips only, ``verify_dev_package``,
   the post-deploy version assertion, the image stored non-stable so auto-recovery stays on the
   stable image), and the build must be of the requested commit;
2. health: the suite's health step for ``stable_minutes`` (2 by default);
3. info: the dev run's info check, the ``json info`` timers;
4. tracks: one validation run per track (DEFAULT_TRACKS unless the request names others),
   scored as the suite scores them, at runtime defaults and streaming only while a track plays;
5. final: the suite's ``final_state``: version unchanged, no reboot since the flash, and no
   firmware ``[ASSERT]``/``[FALLBACK]`` line from the flash on.

The whole job holds the audio lock (workflows.reserve), and the chips' LEDs are lit from the
deploy's end to the job's end however it ends (testing/leds.py): dark afterwards.

Pass = deploy + health + final. The scores are reported with the corpus pin and never gate
(owner, 2026-11-25).
"""

from __future__ import annotations

import asyncio
import subprocess
from pathlib import Path
from typing import Any

from ..firmware import deploy_pipeline
from ..paths import firmware_dir
from ..work import durable, jobs
from . import dev_run, staging
from .workflows import _fleet, lit, reserve

DEFAULT_TRACKS = ("techno-machine-drum", "dnb-energetic-breakbeat", "garage-uk-2step")
HEALTH_MINUTES = 2.0
GATES = ("deploy", "health", "final")


def _git_head(path: str) -> str | None:
    try:
        r = subprocess.run(
            ["git", "-C", path, "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    out = r.stdout.strip()
    return out if r.returncode == 0 and len(out) == 40 else None


@durable.step
async def corpus_pin(track_dir: str) -> str | None:
    """The corpus commit the tracks come from (the blinky-corpus submodule), or None: scores
    compare only within one pin."""
    return await asyncio.to_thread(_git_head, track_dir)


def verdict(report: dict[str, Any]) -> bool:
    """Deploy, every health check and the final asserts pass; the scores never gate."""
    deploy = report.get("deploy") or {}
    return (
        bool(deploy.get("pass"))
        and "health" in report
        and staging._passed(report["health"])
        and "final" in report
        and staging._passed(report["final"])
    )


@durable.workflow("test.pr-smoke")
async def pr_smoke(req: dict[str, Any]) -> dict[str, Any]:
    """req: sha, build_id, device_ids, track_dir, optional pr, tracks, limits (staging.DEFAULTS'
    keys; ``stable_minutes`` defaults to HEALTH_MINUTES here)."""
    from ..api.routes_firmware import _store_dev_hex
    from ..firmware.fleet_workflows import flash_devices

    sha, build_id, device_ids = req["sha"], req["build_id"], list(req["device_ids"])
    tracks = list(req.get("tracks") or DEFAULT_TRACKS)
    limits = {**staging.DEFAULTS, "stable_minutes": HEALTH_MINUTES, **(req.get("limits") or {})}
    report: dict[str, Any] = {
        "sha": sha,
        "build_id": build_id,
        "pr": req.get("pr"),
        "devices": device_ids,
        "limits": limits,
        "tracks": tracks,
        "gates": list(GATES),
    }
    async with reserve(device_ids):
        report["corpus"] = await corpus_pin(req["track_dir"])
        jobs.report(0, "deploying the dev build")
        deploy = await deploy_pipeline.run_deploy(
            _fleet(),
            jobs.Progress(),
            release_tag=None,
            device_ids=device_ids,
            download_dir=Path(firmware_dir() / "dev-download"),
            store_firmware=_store_dev_hex,
            flash=flash_devices,
            dev_build=build_id,
        )
        ok = deploy.get("status") == "ok"
        if ok and deploy.get("sha") != sha:
            # The route checked the build's commit; this is the package that was flashed.
            ok = False
            deploy = {
                **deploy,
                "failed_step": "release",
                "message": f"build {build_id} is of {str(deploy.get('sha'))[:12]}, not {sha[:12]}",
            }
        report["deploy"] = {**deploy, "pass": ok}
        if not ok:
            report["passed"] = False
            jobs.report(100, "FAILED")
            return report
        version = deploy["version"]
        report["version"] = version
        start = await staging.uptimes(device_ids, float(deploy["steps"]["store"]["flash_t"]))
        async with lit(device_ids, needs_light=False, purpose="the PR smoke test"):
            jobs.report(10, f"health: {limits['stable_minutes']:g} min of json info")
            report["health"] = await staging.health(device_ids, limits)
            jobs.report(40, "info")
            report["info"] = await dev_run.info(device_ids)
            jobs.report(45, f"tracks: {', '.join(tracks)}")
            await dev_run.restore_settings(device_ids)  # runtime defaults, as a test session
            report["validation"] = await dev_run._check(
                device_ids, {"kind": "validate", "tracks": tracks}, req["track_dir"], limits
            )
            jobs.report(95, "final asserts")
            report["final"] = await staging.final_state(device_ids, version, start)
    report["passed"] = verdict(report)
    jobs.report(100, "passed" if report["passed"] else "FAILED")
    return report
