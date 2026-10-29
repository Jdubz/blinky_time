"""The Spark CLI (#288) is a thin client of blinky-server's /api/spark routes."""

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


@pytest.fixture
def server(monkeypatch):
    seen: list[dict] = []

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, obj):
            body = json.dumps(obj).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            seen.append({"path": self.path, "key": self.headers.get("X-API-Key"),
                         "body": json.loads(self.rfile.read(n)) if n else None})
            if self.path == "/api/spark/separate":
                self._send({"results": [{"path": f, "action": "submitted", "job_id": "j1",
                                         "state": "queued"} for f in seen[-1]["body"]["files"]]})
            elif self.path.startswith("/api/spark/corpus/"):
                self._send({"mode": "first", "reason": "the first batch runs alone",
                            "counts": {"pending": 3}, "batch_no": 0, "batch": 50})
            elif self.path == "/api/spark/train":
                self._send({"action": "submitted", "job_id": "j2", "state": "queued",
                            "commit": "c" * 40, "code_sha256": "s" * 64})
            else:
                self._send({"counts": {"queued": 1}})

        def do_GET(self):
            seen.append({"path": self.path, "key": self.headers.get("X-API-Key")})
            self._send({"counts": {"failed": 1}, "webhook": True, "jobs": [
                {"path": "/m/techno.mp3", "job_id": "j1", "outcome": "failed", "error": "boom"}]})

    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    monkeypatch.setenv("BLINKY_SERVER", f"http://127.0.0.1:{srv.server_address[1]}")
    monkeypatch.setenv("BLINKY_API_KEY", "k")
    yield seen
    srv.shutdown()


def test_separate_sends_absolute_paths_and_omits_no_chunk(server, tmp_path, capsys):
    f = tmp_path / "techno.mp3"
    f.write_bytes(b"x")
    assert spark_jobs.main(["separate", str(f)]) == 0
    (req,) = server
    assert req["path"] == "/api/spark/separate" and req["key"] == "k"
    assert req["body"] == {"files": [str(f.resolve())], "stems": ["drums"], "chunk_seconds": None}
    assert "submitted job j1" in capsys.readouterr().out


def test_train_passes_everything_after_the_separator_to_train_py(server, capsys):
    argv = ["train", "configs/x.yaml", "--prep-arg=--seed", "--prep-arg=7", "--",
            "--epochs", "5"]
    assert spark_jobs.main(argv) == 0
    (req,) = server
    assert req["path"] == "/api/spark/train"
    assert req["body"] == {"config": "configs/x.yaml", "labels_dir": None, "augment": True,
                           "prep_args": ["--seed", "7"], "memory_gb": 12.0,
                           "train_args": ["--epochs", "5"]}
    assert "submitted job j2" in capsys.readouterr().out


def test_corpus_start_and_status(server, capsys):
    assert spark_jobs.main(["corpus", "start", "--stems", "drums,bass", "--batch", "20"]) == 0
    (req,) = server
    assert req["path"] == "/api/spark/corpus/start"
    assert req["body"] == {"stems": ["drums", "bass"], "batch": 20, "retry_failed": False}
    out = capsys.readouterr().out
    assert "corpus pass: first -- the first batch runs alone" in out and "pending=3" in out


def test_status_fails_on_a_failed_job(server, capsys):
    assert spark_jobs.main(["status"]) == 1
    assert "boom" in capsys.readouterr().out


def test_unreachable_server_is_an_error(monkeypatch, capsys):
    monkeypatch.setenv("BLINKY_SERVER", "http://127.0.0.1:9")
    monkeypatch.setenv("BLINKY_API_KEY", "k")
    assert spark_jobs.main(["reconcile"]) == 2
    assert "is blinky-server running" in capsys.readouterr().err
