"""Durable work survives a restart at any moment (#325).

The real ``test.validate`` workflow runs in its own process (tests/durable_harness.py), is
killed mid-step or drained mid-step as SIGTERM does it, and is started again. Its result must
equal an uninterrupted run's, with every run played exactly once, except the one a crash cut
off, which plays again from its start (a step runs at least once). Device setup must run again
after the restart, and a drain must let the run in flight finish rather than repeat it.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from blinky_server.work import durable

SERVER = Path(__file__).resolve().parents[1]
RUNS = [(t, r) for t in ("alpha", "bravo", "charlie") for r in (1, 2)]  # the harness's job


def harness(out: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "XDG_DATA_HOME": str(out / "data")}
    return subprocess.run(
        [sys.executable, "-m", "tests.durable_harness", *args[:1], str(out), *args[1:]],
        cwd=SERVER,
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )


def effects(out: Path) -> list[str]:
    return (out / "effects.log").read_text().splitlines()


def plays(out: Path) -> list[str]:
    return [line for line in effects(out) if line.startswith("play ")]


def result(r: subprocess.CompletedProcess[str]) -> list[tuple[str, int]]:
    assert r.returncode == 0, r.stderr[-3000:]
    res = json.loads(r.stdout.strip().splitlines()[-1])
    assert res["status"] == "ok" and res["tracks"] == 3 and res["runs"] == 2
    return [(x["track"], x["run"]) for x in res["results"]]


def started_and_ended(out: Path) -> list[str]:
    """Each run's playback as `track start` / `track end`, in order."""
    return [line.removeprefix("play ") for line in plays(out)]


def once_each() -> list[str]:
    return [f"{t} {e}" for t, _ in RUNS for e in ("start", "end")]


def test_uninterrupted(tmp_path: Path) -> None:
    assert result(harness(tmp_path, "start", "none")) == RUNS
    assert started_and_ended(tmp_path) == once_each()


def test_crash_mid_step_reruns_only_that_step(tmp_path: Path) -> None:
    r = harness(tmp_path, "start", "crash:3")
    assert r.returncode == 9  # died inside the third run (bravo run 1)
    assert result(harness(tmp_path, "resume")) == RUNS
    # Runs 1-2 once; run 3 started, was cut off, and played again in full; the rest once.
    want = once_each()
    want.insert(4, "bravo start")
    assert started_and_ended(tmp_path) == want
    # The session ran again after the restart: runtime defaults restored, streaming restarted.
    assert sum(line == "dev-a cmd restore_runtime_settings" for line in effects(tmp_path)) == 2
    assert sum(line == "dev-a stream fast" for line in effects(tmp_path)) == 2


def test_drain_mid_step_finishes_it_then_resumes_after(tmp_path: Path) -> None:
    r = harness(tmp_path, "start", "drain:3")
    assert r.returncode == 0, r.stderr[-3000:]
    log = effects(tmp_path)
    # The run in flight finished before the drain completed; nothing after it started.
    assert log.index("play bravo end") < log.index("drained")
    assert [p for p in plays(tmp_path)] == [f"play {x}" for x in once_each()[:6]]
    assert any(line.startswith("status draining: waiting for") for line in log)
    # The held workflow's cleanup ran before exit, with the devices still there.
    assert log.index("dev-a stream off") < log.index("exited")
    assert result(harness(tmp_path, "resume")) == RUNS
    assert started_and_ended(tmp_path) == once_each()  # nothing played twice


@pytest.mark.asyncio
async def test_submit_is_refused_while_draining_or_not_running(monkeypatch) -> None:
    async def wf() -> None:
        return None

    monkeypatch.setattr(durable, "_launched", False)
    with pytest.raises(durable.NotRunning):
        await durable.submit(wf)
    monkeypatch.setattr(durable, "_launched", True)
    monkeypatch.setattr(durable, "_draining", True)
    with pytest.raises(durable.Draining):
        await durable.submit(wf)


@pytest.mark.asyncio
async def test_drain_waits_for_steps_in_flight_and_reports_them(monkeypatch) -> None:
    monkeypatch.setattr(durable, "_draining", False)
    monkeypatch.setattr(durable, "DRAIN_TICK_S", 0.01)
    monkeypatch.setattr(
        durable, "_in_flight", {1: {"workflow_id": "w1", "step": "m.run", "since": 0.0}}
    )
    status: list[str] = []
    extends: list[float] = []

    async def finish() -> None:
        import asyncio

        await asyncio.sleep(0.05)
        durable._in_flight.clear()

    import asyncio

    t = asyncio.create_task(finish())
    assert await durable.drain(status.append, extends.append)
    await t
    assert status[0].startswith("draining: waiting for w1 run") and status[-1].startswith("drained")
    assert extends and all(e == durable.DRAIN_EXTEND_S for e in extends)


@pytest.mark.asyncio
async def test_drain_gives_up_on_a_hung_step_loudly(monkeypatch, caplog) -> None:
    monkeypatch.setattr(durable, "_draining", False)
    monkeypatch.setattr(durable, "DRAIN_MAX_S", 0)
    monkeypatch.setattr(
        durable, "_in_flight", {1: {"workflow_id": "w1", "step": "m.run", "since": 0.0}}
    )
    assert not await durable.drain(lambda s: None, lambda s: None)
    assert "drain gave up" in caplog.text


@pytest.mark.asyncio
async def test_routes_answer_503_without_the_work_store(api_client) -> None:
    r = await api_client.post("/api/test/validate", json={"device_ids": ["x"], "track_dir": "/tmp"})
    assert r.status_code == 503
    assert (await api_client.get("/api/test/jobs")).status_code == 503


def test_recovered_job_waits_for_its_devices_to_reconnect(tmp_path: Path) -> None:
    """Recovery runs as soon as the store launches, before discovery has connected anything.
    The job must wait for its devices, not play to none and record empty scores for good."""
    assert harness(tmp_path, "start", "crash:3").returncode == 9
    r = harness(tmp_path, "resume", "late")
    assert result(r) == RUNS
    res = json.loads(r.stdout.strip().splitlines()[-1])
    assert all(set(x["scores"]) == {"dev-a", "dev-b"} for x in res["results"]), res["results"]
    log = effects(tmp_path)
    # Nothing was configured or played after the restart until the devices were back.
    after = log[log.index("play bravo start") + 1 :]
    first_connect = after.index("dev-a connected")
    assert not any(x.startswith(("play ", "dev-a cmd")) for x in after[:first_connect])
