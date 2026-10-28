"""Spark Job API client (#288): the service against a fake Job API, and the routes."""

from __future__ import annotations

import json
import threading
import time
from collections.abc import Iterator
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Any

import pytest
from httpx import ASGITransport, AsyncClient

from blinky_server.api.app import create_app
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
    assert spec["memory_estimate_gb"] > 0
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
    ("model_type", "chunk"), [("htdemucs", 8), ("htdemucs", 0), ("mel-band-roformer", None)]
)
def test_refused_before_any_request(
    svc: spark.SparkService, fake: FakeSpark, music: Path, model_type: str, chunk: int | None
) -> None:
    with pytest.raises(SparkError):
        svc.separate([track(music)], model_type=model_type, chunk_seconds=chunk)
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
