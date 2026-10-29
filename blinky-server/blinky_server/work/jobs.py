"""Durable jobs as the API shows them, read from the work store (#325).

A job is a DBOS workflow; its id is the workflow id, its kind the workflow's name without its
family prefix (``test.validate`` is a ``validate`` job of the ``test.`` family). Status, result,
error and the partial results of a job still running or failed all come from DBOS's records,
so they survive a restart. Only the progress line is in memory: it is display, rebuilt as a
resumed job replays.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any

from dbos import DBOS

# DBOS status -> the API's (the values the MCP tools and the deployer read).
STATUS = {
    "ENQUEUED": "pending",
    "DELAYED": "pending",
    "PENDING": "running",
    "SUCCESS": "complete",
    "ERROR": "error",
    "CANCELLED": "cancelled",
    "MAX_RECOVERY_ATTEMPTS_EXCEEDED": "error",
}


@dataclass(frozen=True)
class Family:
    """A family of jobs: its workflow-name prefix, its kinds, and the steps whose results are
    its jobs' partial results (by step-name suffix)."""

    prefix: str
    kinds: tuple[str, ...]
    result_steps: tuple[str, ...]


_progress: dict[str, tuple[int, str]] = {}
# Progress is display only (rebuilt as a resumed job replays): the newest this many are kept.
PROGRESS_KEEP = 500


def report(progress: int | None, message: str) -> None:
    """From inside a workflow: its progress (None keeps the percentage) and a line of text."""
    wid = DBOS.workflow_id
    if wid is None:
        return
    pct = progress if progress is not None else _progress.get(wid, (0, ""))[0]
    _progress.pop(wid, None)  # re-insert: the dict's order is most recently reported last
    _progress[wid] = (pct, message)
    while len(_progress) > PROGRESS_KEEP:
        del _progress[next(iter(_progress))]


class Progress:
    """``job.progress`` / ``job.progress_message`` for code written against the old in-memory
    job object (deploy_pipeline.run_deploy): assignments become ``report`` calls."""

    def __setattr__(self, name: str, value: Any) -> None:
        if name == "progress":
            report(int(value), _progress.get(str(DBOS.workflow_id), (0, ""))[1])
        elif name == "progress_message":
            report(None, str(value))
        else:
            object.__setattr__(self, name, value)


def _seconds(ms: int | None) -> float | None:
    return ms / 1000 if ms is not None else None


async def _partial(family: Family, workflow_id: str) -> list[Any]:
    steps = await DBOS.list_workflow_steps_async(workflow_id)
    out: list[Any] = []
    for s in steps:
        if s["function_name"].endswith(family.result_steps) and s["error"] is None and s["output"]:
            o = s["output"]
            out.extend(o if isinstance(o, list) else [o])
    return out


async def _to_dict(family: Family, st: Any, partial: bool) -> dict[str, Any]:
    status = STATUS.get(st.status, st.status.lower())
    wid = st.workflow_id
    pct, msg = _progress.get(wid, (0, ""))
    created = _seconds(st.created_at) or time.time()
    result = st.output if status == "complete" else None
    error = str(st.error) if st.error is not None else None
    if partial and status in ("running", "error", "cancelled"):
        got = await _partial(family, wid)
        if status != "running" and got:
            result = {"status": "partial", "partial_results": got}
        elif status == "running":
            result = {"status": "running", "partial_results": got} if got else None
    if status == "complete":
        pct, msg = 100, msg or "complete"
    return {
        "id": wid,
        "kind": st.name.removeprefix(family.prefix),
        "status": status,
        "progress": pct,
        "progressMessage": msg,
        "result": result,
        "error": error,
        "created_at": created,
        "completed_at": _seconds(st.completed_at),
        "age_s": round(time.time() - created, 1),
        "durable": True,
    }


async def get(family: Family, job_id: str) -> dict[str, Any] | None:
    st = await DBOS.get_workflow_status_async(job_id)
    if st is None or not st.name.startswith(family.prefix):
        return None
    return await _to_dict(family, st, partial=True)


async def list_recent(family: Family, limit: int = 50) -> list[dict[str, Any]]:
    sts = await DBOS.list_workflows_async(
        name=[f"{family.prefix}{k}" for k in family.kinds],
        limit=limit,
        sort_desc=True,
        load_input=False,
        load_output=False,
    )
    return [await _to_dict(family, st, partial=False) for st in sts]


async def active(family: Family, kind: str) -> list[str]:
    """Ids of this kind's jobs not yet finished (queued, running, or held for a restart)."""
    sts = await DBOS.list_workflows_async(
        name=f"{family.prefix}{kind}",
        status=["ENQUEUED", "PENDING", "DELAYED"],
        load_input=False,
        load_output=False,
    )
    return [st.workflow_id for st in sts]
