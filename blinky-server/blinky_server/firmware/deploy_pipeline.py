"""Server-side fleet deploy: the same hardened pipeline as scripts/deploy.sh.

The rule this enforces isn't "only deploy.sh may deploy". It's "every
deploy runs deploy.sh's safety mechanisms" (CLAUDE.md, Upload Safety). Each
step below reuses the module deploy.sh relies on, and none can be skipped:

1. Firmware: ``tools/firmware_release.py download`` (the exact command and
   JSON contract deploy.sh uses): a STABLE release only, every asset checked
   against the release manifest's sha256. The release's hex/UF2 passed the
   strict safety validators when it was packaged. Never a local build (#269).
2. Store it as the server's current firmware (the same storage
   /api/fleet/upload writes, so auto-recovery and /api/fleet/firmware see it).
3. Targets: an explicit list, or "all" = every nRF52840 that is connected or
   present (deploy.sh's rule). Unknown or unflashable IDs fail loudly.
   /api/fleet/flash uses the same resolver.
4. Flash through ``FleetManager.flash_fleet`` (the single flash entry point),
   with auto-recovery armed for exactly these targets.
5. ``restore_runtime_settings`` then ``save`` over the fleet channels, judged
   by deploy.sh's rule.
6. Post-deploy assertion (port of deploy.sh step 7): each target in a good
   state; connected ones report exactly the release version and expose the
   audio overrun counters.

Any failed step stops the deploy and is reported with its details.
"""

from __future__ import annotations

import asyncio
import json
import logging
import subprocess
import sys
from collections.abc import Awaitable, Callable, Sequence
from pathlib import Path
from typing import Any

from ..device.device import DeviceState
from ..work import durable
from .compile import find_project_root

log = logging.getLogger(__name__)

# deploy.sh's "all": flashable nRF52840s in range (BLE devices sit at PRESENT).
ALL_STATES = (DeviceState.CONNECTED, DeviceState.PRESENT)
# An explicit list may also name a device stuck in its bootloader.
FLASHABLE_STATES = (DeviceState.CONNECTED, DeviceState.DFU_RECOVERY, DeviceState.PRESENT)
# deploy.sh LOOP_METRICS_SETTLE_S: let the 5 s fps window close before reading it.
LOOP_METRICS_SETTLE_S = 6.0
RELEASE_DOWNLOAD_TIMEOUT_S = 300.0
# deploy.sh: one json-info retry after a short pause (GATT teardown, connect race).
JSON_INFO_RETRY_S = 2.0


class DeployError(Exception):
    """A deploy step failed. ``step`` names it; the message says why."""

    def __init__(self, step: str, message: str, detail: Any = None) -> None:
        super().__init__(f"{step}: {message}")
        self.step = step
        self.message = message
        self.detail = detail

    def __reduce__(self) -> tuple[Any, ...]:
        # DBOS records a step's exception and raises it again on replay, which needs pickle,
        # and the default reduction would call __init__ with the formatted message only.
        return (DeployError, (self.step, self.message, self.detail))


# ── 1. Firmware: a stable release, downloaded and verified ─────────────


def fetch_stable_release(tag: str | None, out_dir: Path) -> dict[str, Any]:
    """Run ``tools/firmware_release.py download`` exactly as deploy.sh does.

    Returns its JSON (tag, stable, hex, uf2, version). Refuses anything that
    isn't a promoted stable release.
    """
    root = find_project_root()
    args = [
        sys.executable,
        str(root / "tools" / "firmware_release.py"),
        "download",
        "--out",
        str(out_dir),
    ]
    if tag:
        args += ["--tag", tag]
    try:
        r = subprocess.run(
            args, cwd=root, capture_output=True, text=True, timeout=RELEASE_DOWNLOAD_TIMEOUT_S
        )
    except subprocess.TimeoutExpired as e:
        raise DeployError(
            "release", f"download timed out after {RELEASE_DOWNLOAD_TIMEOUT_S:.0f}s"
        ) from e
    if r.returncode != 0:
        msg = (r.stderr or r.stdout).strip().splitlines()
        raise DeployError(
            "release", msg[-1] if msg else f"firmware_release.py exited {r.returncode}"
        )
    try:
        rel: dict[str, Any] = json.loads(r.stdout)
    except json.JSONDecodeError as e:
        raise DeployError(
            "release", f"firmware_release.py printed no JSON: {r.stdout[:200]!r}"
        ) from e
    if rel.get("stable") is not True:
        raise DeployError(
            "release",
            f"{rel.get('tag')} is not a stable release. Fleet deploys install stable releases only; "
            "gate a candidate with tools/release_gate.py on a test device (docs/RELEASE_PROCESS.md)",
        )
    for key in ("tag", "hex", "version"):
        if key not in rel:
            raise DeployError("release", f"firmware_release.py output has no {key!r}")
    if not Path(rel["hex"]).is_file():
        raise DeployError("release", f"verified hex missing: {rel['hex']}")
    return rel


# ── 3. Targets ──────────────────────────────────────────────────────────


def resolve_targets(fleet: Any, device_ids: Sequence[str] | str) -> list[Any]:
    """Resolve an explicit target list, or "all", to devices. Fails loudly.

    "all" = every nRF52840 that is connected or present (deploy.sh). A list
    must name only known, flashable nRF52840s: a typo or a stale ID is an
    error, never silently dropped (the operator would think they deployed
    to N devices and actually deployed to N-1).
    """
    if isinstance(device_ids, str):
        if device_ids != "all":
            raise DeployError(
                "targets", f'device_ids must be a list of IDs or "all", got {device_ids!r}'
            )
        devices = [
            d for d in fleet.get_all_devices() if d.platform == "nrf52840" and d.state in ALL_STATES
        ]
        if not devices:
            raise DeployError("targets", 'no connected or present nRF52840 devices for "all"')
        return devices

    if not device_ids:
        raise DeployError("targets", 'empty device_ids: name the devices, or pass "all"')
    devices, missing, wrong_state = [], [], []
    for device_id in dict.fromkeys(device_ids):  # de-duplicate, keep order
        d = fleet.get_device(device_id)
        if d is None:
            missing.append(device_id)
        elif d.platform != "nrf52840" or d.state not in FLASHABLE_STATES:
            wrong_state.append(f"{device_id}({d.platform},{d.state.value})")
        else:
            devices.append(d)
    if missing or wrong_state:
        raise DeployError(
            "targets",
            "device_ids whitelist had bad entries — "
            f"unknown: {missing or '[]'}, not flashable: {wrong_state or '[]'}",
        )
    return devices


# ── 5. Restore runtime settings + save ─────────────────────────────────


def broadcast_failures(results: dict[str, str]) -> list[str]:
    """deploy.sh's rule for a fleet command's per-source results.

    "OK..." = executed. "skipped: ..." = not directly connected (BLE at
    PRESENT); the broadcaster still delivered it, so informational. Anything
    else is a failure, including a broadcaster that didn't send.
    """
    fails = []
    for source, resp in results.items():
        text = str(resp).strip()
        if text.startswith("OK") or text.startswith("skipped:"):
            continue
        fails.append(f"{source[:12]}: {text.splitlines()[0][:80] if text else '(empty)'}")
    return fails


async def restore_and_save(fleet: Any) -> dict[str, Any]:
    """restore_runtime_settings, then save: deploy.sh step 6."""
    out: dict[str, Any] = {}
    for command in ("restore_runtime_settings", "save"):
        results = await fleet.broadcast(command)
        out[command] = results
        fails = broadcast_failures(results)
        if fails:
            raise DeployError(
                "settings",
                f"{command} did not land on every device: {'; '.join(fails)}",
                detail=out,
            )
    return out


# ── 6. Post-deploy assertion ────────────────────────────────────────────


async def _json_info(device: Any) -> dict[str, Any]:
    """``json info`` with one retry after 2 s (deploy.sh)."""
    last: Exception | None = None
    for attempt in range(2):
        try:
            return dict(json.loads(await device.protocol.send_command("json info", timeout=20.0)))
        except Exception as e:
            last = e
            if attempt == 0:
                await asyncio.sleep(JSON_INFO_RETRY_S)
    raise RuntimeError(f"json info unreachable after retry: {last}")


async def assert_post_deploy(
    fleet: Any, target_ids: Sequence[str], expected_version: str
) -> dict[str, Any]:
    """deploy.sh step 7, for the targeted devices only.

    - Gone, or in ERROR / DFU_RECOVERY: fail.
    - Not connected (BLE at PRESENT): OK. The app is advertising, and the
      flash job already verified its version.
    - Connected: ``json info`` must report exactly ``expected_version`` and
      expose the audio overrun counters. Low fps or many overruns are
      warnings, not failures (boot-time stalls are expected).
    """
    rows: dict[str, dict[str, Any]] = {}
    fails: list[str] = []
    for device_id in target_ids:
        short = device_id[:12]
        d = fleet.get_device(device_id)
        if d is None:
            rows[short] = {"ok": False, "reason": "missing (disappeared post-flash)"}
            fails.append(short)
            continue
        if d.state in (DeviceState.ERROR, DeviceState.DFU_RECOVERY):
            rows[short] = {"ok": False, "reason": f"state={d.state.value}"}
            fails.append(short)
            continue
        if d.state != DeviceState.CONNECTED:
            rows[short] = {
                "ok": True,
                "state": d.state.value,
                "note": "version verified by the flash job",
            }
            continue
        try:
            info = await _json_info(d)
        except Exception as e:
            rows[short] = {"ok": False, "reason": str(e)}
            fails.append(short)
            continue
        version = info.get("version")
        overruns = info.get("audioOverruns", -1)
        lost = info.get("audioSamplesLost", -1)
        fps = float(info.get("fps") or 0.0)
        if version != expected_version:
            rows[short] = {"ok": False, "reason": f"version={version} expected={expected_version}"}
            fails.append(short)
            continue
        if overruns is None or lost is None or overruns < 0 or lost < 0:
            rows[short] = {"ok": False, "reason": "missing audio overrun counters in json info"}
            fails.append(short)
            continue
        warn = []
        if fps == 0.0:
            warn.append("fps=0.0 (window unclosed)")
        elif fps < 30:
            warn.append(f"fps={fps:.1f}<30")
        if overruns > 5:
            warn.append(f"overruns={overruns}")
        rows[short] = {
            "ok": True,
            "version": version,
            "fps": fps,
            "overruns": overruns,
            "warn": warn,
        }
    if fails:
        raise DeployError(
            "verify", f"{len(fails)}/{len(target_ids)} device(s) in an unexpected state", rows
        )
    return rows


# ── The pipeline ────────────────────────────────────────────────────────
#
# Each stage that touches the world is a durable step (work/durable.py, #325): run as the
# fleet.deploy workflow (firmware/fleet_workflows.py), a restart resumes the deploy after the
# last finished stage. Outside a workflow (the unit tests) the steps simply run. Each is safe
# to run again: resolving, downloading and storing are idempotent, and restore + save and the
# assertion are what deploy.sh repeats on a re-run.


@durable.step
async def _targets(fleet: Any, device_ids: Sequence[str] | str) -> list[str]:
    return [d.id for d in resolve_targets(fleet, device_ids)]


@durable.step
async def _release(tag: str | None, download_dir: Path) -> dict[str, Any]:
    return await asyncio.to_thread(fetch_stable_release, tag, download_dir)


@durable.step
async def _store_and_arm(
    fleet: Any,
    store_firmware: Callable[[Path, str], Path],
    rel: dict[str, Any],
    target_ids: list[str],
) -> str:
    firmware = store_firmware(Path(rel["hex"]), rel["tag"])
    fleet.set_recovery_firmware(str(firmware), target_ids)
    return str(firmware)


@durable.step
async def _settings(fleet: Any) -> dict[str, Any]:
    return await restore_and_save(fleet)


@durable.step
async def _verify(
    fleet: Any, target_ids: list[str], version: str, settle_s: float
) -> dict[str, Any]:
    await asyncio.sleep(settle_s)
    return await assert_post_deploy(fleet, target_ids, version)


async def run_deploy(
    fleet: Any,
    job: Any,
    *,
    release_tag: str | None,
    device_ids: Sequence[str] | str,
    download_dir: Path,
    store_firmware: Callable[[Path, str], Path],
    flash: Callable[[Any, list[str], Path, Any], Awaitable[dict[str, Any]]],
    settle_s: float = LOOP_METRICS_SETTLE_S,
) -> dict[str, Any]:
    """All six steps. Returns a result dict; any failed step returns
    ``status: error`` with ``failed_step`` and that step's details.

    ``store_firmware`` and ``flash`` are the routes' own storage and
    flash-job helpers, passed in so the deploy writes and flashes exactly
    the way /api/fleet/upload and /api/fleet/flash do.
    """
    steps: dict[str, Any] = {}

    def failed(e: DeployError) -> dict[str, Any]:
        log.error("Fleet deploy failed at %s", e)
        steps[e.step] = {"status": "error", "message": str(e), "detail": e.detail}
        return {"status": "error", "failed_step": e.step, "message": str(e), "steps": steps}

    try:
        job.progress, job.progress_message = 0, "Resolving targets"
        target_ids = await _targets(fleet, device_ids)
        steps["targets"] = {"status": "ok", "devices": target_ids}

        job.progress_message = "Downloading and verifying the stable release"
        rel = await _release(release_tag, download_dir)
        version = rel["version"]
        steps["release"] = {"status": "ok", "tag": rel["tag"], "version": version}

        firmware = Path(await _store_and_arm(fleet, store_firmware, rel, target_ids))
        steps["store"] = {"status": "ok", "firmware_path": str(firmware)}
    except DeployError as e:
        return failed(e)

    flash_result = await flash(fleet, target_ids, firmware, job)
    steps["flash"] = flash_result
    if flash_result.get("status") != "ok":
        return failed(
            DeployError("flash", flash_result.get("message", "flash failed"), flash_result)
        )

    try:
        job.progress_message = "Restoring runtime settings and saving"
        steps["settings"] = {"status": "ok", "results": await _settings(fleet)}

        job.progress_message = "Verifying post-deploy state"
        steps["verify"] = {
            "status": "ok",
            "devices": await _verify(fleet, target_ids, version, settle_s),
        }
    except DeployError as e:
        return failed(e)

    job.progress, job.progress_message = 100, "Deploy complete"
    return {
        "status": "ok",
        "message": f"{version} deployed and verified on {len(target_ids)} device(s)",
        "release": rel["tag"],
        "version": version,
        "steps": steps,
    }
