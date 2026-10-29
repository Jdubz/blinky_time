#!/usr/bin/env python3
"""Command line for blinky-server's Spark client (#288).

blinky-server owns the Spark integration: it submits `separate` and training jobs, receives
the Spark's webhooks, polls as a backstop, and keeps the ledger (blinky_server/spark/). This
script only calls its API, so the ledger has exactly one writer. Stems stay in the Spark's
object store under blinky/stems/<model_type>/; nothing is downloaded.

usage:
  python scripts/spark_jobs.py separate [--stems drums] [--chunk-seconds N] FILE...
  python scripts/spark_jobs.py train CONFIG [--labels-dir DIR] [--no-augment] [--prep-arg=A]
                                     [--memory-gb N] [-- TRAIN_ARG...]
  python scripts/spark_jobs.py corpus start [--stems drums] [--batch 50] | continue | pause | status
  python scripts/spark_jobs.py status       # the ledger, as blinky-server holds it
  python scripts/spark_jobs.py reconcile    # poll the Spark now instead of waiting for the hour

`corpus` separates every track in the Spark's corpus (its store's manifest, my-spark #1556), in
batches that each drain before the next. The first batch runs alone and the pass waits for
`corpus continue` once its stems are checked; a batch where most jobs fail pauses it (#314).
`separate` takes at most 50 FILEs.

FILEs are read by the server, so they must be under its audio roots (by default the repo's
blinky-test-player/music) on the machine it runs on.

`train` bundles ml-training/ from the latest master commit that passed CI and review (on the
test box, refs/blinky/deployable; elsewhere the server checkout's HEAD, with uncommitted changes
refused) and runs spark_entry.py on the Spark: prep from the corpus, then train.py. CONFIG is relative to ml-training/, e.g. configs/conv1d_w16_onset_v34d_clean_labels.yaml.
Corpus dirs are relative to the corpus in the Spark's store (datasets/blinky-corpus/).

Environment:
  BLINKY_SERVER   blinky-server's base URL (default http://127.0.0.1:8420)
  BLINKY_API_KEY  its API key (default: read from ~/.blinky-api-key)
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
import sys
import time
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


# blinky-server restarts (a deploy) without losing a submission: it finishes the step in flight,
# resumes the rest after, and the same request sent again attaches to it (#325). So a request
# that meets a restart is sent again, for up to this long.
RESTART_RETRY_S = 300.0
RETRY_EVERY_S = 5.0
# Safe to send again: reads, and the submissions that attach to their pending workflow. A
# corpus action that met a restart may already have happened; sent again it would be refused
# ("already running"), so it is reported and the status read instead.
RESENDABLE = ("/api/spark/separate", "/api/spark/train")


def call(method: str, path: str, body: dict | None = None, timeout: float = 600.0) -> dict:
    """The request, sent again while blinky-server is restarting: a 503 (draining), or a
    connection that dropped mid-request. A refused connection counts only once a restart has
    been seen in this call; on its own it means the server isn't running."""
    deadline = time.monotonic() + RESTART_RETRY_S
    resendable = method == "GET" or path in RESENDABLE
    restarting = False
    while True:
        try:
            return _call_once(method, path, body, timeout)
        except _Restarting as e:
            if not resendable:
                raise CliError(
                    f"{method} {path}: blinky-server restarted mid-request ({e}); it may have "
                    f"been applied. Check with `spark_jobs.py corpus status`."
                ) from None
            restarting = True
            why = str(e)
        except urllib.error.URLError as e:
            refused = isinstance(e.reason, ConnectionRefusedError)
            if not (restarting and refused and resendable):
                raise CliError(f"{method} {path}: {e.reason} (is blinky-server running?)") from None
            why = "not listening yet"
        if time.monotonic() > deadline:
            raise CliError(f"{method} {path}: blinky-server still restarting after "
                           f"{RESTART_RETRY_S:.0f} s ({why})")
        print(f"blinky-server is restarting ({why}); sending the request again", file=sys.stderr)
        time.sleep(RETRY_EVERY_S)


class _Restarting(Exception):
    pass


def _call_once(method: str, path: str, body: dict | None, timeout: float) -> dict:
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
        if e.code == 503 and "restarting" in detail:
            raise _Restarting("HTTP 503: draining") from None
        raise CliError(f"{method} {path} -> HTTP {e.code}: {detail}") from None
    except urllib.error.URLError as e:
        # A reset while the body was still being sent arrives wrapped.
        if isinstance(e.reason, (ConnectionResetError, BrokenPipeError)):
            raise _Restarting(f"connection dropped: {type(e.reason).__name__}") from None
        raise
    except (http.client.RemoteDisconnected, ConnectionResetError, http.client.IncompleteRead) as e:
        raise _Restarting(f"connection dropped: {type(e).__name__}") from None


def cmd_separate(files: list[Path], stems: list[str], chunk_seconds: int | None) -> int:
    body = {"files": [str(f.resolve()) for f in files], "stems": stems,
            "chunk_seconds": chunk_seconds}
    for r in call("POST", "/api/spark/separate", body)["results"]:
        line = f"{Path(r['path']).name}: {r['action']} job {r['job_id']} {r.get('state')}"
        if "priority" in r:  # the server reports priority only when it isn't what we asked
            line += f" (WARNING: priority {r['priority']}, clamped {r.get('priority_clamped')})"
        print(line)
    return 0


def cmd_train(config: str, labels_dir: str | None, augment: bool, prep_args: list[str],
              memory_gb: float, train_args: list[str]) -> int:
    body = {"config": config, "labels_dir": labels_dir, "augment": augment,
            "prep_args": prep_args, "memory_gb": memory_gb, "train_args": train_args}
    r = call("POST", "/api/spark/train", body)
    line = (f"{config}: {r['action']} job {r['job_id']} {r.get('state')} "
            f"(commit {r['commit'][:12]}, code {r['code_sha256'][:12]})")
    if "priority" in r:
        line += f" (WARNING: priority {r['priority']}, clamped {r.get('priority_clamped')})"
    print(line)
    return 0


def cmd_corpus(action: str, stems: list[str], batch: int, retry_failed: bool) -> int:
    if action == "status":
        s = call("GET", "/api/spark/corpus")
    elif action == "start":
        s = call("POST", "/api/spark/corpus/start",
                 {"stems": stems, "batch": batch, "retry_failed": retry_failed})
    else:
        s = call("POST", f"/api/spark/corpus/{action}")
    print(f"corpus pass: {s['mode']}" + (f" -- {s['reason']}" if s.get("reason") else ""))
    if "counts" in s:
        print(" ".join(f"{k}={v}" for k, v in s["counts"].items())
              + f" (batch {s['batch_no']}, {s['batch']} per batch)")
    for t in s.get("last_batch") or []:
        line = f"  {t['track']}: {t['outcome']}"
        if isinstance(t.get("stems"), dict):  # only a success has stem keys
            line += " " + " ".join(f"{k}={v}" for k, v in t["stems"].items())
        if t.get("error"):
            line += f" -- {t['error']}"
        print(line)
    return 0


def cmd_status() -> int:
    s = call("GET", "/api/spark/jobs")
    for r in s["jobs"]:
        line = f"{Path(r['path']).name}: {r.get('outcome') or r.get('state')} job {r['job_id']}"
        if r.get("error"):
            line += f" -- {r['error']}"
        print(line)
    for r in s.get("train", []):
        line = (f"train {r['config']} @{r['commit'][:12]}: {r.get('outcome') or r.get('state')} "
                f"job {r['job_id']}")
        if r.get("gpu_peak_gib") is not None:
            line += f" (GPU peak {r['gpu_peak_gib']:.2f} GiB of {r['memory_gb']:g} declared)"
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
    argv = sys.argv[1:] if argv is None else list(argv)
    train_args: list[str] = []
    if "--" in argv:  # everything after `--` goes to train.py on the Spark
        i = argv.index("--")
        argv, train_args = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("separate", help="submit one separate job per track (htdemucs)")
    s.add_argument("files", type=Path, nargs="+")
    s.add_argument("--stems", default="drums", help="comma-separated, e.g. drums or drums,bass")
    s.add_argument("--chunk-seconds", type=int, default=None,
                   help="segment length; omit for the engine's native one (htdemucs: 7.8 s, the max)")
    t = sub.add_parser("train", help="bundle ml-training/ from master and train it on the Spark")
    t.add_argument("config", help="relative to ml-training/, e.g. configs/x.yaml")
    t.add_argument("--labels-dir", default=None,
                   help="relative to the corpus; default: the config's labels_dir")
    t.add_argument("--no-augment", action="store_true")
    t.add_argument("--prep-arg", action="append", default=[],
                   help="passed to prepare_dataset.py (repeatable; --prep-arg=--seed)")
    t.add_argument("--memory-gb", type=float, default=12.0,
                   help="declared GPU memory; the result reports the measured peak")
    c = sub.add_parser("corpus", help="separate the Spark's whole corpus, batch by batch")
    c.add_argument("action", choices=["start", "continue", "pause", "status"])
    c.add_argument("--stems", default="drums", help="comma-separated, for start")
    c.add_argument("--batch", type=int, default=50, help="tracks per batch, for start (max 50)")
    c.add_argument("--retry-failed", action="store_true",
                   help="for start: include tracks whose job failed in an earlier pass")
    sub.add_parser("status", help="show the server's ledger")
    sub.add_parser("reconcile", help="poll the Spark for pending jobs now")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "separate":
            return cmd_separate(a.files, [x for x in a.stems.split(",") if x], a.chunk_seconds)
        if a.cmd == "train":
            return cmd_train(a.config, a.labels_dir, not a.no_augment, a.prep_arg, a.memory_gb,
                             train_args)
        if a.cmd == "corpus":
            return cmd_corpus(a.action, [x for x in a.stems.split(",") if x], a.batch,
                              a.retry_failed)
        if a.cmd == "status":
            return cmd_status()
        return cmd_reconcile()
    except CliError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
