"""Persistent async job tracker for long-running test operations.

Test endpoints return a job_id immediately. The job runs as an asyncio
background task. Clients poll GET /api/test/jobs/{id} for status and results.

Completed jobs are persisted to disk as JSON files and survive server restarts.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

# Default directory for persisted job results.
# On blinkyhost this survives reboots. On dev machines it's ephemeral but
# that's fine — dev results are transient.
JOBS_DIR = Path("/var/lib/blinky-server/test-jobs")


class JobStatus(StrEnum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETE = "complete"
    ERROR = "error"


@dataclass
class Job:
    id: str
    kind: str  # "validate", "param-sweep", etc.
    status: JobStatus = JobStatus.PENDING
    progress: int = 0  # 0-100
    progress_message: str = ""
    result: dict[str, Any] | None = None
    error: str | None = None
    created_at: float = field(default_factory=time.time)
    completed_at: float | None = None

    # Partial results accumulated during the run. Written to disk after
    # each track so nothing is lost on crash/restart.
    partial_results: list[dict[str, Any]] = field(default_factory=list)
    _jobs_dir: Path | None = field(default=None, repr=False)

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "kind": self.kind,
            "status": self.status.value,
            "progress": self.progress,
            "progressMessage": self.progress_message,
            "result": self.result,
            "error": self.error,
            "created_at": self.created_at,
            "completed_at": self.completed_at,
            "age_s": round(time.time() - self.created_at, 1),
        }

    def append_result(self, entry: dict[str, Any]) -> None:
        """Append a partial result and persist to disk immediately.

        Called by the test runner after each track/step completes. The
        progress file is a sibling of the final result file with a
        .progress.json suffix. On crash, the next server restart can
        recover partial results from this file.
        """
        self.partial_results.append(entry)
        if self._jobs_dir is None:
            return
        try:
            path = self._jobs_dir / f"{self.id}.progress.json"
            data = {
                "id": self.id,
                "kind": self.kind,
                "status": self.status.value,
                "progress": self.progress,
                "progressMessage": self.progress_message,
                "created_at": self.created_at,
                "partial_results": self.partial_results,
                "updated_at": time.time(),
            }
            # Atomic write: tmp → rename
            tmp = path.with_suffix(".tmp")
            tmp.write_text(json.dumps(data, separators=(",", ":")))
            tmp.rename(path)
        except Exception as e:
            log.warning("Failed to persist progress for job %s: %s", self.id, e)


class JobManager:
    """Manages background test jobs with disk persistence."""

    def __init__(self, jobs_dir: Path | None = None) -> None:
        self._jobs: dict[str, Job] = {}
        self._tasks: dict[str, asyncio.Task[None]] = {}
        self._jobs_dir = jobs_dir or JOBS_DIR

        # Ensure directory exists (fall back to /tmp if no write access)
        try:
            self._jobs_dir.mkdir(parents=True, exist_ok=True)
        except PermissionError:
            self._jobs_dir = Path("/tmp/blinky-test-jobs")
            self._jobs_dir.mkdir(parents=True, exist_ok=True)
            log.warning("Using fallback jobs dir: %s", self._jobs_dir)

        self._load_persisted()
        self._prune_old_files()

    def _load_persisted(self) -> None:
        """Load completed and interrupted job results from disk on startup.

        Loads both final results (*.json) and progress files (*.progress.json).
        Progress files represent interrupted jobs — their partial results are
        preserved as the job result with status "partial".
        """
        loaded = 0
        for path in sorted(self._jobs_dir.glob("*.json")):
            if path.name.endswith(".progress.json") or path.name.endswith(".tmp"):
                continue
            try:
                data = json.loads(path.read_text())
                job = Job(
                    id=data["id"],
                    kind=data.get("kind", "unknown"),
                    status=JobStatus(data.get("status", "complete")),
                    progress=data.get("progress", 100),
                    result=data.get("result"),
                    error=data.get("error"),
                    created_at=data.get("created_at", path.stat().st_mtime),
                    completed_at=data.get("completed_at"),
                )
                self._jobs[job.id] = job
                loaded += 1
            except Exception as e:
                log.warning("Failed to load job %s: %s", path.name, e)

        # Load interrupted jobs from progress files (no final result file)
        recovered = 0
        for path in sorted(self._jobs_dir.glob("*.progress.json")):
            job_id = path.name.replace(".progress.json", "")
            if job_id in self._jobs:
                # Final result exists, progress file is stale — clean up
                path.unlink(missing_ok=True)
                continue
            try:
                data = json.loads(path.read_text())
                partial = data.get("partial_results", [])
                job = Job(
                    id=data["id"],
                    kind=data.get("kind", "unknown"),
                    status=JobStatus.ERROR,
                    progress=data.get("progress", 0),
                    progress_message="interrupted (recovered partial results)",
                    result={"status": "partial", "partial_results": partial},
                    error="Job interrupted (server restart)",
                    created_at=data.get("created_at", path.stat().st_mtime),
                    completed_at=data.get("updated_at"),
                )
                self._jobs[job.id] = job
                recovered += 1
            except Exception as e:
                log.warning("Failed to load progress file %s: %s", path.name, e)

        if loaded or recovered:
            log.info(
                "Loaded %d completed + %d recovered jobs from %s",
                loaded,
                recovered,
                self._jobs_dir,
            )

    def _prune_old_files(self, keep: int = 200) -> None:
        """Delete oldest persisted job files if more than `keep` exist."""
        files = sorted(self._jobs_dir.glob("*.json"), key=lambda p: p.stat().st_mtime)
        if len(files) <= keep:
            return
        to_remove = files[: len(files) - keep]
        for path in to_remove:
            job_id = path.stem
            self._jobs.pop(job_id, None)
            path.unlink(missing_ok=True)
        log.info("Pruned %d old job files (kept %d)", len(to_remove), keep)

    def _persist(self, job: Job) -> None:
        """Write a completed/errored job to disk."""
        try:
            path = self._jobs_dir / f"{job.id}.json"
            path.write_text(json.dumps(job.to_dict(), separators=(",", ":")))
        except Exception as e:
            log.warning("Failed to persist job %s: %s", job.id, e)

    def submit(
        self,
        kind: str,
        coro_factory: Callable[[Job], Coroutine[Any, Any, dict[str, Any]]],
    ) -> Job:
        """Submit a new background job.

        Args:
            kind: Job type label (e.g., "validate", "param-sweep")
            coro_factory: Async function that takes the Job (for progress updates)
                          and returns a result dict.

        Returns:
            The created Job (status=PENDING, will transition to RUNNING).
        """
        job = Job(id=uuid.uuid4().hex[:12], kind=kind, _jobs_dir=self._jobs_dir)
        self._jobs[job.id] = job

        async def _run() -> None:
            job.status = JobStatus.RUNNING
            try:
                job.result = await coro_factory(job)
                job.status = JobStatus.COMPLETE
            except Exception as e:
                job.status = JobStatus.ERROR
                job.error = str(e)
                # Preserve partial results on failure
                if job.partial_results:
                    job.result = {"status": "partial", "partial_results": job.partial_results}
                log.exception("Job %s (%s) failed", job.id, kind)
            finally:
                job.completed_at = time.time()
                job.progress = 100
                self._persist(job)
                # Clean up progress file now that final result is written
                progress_path = self._jobs_dir / f"{job.id}.progress.json"
                progress_path.unlink(missing_ok=True)

        task = asyncio.create_task(_run())
        self._tasks[job.id] = task

        def _cleanup_task(_: asyncio.Task[None]) -> None:
            self._tasks.pop(job.id, None)

        task.add_done_callback(_cleanup_task)
        return job

    def get(self, job_id: str) -> Job | None:
        return self._jobs.get(job_id)

    def list_recent(self, limit: int = 50) -> list[Job]:
        jobs = sorted(self._jobs.values(), key=lambda j: j.created_at, reverse=True)
        return jobs[:limit]
