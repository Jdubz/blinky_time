"""Flashing survives a restart at any moment (#325).

- A fleet flash is a durable workflow, one step per device: killed mid-device, it resumes with
  that device and goes on to the rest; devices already done are never flashed again; a device
  whose bytes landed before the kill isn't flashed a second time (check-then-act).
- A flash cut off by a restart leaves a record: the next start appends an ABANDONED entry, so
  auto-recovery's dedup window (and the UF2 cascade guard, if bytes landed) covers it.
- While the server drains, no flash starts, and one already started holds the drain.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

from blinky_server.device.manager import FleetManager
from blinky_server.work import durable

SERVER = Path(__file__).resolve().parents[1]
ALL = ["dev-a", "dev-b", "dev-c"]


def harness(out: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "XDG_DATA_HOME": str(out / "data")}
    return subprocess.run(
        [sys.executable, "-m", "tests.durable_fleet_harness", args[0], str(out), *args[1:]],
        cwd=SERVER,
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )


def flashes(out: Path) -> list[str]:
    return [x for x in (out / "effects.log").read_text().splitlines() if x.startswith("flash ")]


def result(r: subprocess.CompletedProcess[str]) -> dict:
    assert r.returncode == 0, r.stderr[-3000:]
    return json.loads(r.stdout.strip().splitlines()[-1])


def test_uninterrupted_fleet_flash(tmp_path: Path) -> None:
    res = result(harness(tmp_path, "start", "none"))
    assert res["status"] == "ok" and set(res["per_device"]) == set(ALL)
    assert flashes(tmp_path) == [f"flash {d} {e}" for d in ALL for e in ("start", "end")]


def test_killed_before_the_bytes_land_reflashes_that_device_only(tmp_path: Path) -> None:
    assert harness(tmp_path, "start", "before:2").returncode == 9
    res = result(harness(tmp_path, "resume"))
    assert res["status"] == "ok" and all(v["version"] == "b300" for v in res["per_device"].values())
    assert flashes(tmp_path) == [
        "flash dev-a start",
        "flash dev-a end",
        "flash dev-b start",  # killed here, nothing written
        "flash dev-b start",
        "flash dev-b end",
        "flash dev-c start",
        "flash dev-c end",
    ]


def test_killed_after_the_bytes_land_does_not_flash_twice(tmp_path: Path) -> None:
    assert harness(tmp_path, "start", "after:2").returncode == 9
    res = result(harness(tmp_path, "resume"))
    assert res["status"] == "ok"
    # dev-b already reports the new version after the restart: checked, not flashed again.
    assert flashes(tmp_path) == [
        "flash dev-a start",
        "flash dev-a end",
        "flash dev-b start",
        "flash dev-c start",
        "flash dev-c end",
    ]


def test_resumed_step_waits_for_the_version_before_deciding(tmp_path: Path) -> None:
    """Review: after a restart discovery registers devices before they connect (serial) or
    with no version (BLE). The resumed step must wait for the version, not flash again."""
    assert harness(tmp_path, "start", "after:2").returncode == 9
    res = result(harness(tmp_path, "resume", "late"))
    assert res["status"] == "ok"
    assert flashes(tmp_path) == [
        "flash dev-a start",
        "flash dev-a end",
        "flash dev-b start",
        "flash dev-c start",
        "flash dev-c end",
    ]


def test_resumed_step_never_flashes_blind(tmp_path: Path) -> None:
    """A device that doesn't come back with a version after the restart isn't flashed again:
    it fails loudly, and the rest of the fleet carries on."""
    assert harness(tmp_path, "start", "after:2").returncode == 9
    res = result(harness(tmp_path, "resume", "never"))
    assert res["status"] == "error"
    assert res["per_device"]["dev-b"]["flash"] == "error"
    assert "interrupted by a restart" in res["per_device"]["dev-b"]["message"]
    assert res["per_device"]["dev-c"]["flash"] == "ok"
    assert flashes(tmp_path).count("flash dev-b start") == 1


# -- the audit log across a restart -----------------------------------------------------------


def _write(path: Path, *entries: dict) -> None:
    with path.open("a") as f:
        for e in entries:
            f.write(json.dumps(e) + "\n")


def test_a_flash_cut_off_by_a_restart_is_recorded_abandoned() -> None:
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    log = fleet._flash_audit_log_path()
    now = time.time()
    base = {
        "job_id": "j1",
        "device_id": "dev-x",
        "canonical_id": "dev-x",
        "transport": "uf2",
        "is_auto_recovery": True,
        "firmware_path": "/f.hex",
        "expected_version": "b300",
    }
    _write(
        log,
        {**base, "event": "started", "at": now - 30},
        {**base, "event": "written", "at": now - 20},
        # a finished job alongside: no second terminal line for it
        {"job_id": "j2", "device_id": "dev-y", "event": "started", "at": now - 60},
        {
            "job_id": "j2",
            "device_id": "dev-y",
            "canonical_id": "dev-y",
            "state": "completed",
            "finished_at": now - 50,
            "transport": "uf2",
            "write_completed_at": now - 55,
        },
    )

    added = fleet._abandon_interrupted_flashes()
    assert [e["job_id"] for e in added] == ["j1"]
    assert added[0]["state"] == "abandoned" and "restart" in added[0]["error"]
    assert added[0]["write_completed_at"] == pytest.approx(now - 20)
    assert fleet._abandon_interrupted_flashes() == []  # recorded once

    fleet._load_recent_flash_attempts_from_audit_log()
    assert "dev-x" in fleet._recent_flash_attempts  # dedup window runs from this start
    assert fleet._recent_uf2_writes["dev-x"] == pytest.approx(now - 20)  # cascade guard
    assert not fleet.should_attempt_auto_recovery("dev-x")


# -- drain ------------------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_no_flash_starts_while_draining(monkeypatch: pytest.MonkeyPatch) -> None:
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    from blinky_server.device.device import Device, DeviceState

    from .mock_transport import MockTransport

    dev = Device(device_id="dev-1", port="/dev/x", platform="nrf52840", transport=MockTransport())
    dev.state = DeviceState.CONNECTED
    fleet._devices[dev.id] = dev
    monkeypatch.setattr(durable, "_draining", True)
    with pytest.raises(durable.Draining):
        await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    assert await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=False) is None
    assert not fleet._flash_jobs


@pytest.mark.asyncio
async def test_a_flash_holds_the_drain_from_the_moment_it_is_created(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import asyncio

    fleet = FleetManager(enable_ble=False, enable_serial=False)
    from blinky_server.device.device import Device, DeviceState

    from .mock_transport import MockTransport

    dev = Device(device_id="dev-1", port="/dev/x", platform="nrf52840", transport=MockTransport())
    dev.state = DeviceState.CONNECTED
    fleet._devices[dev.id] = dev
    release = asyncio.Event()

    async def slow(job: object) -> None:
        await release.wait()

    monkeypatch.setattr(fleet, "_run_flash_job", slow)
    monkeypatch.setattr(durable, "_draining", False)
    await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    # Before the task has even run, the drain already sees the flash.
    assert [s["step"] for s in durable.in_flight()] == ["flash dev-1"]
    release.set()
    for _ in range(50):
        if not durable.in_flight():
            break
        await asyncio.sleep(0.01)
    assert durable.in_flight() == []
