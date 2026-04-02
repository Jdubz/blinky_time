"""In-memory async job tracker for long-running test operations.

Test endpoints return a job_id immediately. The job runs as an asyncio
background task. Clients poll GET /api/test/jobs/{id} for status and results.
"""

from __future__ import annotations

import asyncio
import logging
import time
import uuid
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from enum import StrEnum
from typing import Any

log = logging.getLogger(__name__)

PRUNE_AGE_S = 3600  # Remove completed/errored jobs after 1 hour


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
    created_at: float = field(default_factory=time.monotonic)
    completed_at: float | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "kind": self.kind,
            "status": self.status.value,
            "progress": self.progress,
            "progressMessage": self.progress_message,
            "result": self.result,
            "error": self.error,
            "age_s": round(time.monotonic() - self.created_at, 1),
        }


class JobManager:
    """Manages background test jobs."""

    def __init__(self) -> None:
        self._jobs: dict[str, Job] = {}
        self._tasks: dict[str, asyncio.Task[None]] = {}

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
        self._prune_old()

        job = Job(id=uuid.uuid4().hex[:12], kind=kind)
        self._jobs[job.id] = job

        async def _run() -> None:
            job.status = JobStatus.RUNNING
            try:
                job.result = await coro_factory(job)
                job.status = JobStatus.COMPLETE
            except Exception as e:
                job.status = JobStatus.ERROR
                job.error = str(e)
                log.exception("Job %s (%s) failed", job.id, kind)
            finally:
                job.completed_at = time.monotonic()
                job.progress = 100

        task = asyncio.create_task(_run())
        self._tasks[job.id] = task

        def _cleanup_task(_: asyncio.Task[None]) -> None:
            self._tasks.pop(job.id, None)

        task.add_done_callback(_cleanup_task)
        return job

    def get(self, job_id: str) -> Job | None:
        return self._jobs.get(job_id)

    def list_recent(self, limit: int = 20) -> list[Job]:
        jobs = sorted(self._jobs.values(), key=lambda j: j.created_at, reverse=True)
        return jobs[:limit]

    def _prune_old(self) -> None:
        """Remove completed/errored jobs older than PRUNE_AGE_S."""
        now = time.monotonic()
        to_remove = [
            jid
            for jid, job in self._jobs.items()
            if job.completed_at is not None and (now - job.completed_at) > PRUNE_AGE_S
        ]
        for jid in to_remove:
            del self._jobs[jid]
            self._tasks.pop(jid, None)
