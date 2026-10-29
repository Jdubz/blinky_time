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
from typing import Any

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


def test_drain_holds_a_job_waiting_for_the_audio_lock(tmp_path: Path) -> None:
    """Review: a second job waiting on the audio lock is past every boundary. When the drain
    stops the first, the second must not set up its devices while the server shuts down."""
    r = harness(tmp_path, "start", "two-drain:2")
    assert r.returncode == 0, r.stderr[-3000:]
    log = effects(tmp_path)
    before_exit = log[: log.index("exited")]
    # B touched no device: only A's session restored the defaults before the exit.
    assert before_exit.count("dev-a cmd restore_runtime_settings") == 1
    assert "dev-a cmd set x 2" not in before_exit
    r = harness(tmp_path, "resume")
    assert r.returncode == 0, r.stderr[-3000:]
    a, b = (json.loads(x) for x in r.stdout.strip().splitlines()[-2:])
    for res in (a, b):
        assert res["status"] == "ok"
        assert [(x["track"], x["run"]) for x in res["results"]] == RUNS
    assert "dev-a cmd set x 2" in effects(tmp_path)  # B ran after the restart


@pytest.mark.asyncio
async def test_work_status_route(api_client, monkeypatch) -> None:
    monkeypatch.setattr(
        durable, "_in_flight", {1: {"workflow_id": "w1", "step": "m.run", "since": 0.0}}
    )
    r = await api_client.get("/api/work")
    assert r.status_code == 200
    body = r.json()
    assert body["draining"] is False and body["in_flight"][0]["workflow_id"] == "w1"


def test_drain_during_session_setup_waits_for_it(tmp_path: Path) -> None:
    """Review: the session's device setup runs outside any step. A drain starting in the middle
    of it must wait for it (else the fleet stops under it and the job fails for good), then
    hold the job before its first run, which happens after the restart."""
    r = harness(tmp_path, "start", "drain-setup")
    assert r.returncode == 0, r.stderr[-3000:]
    log = effects(tmp_path)
    assert log.index("dev-b stream fast") < log.index("drained")  # setup finished first
    assert not any(x.startswith("play ") for x in log)  # nothing played before the restart
    assert result(harness(tmp_path, "resume")) == RUNS


def test_progress_table_keeps_only_the_newest(monkeypatch) -> None:
    from types import SimpleNamespace

    from blinky_server.work import jobs

    monkeypatch.setattr(jobs, "_progress", {})
    monkeypatch.setattr(jobs, "PROGRESS_KEEP", 3)
    for i in range(5):
        monkeypatch.setattr(jobs, "DBOS", SimpleNamespace(workflow_id=f"w{i}"))
        jobs.report(i, "x")
    assert list(jobs._progress) == ["w2", "w3", "w4"]


def test_a_schema_bump_retires_unfinished_workflows_loudly(tmp_path: Path) -> None:
    """Review: DBOS never recovers a workflow of another SCHEMA_VERSION, so it would stay
    "running" for good (blocking a new fleet deploy, catching a retried Spark request).
    The next start cancels it and says why."""
    assert harness(tmp_path, "start", "crash:3").returncode == 9
    r = harness(tmp_path, "resume", "newschema")
    assert r.returncode == 0, r.stderr[-3000:]
    st = json.loads(r.stdout.strip().splitlines()[-1])
    assert st["status"] == "CANCELLED" and st["app_version"] == "blinky-work-1"
    assert "can't resume on this code" in r.stderr
    assert sum(x == "play bravo start" for x in effects(tmp_path)) == 1  # not run again


@pytest.mark.parametrize("workflow", ["param-sweep", "tune-threshold", "capture-nn"])
def test_every_test_job_resumes_after_a_crash_mid_unit(tmp_path: Path, workflow: str) -> None:
    """The other test jobs, as validate above: crashed in the middle of a unit, then started
    again, they finish with the same units as an uninterrupted run, only the cut-off unit
    repeated."""

    def run(out: Path, *args: str) -> subprocess.CompletedProcess[str]:
        env = {**os.environ, "XDG_DATA_HOME": str(out / "data"), "WORKFLOW": workflow}
        return subprocess.run(
            [sys.executable, "-m", "tests.durable_harness", args[0], str(out), *args[1:]],
            cwd=SERVER,
            env=env,
            capture_output=True,
            text=True,
            timeout=300,
        )

    clean, cut = tmp_path / "clean", tmp_path / "cut"
    clean.mkdir()
    cut.mkdir()
    r = run(clean, "start", "none")
    assert r.returncode == 0, r.stderr[-3000:]

    def outcome(r: subprocess.CompletedProcess[str]) -> dict:
        res = json.loads(r.stdout.strip().splitlines()[-1])
        res.pop("output_path", None)  # each run writes its capture in its own dir
        return res

    want_result = outcome(r)
    want = plays(clean)
    crash_at = 1 if workflow == "capture-nn" else 2
    assert run(cut, "start", f"crash:{crash_at}").returncode == 9
    r = run(cut, "resume")
    assert r.returncode == 0, r.stderr[-3000:]
    assert outcome(r) == want_result
    starts = [i for i, x in enumerate(want) if x.endswith(" start")]
    expected = want[: starts[crash_at - 1] + 1] + want[starts[crash_at - 1] :]
    assert plays(cut) == expected


# Each durable workflow and the test that kills it mid-unit and checks it resumes (#325). A new
# workflow without an entry here fails CI: durable work is only durable once a restart is shown.
RESTART_TESTS = {
    "test.validate": "test_durable_work.py::test_crash_mid_step_reruns_only_that_step",
    "test.param-sweep": "test_durable_work.py::test_every_test_job_resumes_after_a_crash_mid_unit",
    "test.tune-threshold": "test_durable_work.py::test_every_test_job_resumes_after_a_crash_mid_unit",
    "test.capture-nn": "test_durable_work.py::test_every_test_job_resumes_after_a_crash_mid_unit",
    "fleet.flash": "test_durable_flash.py::test_killed_before_the_bytes_land_reflashes_that_device_only",
    "fleet.deploy": "test_durable_flash.py::test_a_deploy_cut_off_mid_flash_resumes_with_that_device",
    "spark.separate": "test_durable_spark.py::"
    "test_a_separate_cut_off_by_a_restart_resumes_when_asked_again",
    "spark.train": "test_durable_spark.py::"
    "test_a_train_submission_cut_off_by_a_restart_resumes_when_asked_again",
}


def test_every_workflow_has_a_restart_test() -> None:
    import importlib

    # Every module that declares a workflow, found by its source, so none is missed.
    pkg = Path(durable.__file__).resolve().parents[1]
    for src in pkg.rglob("*.py"):
        if "@durable.workflow(" in src.read_text():
            rel = src.relative_to(pkg.parent).with_suffix("")
            importlib.import_module(".".join(rel.parts))
    assert set(durable.WORKFLOWS) == set(RESTART_TESTS), (
        f"workflows without a restart test: {sorted(set(durable.WORKFLOWS) - set(RESTART_TESTS))}"
        f"; entries for no workflow: {sorted(set(RESTART_TESTS) - set(durable.WORKFLOWS))}"
    )
    here = Path(__file__).parent
    for name, ref in RESTART_TESTS.items():
        file, fn = ref.split("::")
        assert f"def {fn}(" in (here / file).read_text(), f"{name}: {ref} doesn't exist"


@pytest.mark.asyncio
async def test_per_device_commands_add_to_the_shared_ones(monkeypatch: pytest.MonkeyPatch) -> None:
    """#327: a device with its own commands gets one reset, then the shared commands, then its
    own; a second reset would undo the shared ones."""
    from types import SimpleNamespace

    from blinky_server.testing import workflows as wf

    sent: dict[str, list[str]] = {"a": [], "b": []}

    def device(dev: str) -> SimpleNamespace:
        async def send_command(cmd: str) -> str:
            sent[dev].append(cmd)
            return "{}"

        return SimpleNamespace(id=dev, protocol=SimpleNamespace(send_command=send_command))

    async def nothing(*a: Any) -> None:
        return None

    async def connected(ids: list[str]) -> list[Any]:
        return [device(d) for d in ids]

    monkeypatch.setattr(wf, "jobs", SimpleNamespace(report=lambda *a: None))
    monkeypatch.setattr(wf, "acquire_audio_lock", lambda ids: True)
    monkeypatch.setattr(wf, "release_audio_lock", lambda: None)
    monkeypatch.setattr(wf, "kill_orphan_audio", nothing)
    monkeypatch.setattr(wf, "stop_audio", nothing)
    monkeypatch.setattr(wf, "_start_streaming", nothing)
    monkeypatch.setattr(wf, "_stop_streaming", nothing)
    monkeypatch.setattr(wf, "_connected", connected)
    async with wf.session(["a", "b"], ["set x 1"], {"b": ["set y 2"]}):
        pass
    assert sent == {
        "a": ["restore_runtime_settings", "set x 1"],
        "b": ["restore_runtime_settings", "set x 1", "set y 2"],
    }
