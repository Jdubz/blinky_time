#!/usr/bin/env python3
"""Command line for blinky-server's Spark client (#288).

blinky-server owns the Spark integration: it submits `separate` jobs, receives the Spark's
webhooks, polls as a backstop, and keeps the ledger (blinky_server/spark/). This script only
calls its API, so the ledger has exactly one writer. Stems stay in the Spark's object store
under blinky/stems/<model_type>/; nothing is downloaded.

usage:
  python scripts/spark_jobs.py separate [--stems drums] [--chunk-seconds N] FILE...
  python scripts/spark_jobs.py status       # the ledger, as blinky-server holds it
  python scripts/spark_jobs.py reconcile    # poll the Spark now instead of waiting for the hour

FILEs are read by the server, so they must be under its audio roots (by default the repo's
blinky-test-player/music) on the machine it runs on.

Environment:
  BLINKY_SERVER   blinky-server's base URL (default http://127.0.0.1:8420)
  BLINKY_API_KEY  its API key (default: read from ~/.blinky-api-key)
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_SERVER = "http://127.0.0.1:8420"


class CliError(RuntimeError):
    pass


def _api_key() -> str:
    key = os.environ.get("BLINKY_API_KEY")
    if not key:
        f = Path.home() / ".blinky-api-key"
        if not f.is_file():
            raise CliError("BLINKY_API_KEY is not set and ~/.blinky-api-key does not exist")
        key = f.read_text().strip()
    return key


def call(method: str, path: str, body: dict | None = None, timeout: float = 600.0) -> dict:
    base = os.environ.get("BLINKY_SERVER", DEFAULT_SERVER).rstrip("/")
    headers = {"X-API-Key": _api_key()}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:500]
        raise CliError(f"{method} {path} -> HTTP {e.code}: {detail}") from None
    except urllib.error.URLError as e:
        raise CliError(f"{method} {path}: {e.reason} (is blinky-server running?)") from None


def cmd_separate(files: list[Path], stems: list[str], chunk_seconds: int | None) -> int:
    body = {"files": [str(f.resolve()) for f in files], "stems": stems,
            "chunk_seconds": chunk_seconds}
    for r in call("POST", "/api/spark/separate", body)["results"]:
        line = f"{Path(r['path']).name}: {r['action']} job {r['job_id']} {r.get('state')}"
        if "priority" in r:  # the server reports priority only when it isn't what we asked
            line += f" (WARNING: priority {r['priority']}, clamped {r.get('priority_clamped')})"
        print(line)
    return 0


def cmd_status() -> int:
    s = call("GET", "/api/spark/jobs")
    for r in s["jobs"]:
        line = f"{Path(r['path']).name}: {r.get('outcome') or r.get('state')} job {r['job_id']}"
        if r.get("error"):
            line += f" -- {r['error']}"
        print(line)
    print(" ".join(f"{k}={v}" for k, v in sorted(s["counts"].items())))
    if not s["webhook"]:
        print("note: webhooks are off on this server; it relies on the hourly poll",
              file=sys.stderr)
    return 1 if any(s["counts"].get(k) for k in ("failed", "lost")) else 0


def cmd_reconcile() -> int:
    counts = call("POST", "/api/spark/reconcile")["counts"]
    print(" ".join(f"{k}={v}" for k, v in sorted(counts.items())) or "nothing pending")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("separate", help="submit one separate job per track (htdemucs)")
    s.add_argument("files", type=Path, nargs="+")
    s.add_argument("--stems", default="drums", help="comma-separated, e.g. drums or drums,bass")
    s.add_argument("--chunk-seconds", type=int, default=None,
                   help="segment length; omit for the engine's native one (htdemucs: 7.8 s, the max)")
    sub.add_parser("status", help="show the server's ledger")
    sub.add_parser("reconcile", help="poll the Spark for pending jobs now")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "separate":
            return cmd_separate(a.files, [x for x in a.stems.split(",") if x], a.chunk_seconds)
        if a.cmd == "status":
            return cmd_status()
        return cmd_reconcile()
    except CliError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
