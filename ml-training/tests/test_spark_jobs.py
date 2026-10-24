"""Spark Job API client (#288), against a fake Job API on localhost."""

from __future__ import annotations

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import pytest

ML = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ML))

from scripts import spark_jobs  # noqa: E402

KEY = "test-key"


class FakeSpark:
    """Just enough of the Job API: artifacts, idempotent submit, job records, a presign redirect."""

    def __init__(self):
        self.jobs: dict[str, dict] = {}
        self.by_key: dict[str, str] = {}
        self.uploads: list[bytes] = []
        self.submits: list[dict] = []
        self.blob_requests: list[dict] = []
        fake = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, code, obj=None, headers=None, raw=None):
                self.send_response(code)
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                body = raw if raw is not None else (b"" if obj is None else json.dumps(obj).encode())
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _authed(self):
                if self.headers.get("X-API-Key") != KEY:
                    self._send(401, {"detail": "no key"})
                    return False
                return True

            def do_POST(self):
                if not self._authed():
                    return
                body = self.rfile.read(int(self.headers["Content-Length"]))
                if self.path == "/artifacts":
                    assert self.headers["Content-Type"].startswith("multipart/form-data")
                    fake.uploads.append(body)
                    self._send(200, {"uri": f"inputs/u{len(fake.uploads)}/track.mp3", "size": len(body)})
                elif self.path == "/jobs":
                    spec = json.loads(body)
                    fake.submits.append(spec)
                    k = spec.get("idempotency_key")
                    if k in fake.by_key and fake.jobs[fake.by_key[k]]["state"] not in ("failed", "canceled"):
                        j = fake.jobs[fake.by_key[k]]
                        self._send(200, {"id": j["id"], "state": j["state"], "reattached": True,
                                         "priority": 0})
                        return
                    jid = f"j{len(fake.jobs) + 1}"
                    fake.jobs[jid] = {"id": jid, "state": "queued", "spec": spec}
                    fake.by_key[k] = jid
                    self._send(200, {"id": jid, "state": "queued", "reattached": False, "priority": 0})

            def do_GET(self):
                if self.path.startswith("/blob/"):
                    fake.blob_requests.append(dict(self.headers))
                    self._send(200, raw=b"RIFFstem")
                    return
                if not self._authed():
                    return
                if self.path.startswith("/jobs/"):
                    j = fake.jobs.get(self.path.split("/")[-1])
                    self._send(404 if j is None else 200, j and {k: v for k, v in j.items() if k != "spec"})
                elif self.path.startswith("/artifacts/"):
                    port = self.server.server_address[1]
                    self._send(307, headers={"Location": f"http://127.0.0.1:{port}/blob/x"})

        self.server = HTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def succeed(self, jid: str, stems: list[str]):
        self.jobs[jid].update(state="succeeded", result={
            "artifacts": [f"outputs/{jid}/{s}.wav" for s in stems],
            "data": {"stems": {s: f"outputs/{jid}/{s}.wav" for s in stems}}})


@pytest.fixture
def fake():
    f = FakeSpark()
    yield f
    f.server.shutdown()


@pytest.fixture
def track(tmp_path):
    p = tmp_path / "techno.mp3"
    p.write_bytes(b"ID3 not really an mp3")
    return p


def test_submit_shape_priority_zero_and_key(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    assert spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 10, ledger) == 0
    spec = fake.submits[0]
    assert spec["type"] == "separate"
    assert spec["priority"] == 0                       # never negative: eviction victim otherwise
    assert spec["memory_estimate_gb"] > 0
    assert spec["payload"] == {"audio": "inputs/u1/track.mp3", "model_type": "htdemucs",
                               "stems": ["drums"], "chunk_seconds": 10}
    assert spec["idempotency_key"].startswith("sep:") and spec["idempotency_key"].endswith(":drums:10")
    assert KEY not in json.dumps(spec)                 # credentials travel in the header only


def test_rerun_reattaches_without_reupload(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 10, ledger)
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 10, ledger)
    assert len(fake.uploads) == 1
    assert len(fake.jobs) == 1
    rows = spark_jobs.read_ledger(ledger)
    assert [r["reattached"] for r in rows] == [False, True]


def test_status_reports_failure_and_does_not_resubmit(fake, track, tmp_path, capsys):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 10, ledger)
    fake.jobs["j1"].update(state="failed", error="ValueError: separator produced no 'drums' stem",
                           retryable=False)
    assert spark_jobs.cmd_status(c, ledger) == 1
    assert "no 'drums' stem" in capsys.readouterr().out
    assert len(fake.submits) == 1


def test_fetch_follows_presign_without_the_key(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 10, ledger)
    fake.succeed("j1", ["drums"])
    out = tmp_path / "stems"
    assert spark_jobs.cmd_fetch(c, ledger, out) == 0
    assert (out / "htdemucs" / "techno.drums.wav").read_bytes() == b"RIFFstem"
    assert all("X-API-Key" not in h for h in fake.blob_requests)


def test_fetch_refuses_missing_stem(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums", "bass"], 10, ledger)
    fake.succeed("j1", ["drums"])
    with pytest.raises(spark_jobs.SparkError, match="requested"):
        spark_jobs.cmd_fetch(c, ledger, tmp_path / "stems")


def test_missing_env_is_an_error(monkeypatch, capsys):
    monkeypatch.delenv("SPARK_JOB_API", raising=False)
    monkeypatch.delenv("SPARK_API_KEY", raising=False)
    assert spark_jobs.main(["status"]) == 2
    assert "SPARK_JOB_API is not set" in capsys.readouterr().err


def test_http_error_is_loud(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, "wrong-key")
    with pytest.raises(spark_jobs.SparkError, match="HTTP 401"):
        spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 10, tmp_path / "l.jsonl")
