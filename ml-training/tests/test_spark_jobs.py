"""Spark Job API client (#288), against a fake Job API on localhost."""

from __future__ import annotations

import json
import sys
import threading
import time
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
        self.reaped = False
        self.created = 0
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
                    fake.created += 1
                    jid = f"j{fake.created}"
                    fake.jobs[jid] = {"id": jid, "state": "queued", "spec": spec}
                    fake.by_key[k] = jid
                    self._send(200, {"id": jid, "state": "queued", "reattached": False, "priority": 0})

            def do_GET(self):
                if self.path.startswith("/blob/"):
                    fake.blob_requests.append(dict(self.headers))
                    if fake.reaped:
                        self._send(404, {"detail": "no such key"})
                    else:
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

    def succeed(self, jid: str, stems: list[str], finished_at: float | None = None):
        self.jobs[jid].update(state="succeeded", finished_at=finished_at or time.time(), result={
            "artifacts": [f"outputs/{jid}/{s}.wav" for s in stems],
            "data": {"stems": {s: f"outputs/{jid}/{s}.wav" for s in stems}, "chunk_seconds": 7.8,
                     "chunk_seconds_requested": None}})

    def prune(self, jid: str):
        """What the platform does to a finished job 24 h later: the record and its key mapping go."""
        k = self.jobs.pop(jid)["spec"]["idempotency_key"]
        del self.by_key[k]


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
    assert spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger) == 0
    spec = fake.submits[0]
    assert spec["type"] == "separate"
    assert spec["priority"] == 0                       # never negative: eviction victim otherwise
    assert spec["memory_estimate_gb"] > 0
    # chunk_seconds omitted = the engine's native segment; htdemucs refuses anything longer (#1534)
    assert spec["payload"] == {"audio": "inputs/u1/track.mp3", "model_type": "htdemucs",
                               "stems": ["drums"]}
    assert spec["idempotency_key"].startswith("sep:")
    assert spec["idempotency_key"].endswith(":drums:native")
    assert KEY not in json.dumps(spec)                 # credentials travel in the header only


def test_explicit_chunk_is_sent_and_keyed(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], 6, tmp_path / "l.jsonl")
    assert fake.submits[0]["payload"]["chunk_seconds"] == 6
    assert fake.submits[0]["idempotency_key"].endswith(":drums:6")


@pytest.mark.parametrize("chunk", [8, 10, 0])
def test_unusable_chunk_is_refused_before_any_request(fake, track, tmp_path, chunk):
    c = spark_jobs.Client(fake.url, KEY)
    with pytest.raises(spark_jobs.SparkError, match="chunk-seconds"):
        spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], chunk, tmp_path / "l.jsonl")
    assert fake.uploads == [] and fake.submits == []


def test_roformer_is_not_offered(capsys):
    with pytest.raises(SystemExit):
        spark_jobs.main(["separate", "--model-type", "mel-band-roformer", "x.mp3"])


def test_rerun_skips_a_live_job(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    assert len(fake.uploads) == 1
    assert len(fake.submits) == 1
    assert len(spark_jobs.read_ledger(ledger)) == 1


def test_lost_ledger_still_reattaches(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    ledger.unlink()
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    assert len(fake.jobs) == 1
    assert [r["reattached"] for r in spark_jobs.read_ledger(ledger)] == [True]


def test_recorded_result_survives_the_prune(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.succeed("j1", ["drums"])
    assert spark_jobs.cmd_status(c, ledger) == 0          # records the stems
    fake.prune("j1")
    out = tmp_path / "stems"
    assert spark_jobs.cmd_fetch(c, ledger, out) == 0
    assert (out / "htdemucs" / "techno.drums.wav").read_bytes() == b"RIFFstem"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    assert len(fake.submits) == 1                         # fetched: nothing re-spent
    rec = spark_jobs.fold_ledger(spark_jobs.read_ledger(ledger))
    (r,) = rec.values()
    assert r["fetched"] == str(out) and r["chunk_seconds_used"] == 7.8


def test_unrecorded_prune_is_reported_then_resubmitted(fake, track, tmp_path, capsys):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.succeed("j1", ["drums"])
    fake.prune("j1")
    assert spark_jobs.cmd_status(c, ledger) == 1
    assert "pruned" in capsys.readouterr().out
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    assert len(fake.submits) == 2
    (r,) = spark_jobs.fold_ledger(spark_jobs.read_ledger(ledger)).values()
    assert r["job_id"] == "j2" and not r.get("result_stems")


def test_reaped_stems_are_marked_and_resubmitted(fake, track, tmp_path, capsys):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.succeed("j1", ["drums"])
    fake.reaped = True
    assert spark_jobs.cmd_fetch(c, ledger, tmp_path / "stems") == 1
    assert "reaped" in capsys.readouterr().err
    assert spark_jobs.cmd_status(c, ledger) == 1
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    assert len(fake.submits) == 2


def test_status_warns_before_the_reaper(fake, track, tmp_path, capsys):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.succeed("j1", ["drums"], finished_at=time.time() - 6 * 86400)
    assert spark_jobs.cmd_status(c, ledger) == 0
    assert "WARNING" in capsys.readouterr().out


def test_failed_job_is_resubmitted_only_on_an_explicit_rerun(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.jobs["j1"].update(state="failed", error="boom", retryable=False)
    spark_jobs.cmd_status(c, ledger)
    assert len(fake.submits) == 1
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    assert len(fake.submits) == 2 and "j2" in fake.jobs


def test_status_reports_failure_and_does_not_resubmit(fake, track, tmp_path, capsys):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.jobs["j1"].update(state="failed", error="ValueError: separator produced no 'drums' stem",
                           retryable=False)
    assert spark_jobs.cmd_status(c, ledger) == 1
    assert "no 'drums' stem" in capsys.readouterr().out
    assert len(fake.submits) == 1


def test_fetch_follows_presign_without_the_key(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, ledger)
    fake.succeed("j1", ["drums"])
    out = tmp_path / "stems"
    assert spark_jobs.cmd_fetch(c, ledger, out) == 0
    assert (out / "htdemucs" / "techno.drums.wav").read_bytes() == b"RIFFstem"
    assert all("X-API-Key" not in h for h in fake.blob_requests)


def test_fetch_refuses_missing_stem(fake, track, tmp_path):
    c = spark_jobs.Client(fake.url, KEY)
    ledger = tmp_path / "l.jsonl"
    spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums", "bass"], None, ledger)
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
        spark_jobs.cmd_separate(c, [track], "htdemucs", ["drums"], None, tmp_path / "l.jsonl")
