"""Submit `separate` jobs, and record their outcomes from the webhook and a reconcile poll.

Stems never come to this machine. Each job writes them under `blinky/stems/<model_type>/` in the
Spark's object store, which the reaper never visits (only `outputs/` is reaped, after 7 days;
my-spark docs/architecture/artifact-store.md). The work that reads stems runs on the Spark, so
the ledger here holds their keys, not their bytes.

Configuration (environment; on the test box, `~/.config/blinky/spark.env`):
  SPARK_JOB_API         base URL of the Job API (required)
  SPARK_API_KEY         blinky-time's key from the Spark's client registry (required)
  SPARK_WEBHOOK_SECRET  the secret the Spark sends as `Authorization: Bearer` (webhook only)
  BLINKY_SPARK_AUDIO_ROOTS  directories tracks may be read from (os.pathsep-separated);
                        default: the repo's blinky-test-player/music
Without the two required values the Spark routes answer 503 and nothing else changes.
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
import os
import time
from pathlib import Path
from typing import Any

from .. import paths
from .client import Client, NotFound, SparkError
from .ledger import FAILED, LOST, SUCCEEDED, Ledger

log = logging.getLogger(__name__)

# Long work submits at exactly 0, never below. The registry caps blinky at max_priority 0,
# which is a CEILING, not a floor: the box's eviction picks the LOWEST-priority running job
# first, so a "polite" negative priority makes our job the first victim of every memory
# event, and a long job then evict-requeue-resumes without ever finishing (my-spark
# plans/0238 §5a). Don't lower this to be nice.
PRIORITY = 0

# The platform floors `memory_estimate_gb` per type (separate: 12 GiB, provisional until
# Phase 1 measures it); a declaration above the floor still wins. Declare the floor.
SEPARATE_MEMORY_GB = 12.0

# mel-band-roformer is in the platform's contract, but its engine refuses it at runtime
# (demucs 4.0.1 doesn't ship it, my-spark #1534). Add it here when the platform's image has it.
MODEL_TYPES = ("htdemucs",)

# htdemucs's segment is fixed by its architecture at 39/5 = 7.8 s. The engine refuses a longer
# `chunk_seconds` rather than clamping it (my-spark #1534), but only once the job has waited its
# turn in the queue. Refuse it here instead. Omit the field to get the native segment.
HTDEMUCS_SEGMENT_S = 7.8

# Durable: the store's reaper only lists outputs/.
STEMS_PREFIX = "blinky/stems"

# The webhook is the primary signal; this poll catches a delivery the Spark dropped. It must run
# well inside the 24 h a finished job's record survives, or that job's outcome is lost.
RECONCILE_INTERVAL_S = 3600.0

TERMINAL = {"succeeded", "failed", "canceled"}

_DEFAULT_AUDIO_ROOT = Path(__file__).resolve().parents[3] / "blinky-test-player" / "music"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def idempotency_key(sha: str, model_type: str, stems: list[str], chunk_seconds: int | None) -> str:
    chunk = "native" if chunk_seconds is None else chunk_seconds
    return f"sep:{sha}:{model_type}:{'+'.join(stems)}:{chunk}"


def check_request(model_type: str, stems: list[str], chunk_seconds: int | None) -> None:
    if model_type not in MODEL_TYPES:
        raise SparkError(f"model_type {model_type!r} is not available; use one of {MODEL_TYPES}")
    if not stems or len(set(stems)) != len(stems):
        raise SparkError(f"stems must be non-empty and unique: {stems}")
    if chunk_seconds is None:
        return
    if chunk_seconds < 1:
        raise SparkError(f"chunk_seconds {chunk_seconds}: must be >= 1, or omitted")
    if model_type == "htdemucs" and chunk_seconds > HTDEMUCS_SEGMENT_S:
        raise SparkError(
            f"chunk_seconds {chunk_seconds} exceeds htdemucs's fixed segment of "
            f"{HTDEMUCS_SEGMENT_S:g} s, and the engine would refuse it after queueing; "
            f"omit it to use the native segment"
        )


class SparkService:
    def __init__(
        self,
        client: Client,
        ledger: Ledger,
        audio_roots: list[Path],
        webhook_secret: str | None = None,
    ) -> None:
        self.client = client
        self.ledger = ledger
        self.audio_roots = [r.resolve() for r in audio_roots]
        self.webhook_secret = webhook_secret

    @classmethod
    def from_env(cls) -> SparkService:
        missing = [n for n in ("SPARK_JOB_API", "SPARK_API_KEY") if not os.environ.get(n)]
        if missing:
            raise SparkError(f"Spark client not configured: {', '.join(missing)} unset")
        roots_env = os.environ.get("BLINKY_SPARK_AUDIO_ROOTS")
        roots = [Path(p) for p in roots_env.split(os.pathsep) if p] if roots_env else []
        return cls(
            Client(os.environ["SPARK_JOB_API"], os.environ["SPARK_API_KEY"]),
            Ledger(paths.data_dir() / "spark" / "separate.jsonl"),
            roots or [_DEFAULT_AUDIO_ROOT],
            os.environ.get("SPARK_WEBHOOK_SECRET") or None,
        )

    # -- submit ------------------------------------------------------------------------------

    def _audio_path(self, raw: str) -> Path:
        p = Path(raw).resolve()
        if not any(p.is_relative_to(root) for root in self.audio_roots):
            roots = ", ".join(str(r) for r in self.audio_roots)
            raise SparkError(f"{raw}: outside the audio roots ({roots})")
        if not p.is_file():
            raise SparkError(f"{raw}: not a file")
        return p

    def separate(
        self,
        files: list[str],
        model_type: str = "htdemucs",
        stems: list[str] | None = None,
        chunk_seconds: int | None = None,
    ) -> list[dict[str, Any]]:
        """Submit one job per track, skipping any the ledger already has or has pending.
        Blocking (uploads, HTTP): call it from a worker thread."""
        stems = stems or ["drums"]
        check_request(model_type, stems, chunk_seconds)
        tracks = [self._audio_path(f) for f in files]  # refuse the whole batch on one bad path
        known = self.ledger.records()
        uploaded = {r["sha256"]: r["audio"] for r in known.values()}
        out = []
        for path in tracks:
            sha = sha256_file(path)
            key = idempotency_key(sha, model_type, stems, chunk_seconds)
            rec = known.get(key)
            if rec is not None and rec.get("outcome") not in (FAILED, LOST):
                state = rec.get("outcome") or rec.get("state")
                out.append(
                    {
                        "path": str(path),
                        "action": "skipped",
                        "job_id": rec["job_id"],
                        "state": state,
                    }
                )
                continue
            audio = uploaded.get(sha) or self.client.upload(path)
            uploaded[sha] = audio
            payload: dict[str, Any] = {
                "audio": audio,
                "model_type": model_type,
                "stems": stems,
                "output_prefix": f"{STEMS_PREFIX}/{model_type}",
            }
            if chunk_seconds is not None:  # omitted = the engine's native segment
                payload["chunk_seconds"] = chunk_seconds
            resp = self.client.submit(
                {
                    "type": "separate",
                    "payload": payload,
                    "memory_estimate_gb": SEPARATE_MEMORY_GB,
                    "priority": PRIORITY,
                    "idempotency_key": key,
                    "name": f"blinky separate {path.stem}",
                }
            )
            row = {
                "path": str(path),
                "sha256": sha,
                "audio": audio,
                "model_type": model_type,
                "stems": stems,
                "chunk_seconds": chunk_seconds,
                "idempotency_key": key,
                "job_id": resp["id"],
                "state": resp.get("state"),
                "reattached": bool(resp.get("reattached")),
                "submitted_at": time.time(),
            }
            self.ledger.append(row)
            result = {
                "path": str(path),
                "action": "reattached" if row["reattached"] else "submitted",
                "job_id": resp["id"],
                "state": resp.get("state"),
            }
            # `priority` is what we got, not what we asked for (api.md, Submit): report a clamp.
            if resp.get("priority_clamped") or resp.get("priority") != PRIORITY:
                result["priority"] = resp.get("priority")
                result["priority_clamped"] = resp.get("priority_clamped")
                log.warning(
                    "spark: %s priority %s (clamped: %s)",
                    path.name,
                    resp.get("priority"),
                    resp.get("priority_clamped"),
                )
            out.append(result)
        return out

    # -- outcomes ----------------------------------------------------------------------------

    def _settle(
        self,
        rec: dict[str, Any],
        state: str,
        data: dict[str, Any] | None,
        error: str | None,
        finished_at: float | None,
    ) -> bool:
        """Record a terminal outcome once. False if this record already has one (a webhook is
        delivered at least once, and the poll may see the same job)."""
        if rec.get("outcome"):
            return False
        base = {
            "idempotency_key": rec["idempotency_key"],
            "job_id": rec["job_id"],
            "finished_at": finished_at,
            "at": time.time(),
        }
        stems = (data or {}).get("stems")
        if state == "succeeded" and isinstance(stems, dict) and set(stems) == set(rec["stems"]):
            # `chunk_seconds` in the result is the segment the engine USED: the provenance a
            # label set needs. The request may have been "native".
            row = {
                **base,
                "event": SUCCEEDED,
                "stems": stems,
                "chunk_seconds_used": (data or {}).get("chunk_seconds"),
            }
        elif state == "succeeded":
            row = {
                **base,
                "event": FAILED,
                "state": "failed",
                "error": f"result stems {stems!r} != requested {rec['stems']}",
            }
        else:
            row = {**base, "event": FAILED, "state": state, "error": error}
        self.ledger.append(row)
        rec["outcome"] = row["event"]
        level = logging.INFO if row["event"] == SUCCEEDED else logging.WARNING
        log.log(
            level,
            "spark: %s job %s %s %s",
            Path(rec["path"]).name,
            rec["job_id"],
            row.get("state", row["event"]),
            row.get("error") or "",
        )
        return True

    def reconcile(self) -> dict[str, int]:
        """One pass over the jobs with no recorded outcome. Blocking: run it in a thread."""
        counts: dict[str, int] = {}
        for rec in self.ledger.records().values():
            if rec.get("outcome"):
                continue
            try:
                j = self.client.job(rec["job_id"])
            except NotFound:
                self.ledger.append(
                    {
                        "event": LOST,
                        "idempotency_key": rec["idempotency_key"],
                        "job_id": rec["job_id"],
                        "at": time.time(),
                    }
                )
                log.warning("spark: job %s pruned before its outcome was recorded", rec["job_id"])
                counts[LOST] = counts.get(LOST, 0) + 1
                continue
            state = str(j.get("state", "?"))
            if state in TERMINAL:
                data = (j.get("result") or {}).get("data")
                self._settle(rec, state, data, j.get("error"), j.get("finished_at"))
            counts[state] = counts.get(state, 0) + 1
        return counts

    def on_webhook(self, body: dict[str, Any]) -> str:
        """Handle one webhook delivery (same shape as GET /jobs/{id}, but `result` is already
        the job's result.data). Idempotent. Returns what was done, for the log and the reply."""
        job_id, state = str(body.get("id") or ""), str(body.get("state") or "")
        rec = self.ledger.by_job_id(job_id)
        if rec is None:
            return "ignored: not a job in this server's ledger"
        if state not in TERMINAL:
            return f"ignored: {state} is not terminal"
        settled = self._settle(
            rec, state, body.get("result"), body.get("error"), body.get("finished_at")
        )
        return f"recorded {state}" if settled else "duplicate"

    def summary(self) -> dict[str, Any]:
        recs = list(self.ledger.records().values())
        counts: dict[str, int] = {}
        for r in recs:
            k = r.get("outcome") or r.get("state") or "?"
            counts[k] = counts.get(k, 0) + 1
        return {"counts": counts, "jobs": recs, "webhook": self.webhook_secret is not None}

    async def run_forever(self) -> None:
        while True:
            try:
                counts = await asyncio.to_thread(self.reconcile)
                if counts:
                    log.info("spark: reconcile %s", counts)
            except Exception:  # a bad pass must not kill the loop
                log.exception("spark: reconcile failed")
            await asyncio.sleep(RECONCILE_INTERVAL_S)


_service: SparkService | None = None
_disabled: str = "Spark client not started"


def start() -> SparkService | None:
    """Build the service from the environment. None (and a logged reason) when unconfigured."""
    global _service, _disabled
    try:
        _service = SparkService.from_env()
    except SparkError as e:
        _service, _disabled = None, str(e)
        log.info("spark: disabled (%s)", e)
    return _service


def set_service(svc: SparkService | None) -> None:
    global _service
    _service = svc


def get_service() -> SparkService | None:
    return _service


def disabled_reason() -> str:
    return _disabled
