"""Submit `separate` and training jobs, and record their outcomes from the webhook and a poll.

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

Training is a `gpu_task` (#305): `ml-training/` at HEAD as a code bundle, `spark_entry.py` as the
entry point, the corpus prefix as its one input. It preps the dataset on the Spark and trains; the
model comes back as artifacts under `outputs/<job>/model/`, which the store reaps after 7 days.
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
import os
import tempfile
import time
from pathlib import Path
from typing import Any

from .. import paths
from . import bundle
from .client import Client, NotFound, SparkError
from .ledger import FAILED, LOST, SUCCEEDED, Ledger

log = logging.getLogger(__name__)

# Long work submits at exactly 0, never below. The registry caps blinky at max_priority 0,
# which is a CEILING, not a floor: the box's eviction picks the LOWEST-priority running job
# first, so a "polite" negative priority makes our job the first victim of every memory
# event, and a long job then evict-requeue-resumes without ever finishing (my-spark
# plans/0238 §5a). Don't lower this to be nice.
PRIORITY = 0

# Admission reserves max(our declaration, the platform's per-type floor). The floor is the
# platform's MEASURED number (separate: 3.0 GiB from a 1.118 GiB GPU peak, my-spark #1540) and
# moves when they re-measure, so declare below it and let it govern: any larger number here
# holds budget no separation job uses. The field is required and must be > 0.
SEPARATE_MEMORY_GB = 0.5

# mel-band-roformer is in the platform's contract, but its engine refuses it at runtime
# (demucs 4.0.1 doesn't ship it, my-spark #1534). Add it here when the platform's image has it.
MODEL_TYPES = ("htdemucs",)

# The platform's stem set (my-spark models.SEPARATE_STEMS). Submit 422s anything else, but by then
# a batch has uploaded and queued the tracks before the bad request; refuse it up front instead.
STEMS = ("drums", "bass", "vocals", "other")

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

_REPO = Path(__file__).resolve().parents[3]
_DEFAULT_AUDIO_ROOT = _REPO / "blinky-test-player" / "music"

# -- training (gpu_task) ---------------------------------------------------------------------------

# The raw corpus in the Spark's store (my-spark #1545). Staged whole: training reads the audio,
# which is 6.0 of its 6.66 GiB, so a narrower prefix would save little.
CORPUS_PREFIX = "datasets/blinky-corpus/"
TRAIN_ENTRY_POINT = "spark_entry.py"
TRAIN_RUNTIME = "torch-cu130"

# The platform floors gpu_task at 8 G and reports the measured peak back (`gpu_peak_gib`), which
# is what this should follow. Over-declaring is the safe direction: an under-declared job OOMs.
TRAIN_MEMORY_GB = 12.0

# A finished model must be in the result; without it a "success" trained nothing usable.
TRAIN_REQUIRED_ARTIFACT = "best_model.pt"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def idempotency_key(sha: str, model_type: str, stems: list[str], chunk_seconds: int | None) -> str:
    chunk = "native" if chunk_seconds is None else chunk_seconds
    return f"sep:{sha}:{model_type}:{'+'.join(stems)}:{chunk}"


def entry_args(
    config: str,
    train_args: list[str],
    prep_args: list[str],
    audio_dir: str | None,
    labels_dir: str | None,
    augment: bool,
) -> list[str]:
    """spark_entry.py's argv. Corpus dirs are relative to the corpus input."""
    args = ["--config", config]
    if audio_dir:
        args += ["--audio-dir", audio_dir]
    if labels_dir:
        args += ["--labels-dir", labels_dir]
    if not augment:
        args.append("--no-augment")
    # `=`-joined: prep's own flags start with `-`, which argparse won't take as a separate value.
    args += [f"--prep-arg={a}" for a in prep_args]
    return [*args, "--", *train_args]


def train_key(code_sha256: str, args: list[str]) -> str:
    h = hashlib.sha256("\0".join(args).encode()).hexdigest()[:16]
    return f"train:{code_sha256}:{h}"


def check_request(model_type: str, stems: list[str], chunk_seconds: int | None) -> None:
    if model_type not in MODEL_TYPES:
        raise SparkError(f"model_type {model_type!r} is not available; use one of {MODEL_TYPES}")
    if not stems or len(set(stems)) != len(stems):
        raise SparkError(f"stems must be non-empty and unique: {stems}")
    bad = [s for s in stems if s not in STEMS]
    if bad:
        raise SparkError(f"unknown stem(s) {bad}; use any of {STEMS}")
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
        train_ledger: Ledger | None = None,
        repo: Path = _REPO,
    ) -> None:
        self.client = client
        self.ledger = ledger
        self.train_ledger = train_ledger or Ledger(ledger.path.with_name("train.jsonl"))
        self.audio_roots = [r.resolve() for r in audio_roots]
        self.webhook_secret = webhook_secret
        self.repo = repo

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
            Ledger(paths.data_dir() / "spark" / "train.jsonl"),
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

    def train(
        self,
        config: str,
        train_args: list[str] | None = None,
        prep_args: list[str] | None = None,
        audio_dir: str | None = "audio",
        labels_dir: str | None = None,
        augment: bool = True,
        memory_gb: float = TRAIN_MEMORY_GB,
    ) -> dict[str, Any]:
        """Bundle ml-training/ at HEAD and submit it as one gpu_task. The same code with the same
        arguments is one job: skipped while it is pending or succeeded, resubmitted after a
        failure. Blocking (git, upload, HTTP): call it from a worker thread."""
        if memory_gb <= 0:
            raise SparkError(f"memory_gb {memory_gb}: must be > 0")
        with tempfile.TemporaryDirectory(prefix="blinky-bundle-") as tmp:
            return self._train(
                bundle.build(self.repo, Path(tmp)),
                config,
                train_args or [],
                prep_args or [],
                audio_dir,
                labels_dir,
                augment,
                memory_gb,
            )

    def _train(
        self,
        b: bundle.Bundle,
        config: str,
        train_args: list[str],
        prep_args: list[str],
        audio_dir: str | None,
        labels_dir: str | None,
        augment: bool,
        memory_gb: float,
    ) -> dict[str, Any]:
        if config not in b.names:
            raise SparkError(f"{config} is not in ml-training/ at {b.commit[:12]}")
        args = entry_args(config, train_args, prep_args, audio_dir, labels_dir, augment)
        key = train_key(b.sha256, args)
        known = self.train_ledger.records()
        rec = known.get(key)
        if rec is not None and rec.get("outcome") not in (FAILED, LOST):
            return {
                "action": "skipped",
                "job_id": rec["job_id"],
                "state": rec.get("outcome") or rec.get("state"),
                "code_sha256": b.sha256,
                "commit": b.commit,
            }
        # inputs/ is never reaped, so a bundle uploaded once serves every job on that code.
        code = next(
            (r["code"] for r in known.values() if r["code_sha256"] == b.sha256), None
        ) or self.client.upload(b.path)
        resp = self.client.submit(
            {
                "type": "gpu_task",
                "payload": {
                    "code": code,
                    "entry_point": TRAIN_ENTRY_POINT,
                    "args": args,
                    "inputs": {"corpus": CORPUS_PREFIX},
                    "outputs": {"model": True},
                    "runtime": TRAIN_RUNTIME,
                },
                "memory_estimate_gb": memory_gb,
                "priority": PRIORITY,
                "idempotency_key": key,
                "name": f"blinky train {Path(config).stem} {b.commit[:8]}",
            }
        )
        row = {
            "kind": "train",
            "config": config,
            "args": args,
            "commit": b.commit,
            "code": code,
            "code_sha256": b.sha256,
            "memory_gb": memory_gb,
            "idempotency_key": key,
            "job_id": resp["id"],
            "state": resp.get("state"),
            "reattached": bool(resp.get("reattached")),
            "submitted_at": time.time(),
        }
        self.train_ledger.append(row)
        out = {
            "action": "reattached" if row["reattached"] else "submitted",
            "job_id": resp["id"],
            "state": resp.get("state"),
            "code_sha256": b.sha256,
            "commit": b.commit,
        }
        if resp.get("priority_clamped") or resp.get("priority") != PRIORITY:
            out["priority"] = resp.get("priority")
            out["priority_clamped"] = resp.get("priority_clamped")
        return out

    # -- outcomes ----------------------------------------------------------------------------

    def _ledgers(self) -> list[Ledger]:
        return [self.ledger, self.train_ledger]

    def _train_success(self, rec: dict[str, Any], result: dict[str, Any]) -> dict[str, Any]:
        """The fields a training success records, or an `error` when the result doesn't prove
        the job trained this code into a model."""
        data = result.get("data") or {}
        artifacts = [a for a in result.get("artifacts") or [] if isinstance(a, str)]
        if data.get("code_sha256") != rec["code_sha256"]:
            return {
                "error": f"result code_sha256 {data.get('code_sha256')!r} != submitted "
                f"{rec['code_sha256']}"
            }
        if not any(a.endswith("/" + TRAIN_REQUIRED_ARTIFACT) for a in artifacts):
            return {"error": f"result has no {TRAIN_REQUIRED_ARTIFACT}: {artifacts[:10]}"}
        return {
            "artifacts": artifacts,
            "gpu_peak_gib": data.get("gpu_peak_gib"),
            "gpu_reserved_gib": data.get("gpu_reserved_gib"),
        }

    def _settle(
        self,
        rec: dict[str, Any],
        state: str,
        result: dict[str, Any] | None,
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
        data = (result or {}).get("data")
        stems = (data or {}).get("stems")
        if rec.get("kind") == "train":
            got = self._train_success(rec, result or {}) if state == "succeeded" else {}
            if state == "succeeded" and "error" not in got:
                row = {**base, "event": SUCCEEDED, **got}
            elif state == "succeeded":
                row = {**base, "event": FAILED, "state": "failed", "error": got["error"]}
            else:
                row = {**base, "event": FAILED, "state": state, "error": error}
        elif state == "succeeded" and isinstance(stems, dict) and set(stems) == set(rec["stems"]):
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
        (self.train_ledger if rec.get("kind") == "train" else self.ledger).append(row)
        rec["outcome"] = row["event"]
        level = logging.INFO if row["event"] == SUCCEEDED else logging.WARNING
        log.log(
            level,
            "spark: %s job %s %s %s",
            rec.get("config") or Path(rec["path"]).name,
            rec["job_id"],
            row.get("state", row["event"]),
            row.get("error") or "",
        )
        return True

    def reconcile(self) -> dict[str, int]:
        """One pass over the jobs with no recorded outcome. Blocking: run it in a thread."""
        counts: dict[str, int] = {}
        for ledger, rec in ((lg, r) for lg in self._ledgers() for r in lg.records().values()):
            if rec.get("outcome"):
                continue
            try:
                j = self.client.job(rec["job_id"])
            except NotFound:
                ledger.append(
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
                self._settle(rec, state, j.get("result"), j.get("error"), j.get("finished_at"))
            counts[state] = counts.get(state, 0) + 1
        return counts

    def on_webhook(self, body: dict[str, Any]) -> str:
        """Handle one webhook delivery: the job record, the shape `GET /jobs/{id}` returns.
        Idempotent. Returns what was done, for the log and the reply.

        `result` is the job's whole result, `{"artifacts": [...], "data": {...}}`: the sender
        sends `job.result` (my-spark `webhooks._payload`). my-spark's webhooks.md says it is
        already `result.data`; that is wrong, and a reader trusting it would find no stems and
        record every success as a failure. Both shapes are accepted, so a fix to either side
        can't break this."""
        job_id, state = str(body.get("id") or ""), str(body.get("state") or "")
        rec = next((r for lg in self._ledgers() if (r := lg.by_job_id(job_id))), None)
        if rec is None:
            return "ignored: not a job in this server's ledger"
        if state not in TERMINAL:
            return f"ignored: {state} is not terminal"
        result = body.get("result")
        if isinstance(result, dict) and "data" not in result:
            result = {"data": result}  # the documented (wrong) shape: result.data itself
        settled = self._settle(
            rec,
            state,
            result if isinstance(result, dict) else None,
            body.get("error"),
            body.get("finished_at"),
        )
        return f"recorded {state}" if settled else "duplicate"

    def summary(self) -> dict[str, Any]:
        recs = list(self.ledger.records().values())
        counts: dict[str, int] = {}
        for r in recs:
            k = r.get("outcome") or r.get("state") or "?"
            counts[k] = counts.get(k, 0) + 1
        train = list(self.train_ledger.records().values())
        for r in train:
            k = r.get("outcome") or r.get("state") or "?"
            counts[k] = counts.get(k, 0) + 1
        return {
            "counts": counts,
            "jobs": recs,
            "train": train,
            "webhook": self.webhook_secret is not None,
        }

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
