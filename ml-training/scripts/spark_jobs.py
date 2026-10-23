#!/usr/bin/env python3
"""Client for the Spark Job API: blinky_time's only integration with the Spark (#288).

my-spark owns all job execution and every image (my-spark plans/0238); blinky submits
parameterized requests and reads results back. Nothing of blinky's runs on the Spark.

The live contract here is `separate` (one track in, its stems out). Contract:
my-spark docs/architecture/api.md, "separate" and "Submit — POST /jobs".

usage:
  python scripts/spark_jobs.py separate [--model-type htdemucs] [--stems drums] FILE...
  python scripts/spark_jobs.py status            # one pass over the ledger, no loop
  python scripts/spark_jobs.py fetch --out DIR   # download the stems of succeeded jobs

Environment (both required, no defaults):
  SPARK_JOB_API   base URL of the Job API, e.g. http://<spark tailscale ip>:4100
  SPARK_API_KEY   blinky-time's key from the Spark's client registry (sent as a header only)

Every submission is appended to a JSONL ledger (default ml-training/outputs/spark/separate.jsonl,
gitignored), so
a restarted client re-attaches to its jobs instead of re-spending the GPU.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

# Long work submits at exactly 0, never below. The registry caps blinky at max_priority 0,
# which is a CEILING, not a floor: the box's eviction picks the LOWEST-priority running job
# first, so a "polite" negative priority makes our job the first victim of every memory
# event, and a long job then evict-requeue-resumes without ever finishing (my-spark
# plans/0238 §5a). Don't lower this to be nice.
PRIORITY = 0

# The platform floors `memory_estimate_gb` per type (separate: 12 GiB, provisional until
# Phase 1 measures it); a declaration above the floor still wins. Declare the floor.
SEPARATE_MEMORY_GB = 12.0

DEFAULT_LEDGER = Path(__file__).resolve().parents[1] / "outputs" / "spark" / "separate.jsonl"

# Retried: the request may not have reached a verdict. Never retried: a job that FAILED.
# The platform already spent its own retries on a retryable failure, and a client retry
# would stack on top of them (api.md, "retryable on a terminal failed job").
TRANSPORT_RETRIES = 3
RETRY_STATUS = {502, 503, 504}

TERMINAL = {"succeeded", "failed", "canceled"}


class SparkError(RuntimeError):
    pass


def _env(name: str) -> str:
    v = os.environ.get(name)
    if not v:
        raise SparkError(f"{name} is not set (see the module docstring); refusing to guess")
    return v


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Client:
    def __init__(self, base_url: str, api_key: str, timeout: float = 60.0):
        self.base = base_url.rstrip("/")
        self.key = api_key
        self.timeout = timeout
        # Artifact GETs answer 307 to a presigned URL. urllib would follow it carrying our
        # X-API-Key; the presigned URL needs no key, so redirects are followed by hand.
        self._opener = urllib.request.build_opener(_NoRedirect)

    def _request(self, method: str, path: str, body: bytes | None = None,
                 content_type: str | None = None) -> tuple[int, dict, bytes]:
        headers = {"X-API-Key": self.key}
        if content_type:
            headers["Content-Type"] = content_type
        req = urllib.request.Request(self.base + path, data=body, method=method, headers=headers)
        for attempt in range(TRANSPORT_RETRIES + 1):
            try:
                with self._opener.open(req, timeout=self.timeout) as r:
                    return r.status, dict(r.headers), r.read()
            except urllib.error.HTTPError as e:
                if e.code in (301, 302, 303, 307, 308):
                    return e.code, dict(e.headers), b""
                if e.code in RETRY_STATUS and attempt < TRANSPORT_RETRIES:
                    time.sleep(2 ** attempt)
                    continue
                detail = e.read().decode("utf-8", "replace")[:500]
                raise SparkError(f"{method} {path} -> HTTP {e.code}: {detail}") from None
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                if attempt < TRANSPORT_RETRIES:
                    time.sleep(2 ** attempt)
                    continue
                raise SparkError(f"{method} {path}: {e}") from None
        raise AssertionError("unreachable")

    def _json(self, method: str, path: str, obj: dict | None = None) -> dict:
        body = None if obj is None else json.dumps(obj).encode()
        status, _, data = self._request(method, path, body,
                                        "application/json" if body is not None else None)
        if status != 200:
            raise SparkError(f"{method} {path} -> HTTP {status}")
        return json.loads(data)

    def upload(self, path: Path) -> str:
        """POST /artifacts (multipart field `file`) -> the artifact key."""
        boundary = uuid.uuid4().hex
        head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                f"filename=\"{path.name}\"\r\nContent-Type: application/octet-stream\r\n\r\n").encode()
        body = head + path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
        status, _, data = self._request("POST", "/artifacts", body,
                                        f"multipart/form-data; boundary={boundary}")
        if status != 200:
            raise SparkError(f"upload {path} -> HTTP {status}")
        uri = json.loads(data).get("uri")
        if not uri:
            raise SparkError(f"upload {path}: response has no uri: {data[:200]!r}")
        return uri

    def submit(self, spec: dict) -> dict:
        return self._json("POST", "/jobs", spec)

    def job(self, job_id: str) -> dict:
        return self._json("GET", f"/jobs/{urllib.parse.quote(job_id)}")

    def download(self, key: str, dest: Path) -> None:
        status, headers, data = self._request("GET", "/artifacts/" + urllib.parse.quote(key))
        if status in (301, 302, 303, 307, 308):
            loc = headers.get("Location") or headers.get("location")
            if not loc:
                raise SparkError(f"GET artifact {key}: redirect without a Location")
            with urllib.request.urlopen(loc, timeout=self.timeout) as r:   # presigned: no key
                data = r.read()
        elif status != 200:
            raise SparkError(f"GET artifact {key} -> HTTP {status}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_suffix(dest.suffix + ".part")
        tmp.write_bytes(data)
        tmp.replace(dest)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def idempotency_key(sha: str, model_type: str, stems: list[str], chunk_seconds: int) -> str:
    return f"sep:{sha}:{model_type}:{'+'.join(stems)}:{chunk_seconds}"


def read_ledger(path: Path) -> list[dict]:
    if not path.exists():
        return []
    rows = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if line.strip():
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as e:
                    raise SparkError(f"{path}:{n}: corrupt ledger line: {e}") from None
    return rows


def latest_by_key(rows: list[dict]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for r in rows:
        out[r["idempotency_key"]] = r
    return out


def append_ledger(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(row, sort_keys=True) + "\n")


def cmd_separate(client: Client, files: list[Path], model_type: str, stems: list[str],
                 chunk_seconds: int, ledger: Path) -> int:
    known = latest_by_key(read_ledger(ledger))
    uploaded = {r["sha256"]: r["audio"] for r in known.values()}
    for path in files:
        if not path.is_file():
            raise SparkError(f"{path}: not a file")
        sha = sha256_file(path)
        key = idempotency_key(sha, model_type, stems, chunk_seconds)
        audio = uploaded.get(sha) or client.upload(path)
        uploaded[sha] = audio
        resp = client.submit({
            "type": "separate",
            "payload": {"audio": audio, "model_type": model_type, "stems": stems,
                        "chunk_seconds": chunk_seconds},
            "memory_estimate_gb": SEPARATE_MEMORY_GB,
            "priority": PRIORITY,
            "idempotency_key": key,
            "name": f"blinky separate {path.stem}",
        })
        # `priority` is what we got, not what we asked for (api.md, Submit): read it.
        if resp.get("priority_clamped") or resp.get("priority") != PRIORITY:
            print(f"WARNING {path.name}: priority {resp.get('priority')} "
                  f"(clamped: {resp.get('priority_clamped')})", file=sys.stderr)
        row = {"path": str(path), "sha256": sha, "audio": audio, "model_type": model_type,
               "stems": stems, "chunk_seconds": chunk_seconds, "idempotency_key": key,
               "job_id": resp["id"], "state": resp.get("state"),
               "reattached": bool(resp.get("reattached")), "submitted_at": time.time()}
        append_ledger(ledger, row)
        print(f"{path.name}: job {resp['id']} {resp.get('state')}"
              f"{' (re-attached)' if row['reattached'] else ''}")
    return 0


def cmd_status(client: Client, ledger: Path) -> int:
    rows = latest_by_key(read_ledger(ledger))
    if not rows:
        raise SparkError(f"{ledger}: no submissions recorded")
    counts: dict[str, int] = {}
    for r in rows.values():
        j = client.job(r["job_id"])
        state = j.get("state", "?")
        counts[state] = counts.get(state, 0) + 1
        line = f"{Path(r['path']).name}: {state}"
        if state == "failed":
            line += f" -- {j.get('error')}"
        elif state not in TERMINAL and j.get("progress"):
            line += f" {j['progress'].get('pct', '')}% {j['progress'].get('message', '')}"
        print(line)
    print(" ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return 1 if counts.get("failed") or counts.get("canceled") else 0


def cmd_fetch(client: Client, ledger: Path, out: Path) -> int:
    rows = latest_by_key(read_ledger(ledger))
    pending = 0
    for r in rows.values():
        j = client.job(r["job_id"])
        if j.get("state") != "succeeded":
            pending += 1
            continue
        stems = (j.get("result") or {}).get("data", {}).get("stems")
        if not isinstance(stems, dict) or set(stems) != set(r["stems"]):
            raise SparkError(f"job {r['job_id']}: result stems {stems!r} != requested {r['stems']}")
        for stem, key in stems.items():
            dest = out / r["model_type"] / f"{Path(r['path']).stem}.{stem}.wav"
            if not dest.exists():
                client.download(key, dest)
                print(f"{dest}")
    if pending:
        print(f"{pending} job(s) not succeeded yet; run `status`", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("separate", help="upload tracks and submit one separate job per track")
    s.add_argument("files", type=Path, nargs="+")
    s.add_argument("--model-type", choices=["htdemucs", "mel-band-roformer"], default="htdemucs")
    s.add_argument("--stems", default="drums", help="comma-separated, e.g. drums or drums,bass")
    s.add_argument("--chunk-seconds", type=int, default=10)
    sub.add_parser("status", help="one pass over the ledger's jobs")
    f = sub.add_parser("fetch", help="download stems of succeeded jobs")
    f.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        client = Client(_env("SPARK_JOB_API"), _env("SPARK_API_KEY"))
        if a.cmd == "separate":
            stems = [x for x in a.stems.split(",") if x]
            return cmd_separate(client, a.files, a.model_type, stems, a.chunk_seconds, a.ledger)
        if a.cmd == "status":
            return cmd_status(client, a.ledger)
        return cmd_fetch(client, a.ledger, a.out)
    except SparkError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
