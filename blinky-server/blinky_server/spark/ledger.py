"""The append-only JSONL record of every Spark submission and its outcome (one file per kind).

A finished job's record leaves the Spark 24 h after it finishes (`SPARK_JOB_TTL_SEC`); after
that `GET /jobs/{id}` is a 404 and the same idempotency key starts a new job. So the outcome is
written here the moment the server learns it, from the webhook or the reconcile poll, and the
ledger, not the Spark, is what decides whether a track needs separating again.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from pathlib import Path
from typing import Any

from .client import SparkError

log = logging.getLogger(__name__)

# Outcome events. A row without an `event` is a submission and starts a fresh record.
SUCCEEDED = "succeeded"
FAILED = "failed"  # failed or canceled; `state` says which
LOST = "lost"  # pruned before anyone recorded its outcome
EVENTS = {SUCCEEDED, FAILED, LOST}


class Ledger:
    def __init__(self, path: Path) -> None:
        self.path = path
        # The webhook (event loop) and submit/reconcile (worker threads) all append.
        self._lock = threading.Lock()

    def rows(self) -> list[dict[str, Any]]:
        if not self.path.exists():
            return []
        rows = []
        with open(self.path, encoding="utf-8") as f:
            lines = f.readlines()
        for n, line in enumerate(lines, 1):
            if line.strip():
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as e:
                    if n == len(lines) and not line.endswith("\n"):
                        # A torn tail: the write a crash or power cut interrupted (#325). It
                        # never completed, so it is not a record; append() cuts it off.
                        log.error(
                            "%s:%d: torn final line from an interrupted write, ignored",
                            self.path,
                            n,
                        )
                        continue
                    raise SparkError(f"{self.path}:{n}: corrupt ledger line: {e}") from None
        return rows

    def records(self) -> dict[str, dict[str, Any]]:
        """The current record per idempotency key. An outcome row applies only to the submission
        whose job_id it names; one for a job since replaced by a resubmission is dropped."""
        out: dict[str, dict[str, Any]] = {}
        with self._lock:
            rows = self.rows()
        for r in rows:
            k = r["idempotency_key"]
            ev = r.get("event")
            if ev is None:
                out[k] = dict(r)
                continue
            if ev not in EVENTS:
                raise SparkError(f"{self.path}: unknown event {ev!r} for {k}")
            cur = out.get(k)
            if cur is None or cur["job_id"] != r["job_id"]:
                continue
            cur["outcome"] = ev
            cur["state"] = r.get("state", ev)
            for f in (
                "stems",
                "chunk_seconds_used",
                "finished_at",
                "error",
                "artifacts",
                "gpu_peak_gib",
                "gpu_reserved_gib",
            ):
                if f in r:
                    cur[f] = r[f]
        return out

    def by_job_id(self, job_id: str) -> dict[str, Any] | None:
        for r in self.records().values():
            if r["job_id"] == job_id:
                return r
        return None

    def append(self, row: dict[str, Any]) -> None:
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._cut_torn_tail()
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(json.dumps(row, sort_keys=True) + "\n")
                f.flush()
                os.fsync(f.fileno())  # a submission recorded is one a restart can't lose

    def _cut_torn_tail(self) -> None:
        """Mend a final line without its newline (an interrupted write) before appending, so
        the next row doesn't join it and corrupt both: dropped if it is incomplete, given back
        its newline if the write lost only that."""
        if not self.path.exists() or self.path.stat().st_size == 0:
            return
        with open(self.path, "rb+") as f:
            data = f.read()
            if data.endswith(b"\n"):
                return
            keep = data.rfind(b"\n") + 1
            try:
                json.loads(data[keep:])
            except ValueError:
                log.error(
                    "%s: cutting off a torn final line (%d bytes) from an interrupted write",
                    self.path,
                    len(data) - keep,
                )
                f.truncate(keep)
            else:
                # Complete, only its newline lost: a real record (rows() reads it). Keep it.
                f.write(b"\n")
