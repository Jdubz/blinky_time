"""Spark Job API client (#288): the service against a fake Job API, and the routes."""

from __future__ import annotations

import json
import os
import subprocess
import tarfile
import threading
import time
from collections.abc import Iterator
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Any

import pytest
from httpx import ASGITransport, AsyncClient

from blinky_server.api import deps
from blinky_server.api.app import create_app
from blinky_server.spark import bundle
from blinky_server.spark import service as spark
from blinky_server.spark.client import Client, SparkError
from blinky_server.spark.ledger import Ledger

KEY = "test-key"
SECRET = "hook-secret"


class FakeSpark:
    """Just enough of the Job API: artifact upload, idempotent submit, job records, pruning."""

    def __init__(self) -> None:
        self.jobs: dict[str, dict[str, Any]] = {}
        self.by_key: dict[str, str] = {}
        self.uploads = 0
        self.submits: list[dict[str, Any]] = []
        self.created = 0
        fake = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a: Any) -> None:
                pass

            def _send(self, code: int, obj: Any = None) -> None:
                body = b"" if obj is None else json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self) -> None:
                if self.headers.get("X-API-Key") != KEY:
                    self._send(401, {"detail": "no key"})
                    return
                body = self.rfile.read(int(self.headers["Content-Length"]))
                if self.path == "/artifacts":
                    fake.uploads += 1
                    self._send(200, {"uri": f"inputs/u{fake.uploads}/track.mp3"})
                    return
                spec = json.loads(body)
                fake.submits.append(spec)
                k = spec["idempotency_key"]
                live = k in fake.by_key and fake.jobs[fake.by_key[k]]["state"] not in (
                    "failed",
                    "canceled",
                )
                if live:
                    j = fake.jobs[fake.by_key[k]]
                    self._send(
                        200, {"id": j["id"], "state": j["state"], "reattached": True, "priority": 0}
                    )
                    return
                fake.created += 1
                jid = f"j{fake.created}"
                fake.jobs[jid] = {"id": jid, "state": "queued", "spec": spec}
                fake.by_key[k] = jid
                self._send(200, {"id": jid, "state": "queued", "reattached": False, "priority": 0})

            def do_GET(self) -> None:
                j = fake.jobs.get(self.path.split("/")[-1])
                if j is None:
                    self._send(404, {"detail": "unknown"})
                else:
                    self._send(200, {k: v for k, v in j.items() if k != "spec"})

        self.server = HTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def data(self, jid: str, stems: list[str]) -> dict[str, Any]:
        prefix = self.jobs[jid]["spec"]["payload"]["output_prefix"]
        return {
            "stems": {s: f"{prefix}/{jid}/{s}.wav" for s in stems},
            "chunk_seconds": 7.8,
            "chunk_seconds_requested": None,
            "model_type": "htdemucs",
        }

    def succeed(self, jid: str, stems: list[str]) -> None:
        self.jobs[jid].update(
            state="succeeded",
            finished_at=time.time(),
            result={"artifacts": [], "data": self.data(jid, stems)},
        )

    def prune(self, jid: str) -> None:
        """What the platform does to a finished job 24 h later: the record and its key go."""
        del self.by_key[self.jobs.pop(jid)["spec"]["idempotency_key"]]


@pytest.fixture
def fake() -> Iterator[FakeSpark]:
    f = FakeSpark()
    yield f
    f.server.shutdown()


@pytest.fixture
def music(tmp_path: Path) -> Path:
    d = tmp_path / "music"
    d.mkdir()
    (d / "techno.mp3").write_bytes(b"ID3 not really an mp3")
    return d


@pytest.fixture
def svc(fake: FakeSpark, music: Path, tmp_path: Path) -> spark.SparkService:
    return spark.SparkService(Client(fake.url, KEY), Ledger(tmp_path / "l.jsonl"), [music], SECRET)


def track(music: Path) -> str:
    return str(music / "techno.mp3")


def the_record(svc: spark.SparkService) -> dict[str, Any]:
    (r,) = svc.ledger.records().values()
    return r


def test_submit_shape(svc: spark.SparkService, fake: FakeSpark, music: Path) -> None:
    (res,) = svc.separate([track(music)])
    assert res["action"] == "submitted"
    spec = fake.submits[0]
    assert spec["type"] == "separate"
    assert spec["priority"] == 0  # never negative: eviction victim otherwise
    # below the platform's measured floor, so its number governs the reservation, not ours
    assert 0 < spec["memory_estimate_gb"] < 3.0
    # chunk_seconds omitted = the native segment; stems go to a prefix the reaper never visits
    assert spec["payload"] == {
        "audio": "inputs/u1/track.mp3",
        "model_type": "htdemucs",
        "stems": ["drums"],
        "output_prefix": "blinky/stems/htdemucs",
    }
    assert spec["idempotency_key"].endswith(":htdemucs:drums:native")
    assert KEY not in json.dumps(spec)  # credentials travel in the header only


def test_explicit_chunk(svc: spark.SparkService, fake: FakeSpark, music: Path) -> None:
    svc.separate([track(music)], chunk_seconds=6)
    assert fake.submits[0]["payload"]["chunk_seconds"] == 6
    assert fake.submits[0]["idempotency_key"].endswith(":drums:6")


@pytest.mark.parametrize(
    ("model_type", "chunk", "stems"),
    [
        ("htdemucs", 8, None),
        ("htdemucs", 0, None),
        ("mel-band-roformer", None, None),
        ("htdemucs", None, ["drum"]),
        ("htdemucs", None, ["drums", "drums"]),
    ],
)
def test_refused_before_any_request(
    svc: spark.SparkService,
    fake: FakeSpark,
    music: Path,
    model_type: str,
    chunk: int | None,
    stems: list[str] | None,
) -> None:
    with pytest.raises(SparkError):
        svc.separate([track(music)], model_type=model_type, stems=stems, chunk_seconds=chunk)
    assert fake.uploads == 0 and fake.submits == []


def test_path_outside_the_audio_roots(
    svc: spark.SparkService, fake: FakeSpark, tmp_path: Path
) -> None:
    secret = tmp_path / "secret.wav"
    secret.write_bytes(b"x")
    with pytest.raises(SparkError, match="outside the audio roots"):
        svc.separate([str(secret)])
    assert fake.uploads == 0


def test_rerun_skips_pending_and_succeeded(
    svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    svc.separate([track(music)])
    assert svc.separate([track(music)])[0]["action"] == "skipped"
    fake.succeed("j1", ["drums"])
    svc.reconcile()
    (res,) = svc.separate([track(music)])
    assert res == {"path": track(music), "action": "skipped", "job_id": "j1", "state": "succeeded"}
    assert len(fake.submits) == 1 and fake.uploads == 1


def test_lost_ledger_still_reattaches(
    svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    svc.separate([track(music)])
    svc.ledger.path.unlink()
    assert svc.separate([track(music)])[0]["action"] == "reattached"
    assert len(fake.jobs) == 1


def test_reconcile_records_success_with_provenance(
    svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    svc.separate([track(music)])
    fake.succeed("j1", ["drums"])
    assert svc.reconcile() == {"succeeded": 1}
    r = the_record(svc)
    assert r["outcome"] == "succeeded"
    assert r["stems"] == {"drums": "blinky/stems/htdemucs/j1/drums.wav"}
    assert r["chunk_seconds_used"] == 7.8
    assert svc.reconcile() == {}  # settled records are not polled again
    fake.prune("j1")
    assert the_record(svc)["outcome"] == "succeeded"  # the ledger outlives the job record


def test_pruned_before_recorded_is_lost_then_resubmitted(
    svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    svc.separate([track(music)])
    fake.succeed("j1", ["drums"])
    fake.prune("j1")
    assert svc.reconcile() == {"lost": 1}
    assert svc.separate([track(music)])[0]["action"] == "submitted"
    r = the_record(svc)
    assert r["job_id"] == "j2" and "outcome" not in r


def test_failed_is_recorded_and_resubmitted_only_on_request(
    svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    svc.separate([track(music)])
    fake.jobs["j1"].update(state="failed", error="boom", retryable=False)
    svc.reconcile()
    svc.reconcile()
    assert len(fake.submits) == 1  # the poll never retries a failed job
    r = the_record(svc)
    assert r["outcome"] == "failed" and r["error"] == "boom"
    svc.separate([track(music)])
    assert len(fake.submits) == 2 and "j2" in fake.jobs


def test_missing_stem_is_a_failure(svc: spark.SparkService, fake: FakeSpark, music: Path) -> None:
    svc.separate([track(music)], stems=["drums", "bass"])
    fake.succeed("j1", ["drums"])
    svc.reconcile()
    r = the_record(svc)
    assert r["outcome"] == "failed" and "!= requested" in r["error"]


def test_webhook_reads_the_full_result_the_sender_sends(
    svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    # The sender's body carries job.result, {"artifacts", "data"} (my-spark webhooks._payload).
    svc.separate([track(music)])
    data = fake.data("j1", ["drums"])
    body = {
        "id": "j1",
        "state": "succeeded",
        "finished_at": time.time(),
        "result": {"artifacts": list(data["stems"].values()), "data": data},
    }
    assert svc.on_webhook(body) == "recorded succeeded"
    r = the_record(svc)
    assert r["outcome"] == "succeeded"
    assert r["stems"] == {"drums": "blinky/stems/htdemucs/j1/drums.wav"}
    assert r["chunk_seconds_used"] == 7.8


def test_webhook_records_once(svc: spark.SparkService, fake: FakeSpark, music: Path) -> None:
    svc.separate([track(music)])
    body = {
        "id": "j1",
        "state": "succeeded",
        "result": fake.data("j1", ["drums"]),
        "finished_at": time.time(),
    }
    assert svc.on_webhook(body) == "recorded succeeded"
    assert svc.on_webhook(body) == "duplicate"  # at-least-once delivery
    assert svc.on_webhook({"id": "j1", "state": "running"}).startswith("ignored")
    assert svc.on_webhook({"id": "nope", "state": "succeeded"}).startswith("ignored")
    assert the_record(svc)["stems"]["drums"].endswith("/j1/drums.wav")
    assert sum(1 for r in svc.ledger.rows() if r.get("event")) == 1


@pytest.fixture
async def client(svc: spark.SparkService) -> Any:
    spark.set_service(svc)
    try:
        transport = ASGITransport(app=create_app(enable_ble=False, enable_serial=False))
        async with AsyncClient(transport=transport, base_url="http://test") as c:
            yield c
    finally:
        spark.set_service(None)


async def test_webhook_route_authenticates(
    client: AsyncClient, svc: spark.SparkService, fake: FakeSpark, music: Path
) -> None:
    svc.separate([track(music)])
    body = {"id": "j1", "state": "failed", "error": "boom"}
    r = await client.post(
        "/api/spark/webhook", json=body, headers={"Authorization": "Bearer wrong"}
    )
    assert r.status_code == 401
    r = await client.post(
        "/api/spark/webhook", json=body, headers={"Authorization": f"Bearer {SECRET}"}
    )
    assert r.status_code == 200 and r.json() == {"outcome": "recorded failed"}
    r = await client.get("/api/spark/jobs")
    assert r.json()["counts"] == {"failed": 1}


async def test_separate_route_needs_the_api_key(client: AsyncClient, music: Path) -> None:
    r = await client.post(
        "/api/spark/separate", json={"files": [track(music)]}, headers={"X-API-Key": "wrong"}
    )
    assert r.status_code == 401


async def test_unconfigured_is_503() -> None:
    spark.set_service(None)
    transport = ASGITransport(app=create_app(enable_ble=False, enable_serial=False))
    async with AsyncClient(transport=transport, base_url="http://test") as c:
        r = await c.get("/api/spark/jobs")
    assert r.status_code == 503


# -- training (gpu_task, #305) ---------------------------------------------------------------------


def git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@t", *args],
        cwd=repo,
        check=True,
        capture_output=True,
    )


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    r = tmp_path / "repo"
    (r / "ml-training" / "configs").mkdir(parents=True)
    (r / "ml-training" / "configs" / "x.yaml").write_text("model: {type: frame_conv1d}\n")
    (r / "ml-training" / "spark_entry.py").write_text("print('STEP 1/1')\n")
    (r / "ml-training" / "spark_entry.py").chmod(0o755)
    (r / "elsewhere.txt").write_text("not bundled\n")
    git(r, "init", "-q")
    git(r, "add", ".")
    git(r, "commit", "-qm", "init")
    return r


@pytest.fixture
def tsvc(fake: FakeSpark, music: Path, tmp_path: Path, repo: Path) -> spark.SparkService:
    return spark.SparkService(
        Client(fake.url, KEY), Ledger(tmp_path / "l.jsonl"), [music], SECRET, repo=repo
    )


def train_result(svc: spark.SparkService, jid: str, sha: str | None = None) -> dict[str, Any]:
    (rec,) = svc.train_ledger.records().values()
    return {
        "artifacts": [f"outputs/{jid}/model/best_model.pt", f"outputs/{jid}/model/spark_run.json"],
        "data": {
            "runtime": "torch-cu130",
            "entry_point": "spark_entry.py",
            "code_sha256": sha or rec["code_sha256"],
            "gpu_peak_gib": 3.2,
            "gpu_reserved_gib": 12.0,
        },
    }


def test_bundle_is_reproducible_and_only_ml_training(repo: Path, tmp_path: Path) -> None:
    a = bundle.build(repo, tmp_path / "a")
    os.utime(repo / "ml-training" / "configs" / "x.yaml", (1, 1))  # mtimes must not matter
    b = bundle.build(repo, tmp_path / "b")
    assert a.sha256 == b.sha256 and a.path.read_bytes() == b.path.read_bytes()
    assert a.names == {"configs/x.yaml", "spark_entry.py"}
    with tarfile.open(a.path) as t:
        m = {i.name: i for i in t.getmembers()}
    assert m["spark_entry.py"].mode == 0o755 and m["configs/x.yaml"].mode == 0o644
    assert all(i.mtime == 0 and i.uid == 0 for i in m.values())


def test_bundle_builds_the_deployable_commit_when_there_is_one(repo: Path, tmp_path: Path) -> None:
    # The test box's deployer moves refs/blinky/deployable to every master commit that passed
    # CI and review; the server's own HEAD stays put unless the server itself changed.
    gated = bundle.build(repo, tmp_path / "a").commit
    git(repo, "update-ref", "refs/blinky/deployable", gated)
    (repo / "ml-training" / "configs" / "y.yaml").write_text("model: {type: y}\n")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "ungated")
    (repo / "ml-training" / "configs" / "x.yaml").write_text("uncommitted\n")  # ignored: not HEAD
    b = bundle.build(repo, tmp_path / "b")
    assert b.commit == gated and "configs/y.yaml" not in b.names


def test_bundle_refuses_uncommitted_changes(repo: Path, tmp_path: Path) -> None:
    (repo / "ml-training" / "untracked.py").write_text("x = 1\n")  # not bundled: fine
    (repo / "elsewhere.txt").write_text("changed\n")  # outside ml-training: fine
    bundle.build(repo, tmp_path / "ok")
    (repo / "ml-training" / "configs" / "x.yaml").write_text("model: {type: other}\n")
    with pytest.raises(SparkError, match="uncommitted changes"):
        bundle.build(repo, tmp_path / "no")


def test_train_submit_shape(tsvc: spark.SparkService, fake: FakeSpark) -> None:
    out = tsvc.train(
        "configs/x.yaml", ["--epochs", "5"], ["--seed"], labels_dir="labels/kick_weighted_clean"
    )
    assert out["action"] == "submitted" and out["job_id"] == "j1"
    (spec,) = fake.submits
    assert spec["type"] == "gpu_task" and spec["priority"] == 0
    assert spec["memory_estimate_gb"] == spark.TRAIN_MEMORY_GB
    assert spec["payload"] == {
        "code": "inputs/u1/track.mp3",
        "entry_point": "spark_entry.py",
        "args": [
            "--config",
            "configs/x.yaml",
            "--audio-dir",
            "audio",
            "--labels-dir",
            "labels/kick_weighted_clean",
            "--prep-arg=--seed",
            "--",
            "--epochs",
            "5",
        ],
        "inputs": {"corpus": "datasets/blinky-corpus/"},
        "outputs": {"model": True},
        "runtime": "torch-cu130",
    }
    assert spec["idempotency_key"] == spark.train_key(out["code_sha256"], spec["payload"]["args"])
    (rec,) = tsvc.train_ledger.records().values()
    assert rec["commit"] == out["commit"] and rec["code_sha256"] == out["code_sha256"]
    assert not tsvc.ledger.records()  # separate's ledger is untouched


def test_train_rerun_skips_and_a_failure_resubmits_without_reuploading(
    tsvc: spark.SparkService, fake: FakeSpark
) -> None:
    tsvc.train("configs/x.yaml")
    assert tsvc.train("configs/x.yaml")["action"] == "skipped"
    fake.jobs["j1"].update(state="failed", error="exit 1")
    tsvc.reconcile()
    assert tsvc.train("configs/x.yaml")["action"] == "submitted"
    assert fake.uploads == 1  # the bundle is already in inputs/, which is never reaped
    assert tsvc.train("configs/x.yaml", ["--epochs", "5"])["action"] == "submitted"  # new args


def test_train_refuses_a_config_not_in_the_bundle(
    tsvc: spark.SparkService, fake: FakeSpark
) -> None:
    with pytest.raises(SparkError, match="not in ml-training"):
        tsvc.train("configs/nope.yaml")
    assert fake.uploads == 0 and not fake.submits


def test_train_success_records_the_model_and_measured_peak(
    tsvc: spark.SparkService, fake: FakeSpark
) -> None:
    tsvc.train("configs/x.yaml")
    fake.jobs["j1"].update(state="succeeded", result=train_result(tsvc, "j1"))
    tsvc.reconcile()
    (rec,) = tsvc.train_ledger.records().values()
    assert rec["outcome"] == "succeeded"
    assert "outputs/j1/model/best_model.pt" in rec["artifacts"]
    assert rec["gpu_peak_gib"] == 3.2 and rec["gpu_reserved_gib"] == 12.0
    assert tsvc.summary()["counts"] == {"succeeded": 1}


@pytest.mark.parametrize("bad", ["code", "model"])
def test_train_success_must_prove_code_and_model(
    tsvc: spark.SparkService, fake: FakeSpark, bad: str
) -> None:
    tsvc.train("configs/x.yaml")
    result = train_result(tsvc, "j1", sha="0" * 64 if bad == "code" else None)
    if bad == "model":
        result["artifacts"] = ["outputs/j1/model/training_log.csv"]
    body = {"id": "j1", "state": "succeeded", "finished_at": time.time(), "result": result}
    assert tsvc.on_webhook(body) == "recorded succeeded"
    (rec,) = tsvc.train_ledger.records().values()
    assert rec["outcome"] == "failed"
    assert ("code_sha256" if bad == "code" else "best_model.pt") in rec["error"]


async def test_train_route(tsvc: spark.SparkService, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(deps, "_api_key", "k")
    ok = {"X-API-Key": "k"}
    spark.set_service(tsvc)
    try:
        transport = ASGITransport(app=create_app(enable_ble=False, enable_serial=False))
        async with AsyncClient(transport=transport, base_url="http://test") as c:
            body = {"config": "configs/x.yaml"}
            r = await c.post("/api/spark/train", json=body, headers={"X-API-Key": "wrong"})
            assert r.status_code == 401
            r = await c.post("/api/spark/train", json={"config": "configs/nope.yaml"}, headers=ok)
            assert r.status_code == 400 and "not in ml-training" in r.json()["detail"]
            r = await c.post("/api/spark/train", json=body, headers=ok)
            assert r.status_code == 200 and r.json()["action"] == "submitted"
            r = await c.get("/api/spark/jobs")
            assert [t["config"] for t in r.json()["train"]] == ["configs/x.yaml"]
    finally:
        spark.set_service(None)
