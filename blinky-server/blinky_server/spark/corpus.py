"""The corpus separation pass (#314): every track in the Spark's corpus, in drained batches.

The corpus lives in the Spark's store, listed by the manifest my-spark's upload tool publishes
(`datasets/blinky-corpus/MANIFEST.json`, my-spark #1556). The pass submits BATCH tracks, waits
until every one of them has an outcome, records it, and only then submits the next batch:

- The platform runs `separate` one at a time, so a deep queue is no faster; it costs the
  controller a re-read of the queue every tick, and one bad parameter fails all of it
  (my-spark #1553). A batch loses at most a batch.
- The first batch runs alone. The pass then waits in `review` until someone has checked its
  stems and calls `continue`.
- A batch where more than half the jobs failed pauses the pass, with the reason recorded.
- Progress lives in the ledger (one record per track, keyed by its sha256) and in a small
  control file, so a restart resumes where the pass was. A re-submit after a crash between
  "submitted" and "recorded" reattaches to the same job (the idempotency key).

Failed tracks are not retried by the pass; `start(retry_failed=True)` includes them again.
"""

from __future__ import annotations

import json
import logging
import re
import threading
import time
from collections.abc import Callable
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from .client import NotFound, SparkError
from .ledger import FAILED, LOST, SUCCEEDED

if TYPE_CHECKING:
    from .service import SparkService

log = logging.getLogger(__name__)

MANIFEST_KEY = "datasets/blinky-corpus/MANIFEST.json"
AUDIO_PREFIX = "datasets/blinky-corpus/audio/"
AUDIO_EXTENSIONS = (".mp3", ".wav", ".flac", ".ogg")
BATCH = 50
FAILED_BATCH_FRACTION = 0.5  # more than this many failed in one batch: pause

# Modes. `first` and `running` submit; `review` and `paused` wait for a person; `done` is done.
FIRST, REVIEW, RUNNING, PAUSED, DONE = "first", "review", "running", "paused", "done"

_SHA256 = re.compile(r"^[0-9a-f]{64}$")


def tracks_from_manifest(raw: bytes) -> tuple[dict[str, Any], list[tuple[str, str]]]:
    """(manifest info, [(key, sha256)] for every audio track, sorted). Refuses a manifest that
    isn't the agreed format rather than guessing at one."""
    try:
        m = json.loads(raw)
    except ValueError as e:
        raise SparkError(f"{MANIFEST_KEY} is not JSON: {e}") from None
    if not isinstance(m, dict) or m.get("version") != 1 or not isinstance(m.get("files"), list):
        raise SparkError(f"{MANIFEST_KEY}: expected version 1 with a `files` list")
    tracks = []
    for f in m["files"]:
        key, sha = f.get("key"), f.get("sha256")
        if not isinstance(key, str) or not key.startswith(AUDIO_PREFIX):
            continue
        if not key.lower().endswith(AUDIO_EXTENSIONS):
            continue
        if not isinstance(sha, str) or not _SHA256.match(sha):
            raise SparkError(f"{MANIFEST_KEY}: {key} has no sha256")
        tracks.append((key, sha))
    if not tracks:
        raise SparkError(f"{MANIFEST_KEY} lists no audio under {AUDIO_PREFIX}")
    info = {"generated_at": m.get("generated_at"), "tracks": len(tracks)}
    return info, sorted(tracks)


def _const(value: str) -> Callable[[], str]:
    return lambda: value


class CorpusPass:
    def __init__(self, svc: SparkService, path: Path) -> None:
        self.svc = svc
        self.path = path  # the control file; the track list sits next to it
        self.tracks_path = path.with_name(path.stem + "_tracks.json")
        self._lock = threading.Lock()  # the tick (a worker thread) and the routes

    # -- state ------------------------------------------------------------------------------

    def _load(self) -> dict[str, Any] | None:
        if not self.path.exists():
            return None
        state: dict[str, Any] = json.loads(self.path.read_text())
        return state

    def _save(self, c: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(c, indent=2) + "\n")
        tmp.replace(self.path)

    def _tracks(self) -> list[tuple[str, str]]:
        return [(k, s) for k, s in json.loads(self.tracks_path.read_text())]

    def _key(self, c: dict[str, Any], sha: str) -> str:
        from .service import idempotency_key

        return idempotency_key(sha, c["model_type"], c["stems"], c["chunk_seconds"])

    # -- actions ----------------------------------------------------------------------------

    def start(
        self,
        model_type: str = "htdemucs",
        stems: list[str] | None = None,
        chunk_seconds: int | None = None,
        batch: int = BATCH,
        retry_failed: bool = False,
    ) -> dict[str, Any]:
        from .service import SEPARATE_BATCH_MAX, check_request

        stems = stems or ["drums"]
        check_request(model_type, stems, chunk_seconds)
        if not 1 <= batch <= SEPARATE_BATCH_MAX:
            raise SparkError(f"batch {batch}: must be 1..{SEPARATE_BATCH_MAX}")
        with self._lock:
            c = self._load()
            if c is not None and c["mode"] in (FIRST, RUNNING, REVIEW):
                raise SparkError(f"a corpus pass is already {c['mode']}; pause it first")
            try:
                raw = self.svc.client.get_artifact(MANIFEST_KEY)
            except NotFound:
                raise SparkError(
                    f"{MANIFEST_KEY} isn't in the Spark's store yet (my-spark #1556): "
                    f"nothing lists the corpus without it"
                ) from None
            info, tracks = tracks_from_manifest(raw)
            self.tracks_path.parent.mkdir(parents=True, exist_ok=True)
            self.tracks_path.write_text(json.dumps(tracks))
            c = {
                "mode": FIRST,
                "reason": "the first batch runs alone; check its stems, then `continue`",
                "model_type": model_type,
                "stems": stems,
                "chunk_seconds": chunk_seconds,
                "batch": batch,
                "retry_failed": retry_failed,
                "manifest": info,
                "started_at": time.time(),
                "batch_no": 0,
                "current": [],
                "batches": [],
            }
            self._save(c)
        log.info("spark corpus: started over %d tracks", info["tracks"])
        return self.status()

    def resume(self) -> dict[str, Any]:
        """`continue`: after the first batch's review, or after a pause."""
        with self._lock:
            c = self._load()
            if c is None or c["mode"] not in (REVIEW, PAUSED):
                mode = c["mode"] if c else "not started"
                raise SparkError(f"nothing to continue: the pass is {mode}")
            c["mode"], c["reason"] = RUNNING, None
            self._save(c)
        return self.status()

    def pause(self, reason: str = "paused by request") -> dict[str, Any]:
        """Stop submitting. Jobs already submitted finish; their outcomes still arrive."""
        with self._lock:
            c = self._load()
            if c is None or c["mode"] == DONE:
                raise SparkError("no corpus pass to pause")
            c["mode"], c["reason"] = PAUSED, reason
            self._save(c)
        return self.status()

    # -- the loop ---------------------------------------------------------------------------

    def tick(self) -> None:
        """One step: poll the batch in flight, or close it, or submit the next. Called every
        CORPUS_TICK_S by the service's loop. Blocking: run it in a thread."""
        with self._lock:
            c = self._load()
            if c is None or c["mode"] not in (FIRST, RUNNING):
                return
            if c["current"]:
                self._drain(c)
            else:
                self._submit_next(c)

    def _drain(self, c: dict[str, Any]) -> None:
        records = self.svc.ledger.records()
        batch = [records.get(k) for k in c["current"]]
        waiting = {r["job_id"] for r in batch if r is not None and not r.get("outcome")}
        if waiting:  # the webhook usually settles these first; the poll is the backstop
            self.svc.reconcile(job_ids=waiting)
            records = self.svc.ledger.records()
            batch = [records.get(k) for k in c["current"]]
            if any(r is None or not r.get("outcome") for r in batch):
                return
        outcomes = [r["outcome"] if r else LOST for r in batch]
        ok = outcomes.count(SUCCEEDED)
        bad = len(outcomes) - ok
        c["batches"].append({"n": c["batch_no"], "succeeded": ok, "failed": bad, "at": time.time()})
        c["last_batch"] = [
            {
                "track": r["path"],
                "outcome": r.get("outcome"),
                "stems": r.get("stems"),
                "error": r.get("error"),
            }
            for r in batch
            if r is not None
        ]
        c["current"] = []
        if bad > FAILED_BATCH_FRACTION * len(outcomes):
            c["mode"] = PAUSED
            c["reason"] = (
                f"batch {c['batch_no']}: {bad}/{len(outcomes)} failed; check, then continue"
            )
            log.warning("spark corpus: %s", c["reason"])
        elif c["mode"] == FIRST:
            c["mode"] = REVIEW
            c["reason"] = "first batch done: check its stems (status: last_batch), then continue"
        self._save(c)

    def _submit_next(self, c: dict[str, Any]) -> None:
        records = self.svc.ledger.records()
        retry = (FAILED, LOST) if c.get("retry_failed") else (LOST,)
        pending = []
        for key, sha in self._tracks():
            rec = records.get(self._key(c, sha))
            if rec is None or rec.get("outcome") in retry:
                pending.append((key, sha))
            if len(pending) == c["batch"]:
                break
        if not pending:
            c["mode"], c["reason"] = DONE, None
            self._save(c)
            log.info("spark corpus: done")
            return
        c["batch_no"] += 1
        submitted = []
        try:
            for key, sha in pending:
                self.svc.submit_separate(
                    key,
                    sha,
                    _const(key),  # already in the store: nothing to upload
                    PurePosixPath(key).stem,
                    c["model_type"],
                    c["stems"],
                    c["chunk_seconds"],
                    records,
                    extra={"corpus": True},
                )
                submitted.append(self._key(c, sha))
        finally:  # a crash mid-batch still drains what went in; the rest is pending again
            c["current"] = submitted
            self._save(c)
        log.info("spark corpus: batch %d submitted (%d tracks)", c["batch_no"], len(submitted))

    # -- reading ----------------------------------------------------------------------------

    def status(self) -> dict[str, Any]:
        c = self._load()
        if c is None:
            return {"mode": "not started"}
        records = self.svc.ledger.records()
        counts = {"succeeded": 0, "failed": 0, "in_flight": 0, "pending": 0}
        for _, sha in self._tracks():
            rec = records.get(self._key(c, sha))
            if rec is None or rec.get("outcome") == LOST:
                counts["pending"] += 1
            elif not rec.get("outcome"):
                counts["in_flight"] += 1
            else:
                counts["succeeded" if rec["outcome"] == SUCCEEDED else "failed"] += 1
        keep = (
            "mode",
            "reason",
            "model_type",
            "stems",
            "chunk_seconds",
            "batch",
            "batch_no",
            "manifest",
            "started_at",
            "last_batch",
        )
        return {**{k: c.get(k) for k in keep}, "counts": counts, "batches": c["batches"][-10:]}
