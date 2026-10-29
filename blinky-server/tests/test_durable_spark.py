"""Spark submissions survive a restart at any moment (#325).

- ``spark.separate`` is a durable workflow, one step per track: killed mid-track, the same
  request sent again attaches to the resumed workflow; tracks already submitted aren't
  submitted again, and the one cut off is sent again (the ledger makes that a skip or reattach).
- The corpus pass's loop starts no tick while draining, and a tick in progress holds the drain.
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import threading
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from blinky_server.spark import service as spark
from blinky_server.spark import workflows
from blinky_server.work import durable

SERVER = Path(__file__).resolve().parents[1]


def harness(out: Path, *args: str) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, "XDG_DATA_HOME": str(out / "data")}
    return subprocess.run(
        [sys.executable, "-m", "tests.durable_spark_harness", args[0], str(out), *args[1:]],
        cwd=SERVER,
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )


def submits(out: Path) -> list[str]:
    return (out / "effects.log").read_text().splitlines()


def test_a_separate_cut_off_by_a_restart_resumes_when_asked_again(tmp_path: Path) -> None:
    assert harness(tmp_path, "start", "crash:3").returncode == 9
    r = harness(tmp_path, "resume")
    assert r.returncode == 0, r.stderr[-3000:]
    res = json.loads(r.stdout.strip().splitlines()[-1])
    assert [Path(x["path"]).name for x in res] == ["a.mp3", "b.mp3", "c.mp3", "d.mp3"]
    assert submits(tmp_path) == [
        "submit a.mp3 start",
        "submit a.mp3 end",
        "submit b.mp3 start",
        "submit b.mp3 end",
        "submit c.mp3 start",  # cut off
        "submit c.mp3 start",
        "submit c.mp3 end",
        "submit d.mp3 start",
        "submit d.mp3 end",
    ]


@pytest.mark.asyncio
async def test_the_same_request_attaches_to_its_pending_workflow(
    work_store: None, monkeypatch: pytest.MonkeyPatch
) -> None:
    from dbos import DBOS

    release = threading.Event()
    calls: list[str] = []

    def separate_one(path: str, sha: str, *a: Any) -> dict:
        calls.append(path)
        release.wait(10)
        return {"path": path, "action": "submitted"}

    fake = SimpleNamespace(_audio_path=lambda f: Path(f), separate_one=separate_one)
    monkeypatch.setattr(workflows, "_svc", lambda: fake)
    monkeypatch.setattr(spark, "sha256_file", lambda p: Path(p).stem)
    args = (["/m/x.mp3"], "htdemucs", ["drums"], None)
    first = asyncio.create_task(workflows.run("separate", workflows.separate, *args))
    for _ in range(100):
        if calls:
            break
        await asyncio.sleep(0.02)
    second = asyncio.create_task(workflows.run("separate", workflows.separate, *args))
    await asyncio.sleep(0.2)
    release.set()
    a, b = await asyncio.gather(first, second)
    assert a == b and calls == ["/m/x.mp3"]  # one workflow, one submission
    key = workflows.request_key("separate", args)
    assert len(await DBOS.list_workflows_async(workflow_id_prefix=key)) == 1


@pytest.mark.asyncio
async def test_corpus_loop_holds_the_drain_and_starts_no_tick_while_draining(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    seen: list[list[str]] = []

    def tick() -> None:
        seen.append([s["step"] for s in durable.in_flight()])

    svc = SimpleNamespace(corpus=SimpleNamespace(tick=tick), reconcile=lambda: {})
    monkeypatch.setattr(spark, "CORPUS_TICK_S", 0.01)
    monkeypatch.setattr(spark, "RECONCILE_INTERVAL_S", 3600)
    monkeypatch.setattr(durable, "_draining", True)
    loop = asyncio.create_task(spark.SparkService.run_forever(svc))  # type: ignore[arg-type]
    await asyncio.sleep(0.1)
    assert seen == []  # draining: no tick started
    monkeypatch.setattr(durable, "_draining", False)
    await asyncio.sleep(0.1)
    loop.cancel()
    assert seen and all(s == ["spark corpus tick"] for s in seen)  # each tick held the drain


def test_a_torn_ledger_tail_is_dropped_loudly_and_cut_before_the_next_append(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """A crash or power cut can leave the last append half-written. That one line is not a
    record (the rest of the ledger still loads), and the next append mustn't join it."""
    from blinky_server.spark.client import SparkError
    from blinky_server.spark.ledger import Ledger

    path = tmp_path / "separate.jsonl"
    good = json.dumps({"idempotency_key": "k1", "job_id": "j1"}, sort_keys=True)
    path.write_text(good + "\n" + '{"idempotency_key": "k2", "jo')
    ledger = Ledger(path)
    assert [r["job_id"] for r in ledger.rows()] == ["j1"]
    assert "torn final line" in caplog.text
    ledger.append({"idempotency_key": "k3", "job_id": "j3"})
    assert [r["job_id"] for r in ledger.rows()] == ["j1", "j3"]
    # Corruption anywhere else is still an error, not skipped.
    path.write_text('{"broken\n' + good + "\n")
    with pytest.raises(SparkError, match="corrupt ledger line"):
        ledger.rows()


@pytest.mark.asyncio
async def test_reconcile_is_held_and_skipped_while_draining(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    held: list[list[str]] = []

    def reconcile() -> dict:
        held.append([s["step"] for s in durable.in_flight()])
        return {}

    svc = SimpleNamespace(corpus=SimpleNamespace(tick=lambda: None), reconcile=reconcile)
    monkeypatch.setattr(spark, "CORPUS_TICK_S", 0.01)
    monkeypatch.setattr(spark, "RECONCILE_INTERVAL_S", 0)
    monkeypatch.setattr(durable, "_draining", True)
    loop = asyncio.create_task(spark.SparkService.run_forever(svc))  # type: ignore[arg-type]
    await asyncio.sleep(0.05)
    assert held == []
    monkeypatch.setattr(durable, "_draining", False)
    await asyncio.sleep(0.05)
    loop.cancel()
    assert held and all(h == ["spark reconcile"] for h in held)
