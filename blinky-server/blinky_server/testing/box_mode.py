"""The test box's mode: ``queue`` or ``dev`` (docs/DEV_LOOP.md, "Box modes").

- ``queue`` (the default): the hardware runners (scripts/testbox/staging.py; the PR queue
  later) submit their work in order.
- ``dev``: the runners submit nothing, and the dev endpoints (``/api/dev/*``) flash and test
  build-service builds. ``POST /api/test/staging`` answers 409.

No expiry and no automatic transition (owner, 2026-11-25): leaving dev mode is a conscious
switch. So the work waiting behind it must be obvious: each runner reports what it holds back
(``PUT /api/box/queue/{runner}``), and that shows in ``GET /api/box/mode``, in the runners'
pending commit statuses, in ``dev.py status`` and at ``/metrics`` (``blinky_box_mode``,
``blinky_hw_queue_depth``, ``blinky_hw_queue_oldest_seconds``).

Switching to dev while a staging suite runs waits for it: the box is ``switching`` (still
``queue``, but the runners hold back and the suite route refuses), and becomes ``dev`` when the
suite ends. ``preempt`` cancels the suite instead (it stops at its next step boundary; a flash
in flight finishes) and puts its commit back in the queue: the runner sees its job among
``preempted`` and tests that head again. The state is a JSON file in the server's data dir, so
it survives restarts.
"""

from __future__ import annotations

import json
import logging
import time
from pathlib import Path
from typing import Any

from ..paths import data_dir

log = logging.getLogger(__name__)

FILE = "box-mode.json"
MODES = ("queue", "dev")
PREEMPTED_KEEP = 20
MAX_WAITING = 50

_cache: tuple[Path, dict[str, Any]] | None = None


class ModeError(RuntimeError):
    """A mode request that can't be done now (the route answers 409)."""


def _path() -> Path:
    return data_dir() / FILE


def _default() -> dict[str, Any]:
    return {
        "mode": "queue",
        "since": time.time(),
        "note": None,
        "switching": None,
        "waiting": {},
        "preempted": [],
    }


def _state() -> dict[str, Any]:
    """The state, read from the file once per data dir (tests change it), else the default."""
    global _cache
    path = _path()
    if _cache is not None and _cache[0] == path:
        return _cache[1]
    state = _default()
    try:
        loaded = json.loads(path.read_text())
        if isinstance(loaded, dict) and loaded.get("mode") in MODES:
            state.update(loaded)
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as e:
        # A corrupt file must not wedge the box in dev mode or crash the server: queue mode,
        # loudly (the runners carry on, which is what the box does without a mode at all).
        log.error("box mode: %s unreadable (%s); queue mode", path, e)
    _cache = (path, state)
    return state


def _save(state: dict[str, Any]) -> None:
    global _cache
    path = _path()
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n")
    tmp.replace(path)
    _cache = (path, state)


def waiting() -> list[dict[str, Any]]:
    """Every runner's waiting work, oldest first."""
    items = [
        {**item, "runner": runner} for runner, held in _state()["waiting"].items() for item in held
    ]
    return sorted(items, key=lambda i: float(i.get("since") or 0.0))


def snapshot() -> dict[str, Any]:
    """The state as it is now, without resolving a pending switch (for /metrics)."""
    st = _state()
    return {
        "mode": st["mode"],
        "since": st["since"],
        "note": st["note"],
        "switching": st["switching"],
        "waiting": waiting(),
        "preempted": list(st["preempted"]),
    }


async def _active_suites() -> list[str]:
    from ..work import durable, jobs
    from .workflows import TEST_JOBS

    if not durable.launched():
        return []
    return await jobs.active(TEST_JOBS, "staging")


async def current() -> dict[str, Any]:
    """The mode, after completing a switch to dev whose suite has ended."""
    st = _state()
    sw = st["switching"]
    # The same switch, still pending after the await: a PUT meanwhile may have called it off.
    if sw and not await _active_suites() and st["switching"] is sw:
        st.update(mode="dev", since=time.time(), switching=None)
        _save(st)
        log.info("box mode: dev (the staging suite it waited for has ended)")
    return snapshot()


def holds_queue() -> bool:
    """The runners hold back: dev mode, or switching to it."""
    st = _state()
    return bool(st["mode"] == "dev" or st["switching"])


def in_dev() -> bool:
    return bool(_state()["mode"] == "dev")


async def _suite_sha(job_id: str) -> str | None:
    from dbos import DBOS

    try:
        sts = await DBOS.list_workflows_async(workflow_ids=[job_id], load_output=False)
        req = sts[0].input["args"][0] if sts and sts[0].input else {}
        sha = req.get("sha") if isinstance(req, dict) else None
        return str(sha) if sha else None
    except Exception as e:  # the sha is for the queue's display; never fail the switch on it
        log.warning("box mode: no sha for preempted suite %s: %s", job_id, e)
        return None


async def set_mode(mode: str, note: str | None = None, preempt: bool = False) -> dict[str, Any]:
    """Switch. To dev with a staging suite running: wait for it (``switching``), or with
    ``preempt`` cancel it and queue its commit again. Call under routes_firmware._deploy_lock,
    so no suite starts between the check and the switch."""
    if mode not in MODES:
        raise ModeError(f"mode must be one of {MODES}, not {mode!r}")
    st = _state()
    now = time.time()
    if mode == "queue":
        if st["mode"] != "queue" or st["switching"]:
            log.info("box mode: queue%s", f" ({note})" if note else "")
            st.update(mode="queue", since=now, switching=None)
        st["note"] = note
        _save(st)
        return snapshot()
    if st["mode"] == "dev":
        st["note"] = note if note is not None else st["note"]
        _save(st)
        return snapshot()
    suites = await _active_suites()
    if suites and preempt:
        from dbos import DBOS

        for job in suites:
            sha = await _suite_sha(job)
            await DBOS.cancel_workflow_async(job)
            log.warning("box mode: dev preempts staging suite %s (%s)", job, (sha or "?")[:12])
            st["preempted"] = [*st["preempted"], {"job": job, "sha": sha, "at": now}][
                -PREEMPTED_KEEP:
            ]
            if sha:
                held = [i for i in st["waiting"].get("staging", []) if i.get("sha") != sha]
                st["waiting"]["staging"] = [*held, {"sha": sha, "since": now, "preempted": True}]
        suites = []
    st["note"] = note
    if suites:
        st["switching"] = {"to": "dev", "requested_at": now, "waiting_for": suites}
        log.info("box mode: switching to dev once staging suite %s ends", ", ".join(suites))
    else:
        st.update(mode="dev", since=now, switching=None)
        log.info("box mode: dev%s", f" ({note})" if note else "")
    _save(st)
    return snapshot()


def set_waiting(runner: str, items: list[dict[str, Any]]) -> dict[str, Any]:
    """What ``runner`` holds back now (replaces its previous list; empty clears it)."""
    st = _state()
    items = items[:MAX_WAITING]
    if st["waiting"].get(runner, []) != items:  # the staging runner reports every 2 min
        if items:
            st["waiting"][runner] = items
        else:
            st["waiting"].pop(runner, None)
        _save(st)
    return snapshot()


def describe(state: dict[str, Any]) -> str:
    """One line: the runners' pending status reads the same."""
    n = len(state["waiting"])
    if state["mode"] == "dev":
        when = time.strftime("%H:%M", time.localtime(float(state["since"])))
        return f"box in dev mode since {when} ({n} waiting)"
    if state.get("switching"):
        return f"box switching to dev mode ({n} waiting)"
    return f"box in queue mode ({n} waiting)"
