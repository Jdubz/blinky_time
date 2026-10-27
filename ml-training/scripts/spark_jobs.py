#!/usr/bin/env python3
"""Client for the Spark Job API: blinky_time's only integration with the Spark (#288).

my-spark owns all job execution and every image (my-spark plans/0238); blinky submits
parameterized requests and reads results back. Nothing of blinky's runs on the Spark.

The live contract here is `separate` (one track in, its stems out). Contract:
my-spark docs/architecture/api.md, "separate" and "Submit — POST /jobs".

usage:
  python scripts/spark_jobs.py separate [--stems drums] [--chunk-seconds N] FILE...
  python scripts/spark_jobs.py status            # one pass over the ledger, no loop
  python scripts/spark_jobs.py fetch --out DIR   # download the stems of succeeded jobs

Environment (both required, no defaults):
  SPARK_JOB_API   base URL of the Job API, e.g. http://<spark tailscale ip>:4100
  SPARK_API_KEY   blinky-time's key from the Spark's client registry (sent as a header only)

Every submission is appended to a JSONL ledger (default ml-training/outputs/spark/separate.jsonl,
gitignored), and so is each result the first time `status` or `fetch` sees it. A restarted client
skips the tracks it already has instead of re-spending the GPU.

Two retention limits on the platform side matter for a pass that runs for days:
  - A finished job's record is pruned 24 h after it finishes. After that, GET /jobs/{id} is a 404,
    and the same idempotency key starts a new job. Run `status` or `fetch` at least daily during
    a pass: a result nobody recorded within 24 h is lost, and its track gets separated again.
  - Stems live under outputs/, which the store reaps 7 days after they are written. `fetch` well
    inside that; `status` warns at 5 days.
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

# htdemucs's segment is fixed by its architecture at 39/5 = 7.8 s. The engine refuses a longer
# `chunk_seconds` rather than clamping it (my-spark #1534), but only once the job has waited its
# turn in the queue. Refuse it here instead. Omit the field to get the native segment.
HTDEMUCS_SEGMENT_S = 7.8

# Retention on the platform (see the module docstring): finished job records go after 24 h,
# un-kept outputs/ after 7 days. We fetch the stems rather than keep-tag them, since a corpus of
# keep-tagged stems would sit in the shared store forever.
JOB_RECORD_TTL_S = 24 * 3600
OUTPUT_TTL_S = 7 * 24 * 3600
REAP_WARN_S = 5 * 24 * 3600

DEFAULT_LEDGER = Path(__file__).resolve().parents[1] / "outputs" / "spark" / "separate.jsonl"

# Retried: the request may not have reached a verdict. Never retried: a job that FAILED.
# The platform already spent its own retries on a retryable failure, and a client retry
# would stack on top of them (api.md, "retryable on a terminal failed job").
TRANSPORT_RETRIES = 3
RETRY_STATUS = {502, 503, 504}

TERMINAL = {"succeeded", "failed", "canceled"}


class SparkError(RuntimeError):
    pass


class NotFound(SparkError):
    """A 404: a pruned job record, or a reaped artifact."""


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
                if e.code == 404:
                    raise NotFound(f"{method} {path} -> HTTP 404") from None
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
            try:
                with urllib.request.urlopen(loc, timeout=self.timeout) as r:   # presigned: no key
                    data = r.read()
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    raise NotFound(f"GET artifact {key} (presigned) -> HTTP 404") from None
                raise SparkError(f"GET artifact {key} (presigned) -> HTTP {e.code}") from None
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


def idempotency_key(sha: str, model_type: str, stems: list[str], chunk_seconds: int | None) -> str:
    chunk = "native" if chunk_seconds is None else chunk_seconds
    return f"sep:{sha}:{model_type}:{'+'.join(stems)}:{chunk}"


def check_chunk(model_type: str, chunk_seconds: int | None) -> None:
    if chunk_seconds is None:
        return
    if chunk_seconds < 1:
        raise SparkError(f"--chunk-seconds {chunk_seconds}: must be >= 1, or omitted")
    if model_type == "htdemucs" and chunk_seconds > HTDEMUCS_SEGMENT_S:
        raise SparkError(f"--chunk-seconds {chunk_seconds} exceeds htdemucs's fixed segment of "
                         f"{HTDEMUCS_SEGMENT_S:g} s, and the engine would refuse it after queueing; "
                         f"omit it to use the native segment")


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


def fold_ledger(rows: list[dict]) -> dict[str, dict]:
    """The current record per idempotency key. A row without an `event` is a submission and starts
    a fresh record. An event row annotates the submission whose job_id it names, and is dropped if
    that job has since been replaced by a resubmission."""
    out: dict[str, dict] = {}
    for r in rows:
        k = r["idempotency_key"]
        ev = r.get("event")
        if ev is None:
            out[k] = dict(r)
            continue
        cur = out.get(k)
        if cur is None or cur["job_id"] != r["job_id"]:
            continue
        if ev == "succeeded":
            cur.update(result_stems=r["stems"], chunk_seconds_used=r.get("chunk_seconds_used"),
                       finished_at=r.get("finished_at"))
        elif ev == "fetched":
            cur["fetched"] = r["out"]
        elif ev == "reaped":
            cur.update(result_stems=None, fetched=None, reaped=True)
        else:
            raise SparkError(f"ledger: unknown event {ev!r} for {k}")
    return out


def append_ledger(path: Path, row: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(row, sort_keys=True) + "\n")


def record_success(ledger: Path, rec: dict, job: dict) -> None:
    """Write a succeeded job's stems to the ledger, so they outlive the job record (24 h)."""
    data = (job.get("result") or {}).get("data") or {}
    stems = data.get("stems")
    if not isinstance(stems, dict) or set(stems) != set(rec["stems"]):
        raise SparkError(f"job {rec['job_id']}: result stems {stems!r} != requested {rec['stems']}")
    # `chunk_seconds` in the result is the segment the engine USED, which is the provenance a label
    # set needs; the request may have been "native".
    row = {"event": "succeeded", "idempotency_key": rec["idempotency_key"], "job_id": rec["job_id"],
           "stems": stems, "chunk_seconds_used": data.get("chunk_seconds"),
           "finished_at": job.get("finished_at"), "at": time.time()}
    append_ledger(ledger, row)
    rec.update(result_stems=stems, chunk_seconds_used=row["chunk_seconds_used"],
               finished_at=row["finished_at"])


def _resubmit_reason(client: Client, ledger: Path, rec: dict) -> str | None:
    """Why a track the ledger already knows must be submitted again, or None to skip it."""
    name = Path(rec["path"]).name
    if rec.get("fetched"):
        print(f"{name}: already fetched to {rec['fetched']}; skipping")
        return None
    if rec.get("result_stems"):
        print(f"{name}: succeeded, stems in the store; run `fetch`")
        return None
    if rec.get("reaped"):
        return "its stems were reaped from the store before they were fetched"
    try:
        j = client.job(rec["job_id"])
    except NotFound:
        return f"job {rec['job_id']} was pruned before its result was recorded"
    state = j.get("state")
    if state == "succeeded":
        record_success(ledger, rec, j)
        print(f"{name}: succeeded; run `fetch`")
        return None
    if state in ("failed", "canceled"):
        return f"job {rec['job_id']} {state}"
    print(f"{name}: job {rec['job_id']} {state}; skipping")
    return None


def cmd_separate(client: Client, files: list[Path], model_type: str, stems: list[str],
                 chunk_seconds: int | None, ledger: Path) -> int:
    check_chunk(model_type, chunk_seconds)
    known = fold_ledger(read_ledger(ledger))
    uploaded = {r["sha256"]: r["audio"] for r in known.values()}
    for path in files:
        if not path.is_file():
            raise SparkError(f"{path}: not a file")
        sha = sha256_file(path)
        key = idempotency_key(sha, model_type, stems, chunk_seconds)
        if key in known:
            why = _resubmit_reason(client, ledger, known[key])
            if why is None:
                continue
            print(f"{path.name}: {why}; resubmitting", file=sys.stderr)
        audio = uploaded.get(sha) or client.upload(path)
        uploaded[sha] = audio
        payload: dict = {"audio": audio, "model_type": model_type, "stems": stems}
        if chunk_seconds is not None:   # omitted = the engine's native segment
            payload["chunk_seconds"] = chunk_seconds
        resp = client.submit({
            "type": "separate",
            "payload": payload,
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


def _describe(client: Client, ledger: Path, r: dict, now: float) -> tuple[str, str]:
    name = Path(r["path"]).name
    if r.get("fetched"):
        return "fetched", f"{name}: fetched to {r['fetched']}"
    if r.get("reaped"):
        return "reaped", f"{name}: stems reaped before they were fetched; `separate` resubmits it"
    if not r.get("result_stems"):
        try:
            j = client.job(r["job_id"])
        except NotFound:
            return "pruned", (f"{name}: job {r['job_id']} pruned (records last "
                              f"{JOB_RECORD_TTL_S / 3600:g} h after finishing) before its result was "
                              f"recorded; `separate` resubmits it")
        state = j.get("state", "?")
        if state == "failed":
            return state, f"{name}: failed -- {j.get('error')}"
        if state != "succeeded":
            line = f"{name}: {state}"
            if state not in TERMINAL and j.get("progress"):
                line += f" {j['progress'].get('pct', '')}% {j['progress'].get('message', '')}"
            return state, line
        record_success(ledger, r, j)
    line = f"{name}: succeeded, not fetched"
    if r.get("finished_at") and now - r["finished_at"] > REAP_WARN_S:
        line += (f" -- WARNING: finished {(now - r['finished_at']) / 86400:.1f} d ago and the store "
                 f"reaps outputs/ at {OUTPUT_TTL_S / 86400:g} d; run `fetch`")
    return "succeeded", line


def cmd_status(client: Client, ledger: Path) -> int:
    rows = fold_ledger(read_ledger(ledger))
    if not rows:
        raise SparkError(f"{ledger}: no submissions recorded")
    counts: dict[str, int] = {}
    now = time.time()
    for r in rows.values():
        state, line = _describe(client, ledger, r, now)
        counts[state] = counts.get(state, 0) + 1
        print(line)
    print(" ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    return 1 if any(counts.get(k) for k in ("failed", "canceled", "pruned", "reaped")) else 0


def cmd_fetch(client: Client, ledger: Path, out: Path) -> int:
    rows = fold_ledger(read_ledger(ledger))
    pending = lost = 0
    for r in rows.values():
        if not r.get("result_stems"):
            if r.get("reaped"):
                lost += 1
                continue
            try:
                j = client.job(r["job_id"])
            except NotFound:          # pruned before anyone recorded it; `status` names it
                pending += 1
                continue
            if j.get("state") != "succeeded":
                pending += 1
                continue
            record_success(ledger, r, j)
        try:
            for stem, key in r["result_stems"].items():
                dest = out / r["model_type"] / f"{Path(r['path']).stem}.{stem}.wav"
                if not dest.exists():
                    client.download(key, dest)
                    print(f"{dest}")
        except NotFound as e:
            append_ledger(ledger, {"event": "reaped", "idempotency_key": r["idempotency_key"],
                                   "job_id": r["job_id"], "at": time.time()})
            print(f"{Path(r['path']).name}: {e}; the stems were reaped before fetch", file=sys.stderr)
            lost += 1
            continue
        if r.get("fetched") != str(out):
            append_ledger(ledger, {"event": "fetched", "idempotency_key": r["idempotency_key"],
                                   "job_id": r["job_id"], "out": str(out), "at": time.time()})
    if pending:
        print(f"{pending} job(s) not succeeded yet; run `status`", file=sys.stderr)
    if lost:
        print(f"{lost} job(s) lost their stems to the store's {OUTPUT_TTL_S / 86400:g}-day reaper; "
              f"`separate` resubmits them", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("separate", help="upload tracks and submit one separate job per track")
    s.add_argument("files", type=Path, nargs="+")
    # mel-band-roformer is in the platform's contract, but its engine refuses it at runtime
    # (demucs 4.0.1 doesn't ship it, my-spark #1534). Add it back when the platform's image has it.
    s.add_argument("--model-type", choices=["htdemucs"], default="htdemucs")
    s.add_argument("--stems", default="drums", help="comma-separated, e.g. drums or drums,bass")
    s.add_argument("--chunk-seconds", type=int, default=None,
                   help="segment length; omit for the engine's native one (htdemucs: 7.8 s, the max)")
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
