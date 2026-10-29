"""Test jobs as the API shows them, read from the durable work store (#325).

A job is a DBOS workflow (``testing/workflows.py``); its id is the workflow id. Status, result,
error and the partial results of a job still running or failed all come from DBOS's records,
so they survive a restart. Only the progress line is in memory: it is display, rebuilt as a
resumed job replays.
"""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

from dbos import DBOS

PREFIX = "test."
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
# The steps whose results are a job's partial results, by step name suffix.
RESULT_STEPS = ("validation_run", "sweep_run", "tune_run", "nn_capture")

_progress: dict[str, tuple[int, str]] = {}


def report(progress: int | None, message: str) -> None:
    """From inside a workflow: its progress (None keeps the percentage) and a line of text."""
    wid = DBOS.workflow_id
    if wid is None:
        return
    pct = progress if progress is not None else _progress.get(wid, (0, ""))[0]
    _progress[wid] = (pct, message)


def _seconds(ms: int | None) -> float | None:
    return ms / 1000 if ms is not None else None


async def _partial(workflow_id: str) -> list[Any]:
    steps = await DBOS.list_workflow_steps_async(workflow_id)
    out: list[Any] = []
    for s in steps:
        if s["function_name"].endswith(RESULT_STEPS) and s["error"] is None and s["output"]:
            o = s["output"]
            out.extend(o if isinstance(o, list) else [o])
    return out


async def _to_dict(st: Any, partial: bool) -> dict[str, Any]:
    status = STATUS.get(st.status, st.status.lower())
    wid = st.workflow_id
    pct, msg = _progress.get(wid, (0, ""))
    created = _seconds(st.created_at) or time.time()
    result = st.output if status == "complete" else None
    error = str(st.error) if st.error is not None else None
    if partial and status in ("running", "error", "cancelled"):
        got = await _partial(wid)
        if status != "running" and got:
            result = {"status": "partial", "partial_results": got}
        elif status == "running":
            result = {"status": "running", "partial_results": got} if got else None
    if status == "complete":
        pct, msg = 100, msg or "complete"
    return {
        "id": wid,
        "kind": st.name.removeprefix(PREFIX),
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


async def get(job_id: str) -> dict[str, Any] | None:
    st = await DBOS.get_workflow_status_async(job_id)
    if st is None or not st.name.startswith(PREFIX):
        return None
    return await _to_dict(st, partial=True)


async def list_recent(limit: int = 50) -> list[dict[str, Any]]:
    names = [f"{PREFIX}{k}" for k in ("validate", "param-sweep", "tune-threshold", "capture-nn")]
    sts = await DBOS.list_workflows_async(
        name=names, limit=limit, sort_desc=True, load_input=False, load_output=False
    )
    return [await _to_dict(st, partial=False) for st in sts]


def inline_raw(job: dict[str, Any]) -> dict[str, Any]:
    """A validation job with its runs' raw captures read back from their files, as the API
    returned them before they moved out of the step results (gate-(b) tooling reads them)."""
    res = job.get("result")
    if not isinstance(res, dict):
        return job
    for entry in res.get("results", []) + res.get("partial_results", []):
        paths = entry.pop("raw_capture_path", None) if isinstance(entry, dict) else None
        if paths:
            entry["raw_capture"] = {
                dev: json.loads(Path(p).read_text()) for dev, p in paths.items()
            }
    return job
