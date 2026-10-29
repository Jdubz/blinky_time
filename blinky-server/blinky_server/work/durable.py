"""Durable work: every long-running server job survives a restart (#325).

A restart can arrive at any moment: a deploy, a manual ``systemctl restart``, a reboot, a crash.
So a job is a DBOS workflow (https://docs.dbos.dev), recorded in a SQLite file under the
server's data dir, and its units of work are steps. DBOS records each step's result; after a
restart it replays the workflow, and completed steps return their recorded result instead of
running again. No job has resume code of its own: its workflow is its resume code.

The rules a workflow must follow (and why):

1. **The workflow body is deterministic.** It decides what to do next only from its arguments
   and its steps' results. Device I/O, file reads and the clock that feed a decision go in a
   step. DBOS raises ``DBOSStepNondeterminismError`` when a replay takes another path.
2. **A step is a non-interruptible unit, and runs at least once.** One that a crash cuts off
   runs again from its start, so it must be safe to repeat: self-contained, and check-then-act
   for anything that isn't (a flash checks the device's version before writing).
3. **Per-run setup lives in the workflow body, outside steps** (``session`` in the test
   runner): it runs again on every execution, the one after a restart included, so devices and
   locks are set up again. It is side effects only and never feeds a decision.
4. **Step results are small and serializable.** Bulk data goes to a file; the step returns
   its path.

Safe points are step boundaries. ``drain`` (on SIGTERM) stops new work, lets every step in
flight finish, and holds each workflow at its next boundary. The process then exits with those
workflows still PENDING, and DBOS recovers them when the server starts again, from the step
after the last one finished. A user's cancel stays a cancel: draining never cancels anything.

The application version is ``SCHEMA_VERSION``, not DBOS's default hash of the source: DBOS
recovers only workflows of the running version, and the hash would change with every deploy
and orphan every pending workflow. Bump it when a workflow's sequence of steps changes
incompatibly; a replay that diverges anyway fails loudly (rule 1).
"""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import functools
import logging
import time
import uuid
from collections.abc import Awaitable, Callable, Coroutine, Iterator
from pathlib import Path
from typing import Any, ParamSpec, TypeVar

from dbos import DBOS, DBOSConfig, SetWorkflowID

log = logging.getLogger(__name__)

SCHEMA_VERSION = "blinky-work-1"
DB_FILE = "work.sqlite"
# A step that hasn't finished after this long while draining is a hung step, not a long one
# (a track is at most a few minutes, a flash a few more): give up waiting, loudly. It runs
# again after the restart, as after a crash (rule 2).
DRAIN_MAX_S = 20 * 60
# How often drain reports progress to systemd, and how much more time it asks for each time.
DRAIN_TICK_S = 5.0
DRAIN_EXTEND_S = 30.0

P = ParamSpec("P")
R = TypeVar("R")


class Draining(RuntimeError):
    """The server is shutting down: no new work. Routes answer 503 with Retry-After."""


class NotRunning(RuntimeError):
    """The work store isn't launched (an app built without its lifespan, as in some tests)."""


_launched = False
_launched_at: float | None = None
_draining = False
# The tasks of workflows held at a boundary, so shutdown can stop them (running their cleanup,
# while the devices are still connected) without DBOS recording an outcome: they stay PENDING.
_held: set[asyncio.Task[Any]] = set()
# Every workflow executing in this process, for shutdown to stop.
_running: set[asyncio.Task[Any]] = set()
# True inside held work; asyncio tasks inherit it, so a flash task a step starts does too.
_inside_hold: contextvars.ContextVar[bool] = contextvars.ContextVar("inside_hold", default=False)
# Steps executing now, by a token, for drain to wait on and report.
_in_flight: dict[int, dict[str, Any]] = {}
_next_token = 0


_data_dir: Path | None = None


def configure(data_dir: Path) -> Path:
    """Create the DBOS instance (once per process), with its system database under data_dir."""
    global _data_dir
    _data_dir = data_dir
    _prune_job_files(data_dir)
    db = data_dir / DB_FILE
    DBOS(
        config=DBOSConfig(
            name="blinky-server",
            system_database_url=f"sqlite:///{db}",
            application_version=SCHEMA_VERSION,
            # DBOS logs every schema migration at INFO on first launch.
            log_level="WARNING",
        )
    )
    return db


async def launch() -> None:
    """Launch from the server's event loop. DBOS runs async workflows, recovered ones too, on
    the loop that launched it, which is the one the devices' connections belong to."""
    global _launched, _draining, _launched_at
    _draining = False
    _launched_at = time.monotonic()
    DBOS.launch()
    _launched = True
    await _retire_other_versions()


async def _retire_other_versions() -> None:
    """Cancel, loudly, unfinished workflows of another SCHEMA_VERSION. DBOS never recovers
    them, so they would stay "running" for good: blocking a new fleet deploy (one at a time)
    and catching a retried Spark request that attaches to them. A bump of SCHEMA_VERSION
    declares their recorded steps incompatible with this code; they are not resumed."""
    pending = await DBOS.list_workflows_async(
        status=["ENQUEUED", "PENDING", "DELAYED"], load_input=False, load_output=False
    )
    for st in pending:
        if st.app_version != SCHEMA_VERSION:
            log.error(
                "workflow %s (%s) is from schema %s, not %s: it can't resume on this code; "
                "cancelled. Run the job again.",
                st.workflow_id,
                st.name,
                st.app_version,
                SCHEMA_VERSION,
            )
            await DBOS.cancel_workflow_async(st.workflow_id)


async def shutdown() -> None:
    """After ``drain``, before the devices disconnect: stop the held workflows (their cleanup,
    such as stopping streams, runs now), then DBOS. Held workflows stay PENDING for recovery."""
    global _launched
    # After the drain, no step or held region is running, so every live workflow is at a safe
    # point: held at a boundary, or waiting (on the audio lock, say) before reaching one. Stop
    # them all, again while stopping one lets another start (it then stops at its boundary or
    # at its next await); they stay PENDING. After a drain that gave up, this also cuts off the
    # stragglers, which run again after the restart (rule 2).
    while live := [t for t in _running if not t.done()]:
        for t in live:
            t.cancel()
        await asyncio.gather(*live, return_exceptions=True)
    _held.clear()
    if _launched:
        DBOS.destroy()
        _launched = False


def launched() -> bool:
    return _launched


def since_launch() -> float:
    """Seconds since the store launched: the restart window recovered work runs in, while
    discovery is still reconnecting devices."""
    return time.monotonic() - _launched_at if _launched_at is not None else float("inf")


def draining() -> bool:
    return _draining


@contextlib.contextmanager
def hold(name: str, owner: str | None = None) -> Iterator[None]:
    """A non-interruptible region that a drain waits for: every step runs in one, and so does
    work that can't be a step at all, such as a flash (a write can't resume halfway). ``owner``
    names it in the drain's status; inside a workflow it defaults to the workflow id."""
    token = begin_hold(name, owner)
    inside = _inside_hold.set(True)
    try:
        yield
    finally:
        _inside_hold.reset(inside)
        end_hold(token)


def inside_hold() -> bool:
    """Whether this code runs inside held work (a step, or a ``hold``). The drain already waits
    for that work, so what it starts (a step's flash) must go ahead, not be refused."""
    return _inside_hold.get()


def begin_hold(name: str, owner: str | None = None) -> int:
    """``hold`` for a region that starts in one function and ends in another (a flash job is
    held from the moment it's created, before its task first runs). Pair with ``end_hold``."""
    global _next_token
    _next_token += 1
    _in_flight[_next_token] = {
        "workflow_id": owner if owner is not None else DBOS.workflow_id,
        "step": name,
        "since": time.time(),
    }
    return _next_token


def end_hold(token: int) -> None:
    _in_flight.pop(token, None)


def refuse_while_draining(what: str) -> None:
    """Raise Draining if the server is draining: for work that starts outside ``submit``."""
    if _draining:
        raise Draining(f"the server is restarting; {what} again shortly")


async def boundary() -> None:
    """A safe point. While draining, the workflow stops here, before its next step, and stays
    PENDING until the process exits."""
    if _draining:
        log.info("workflow %s held at a step boundary for the restart", DBOS.workflow_id)
        task = asyncio.current_task()
        if task is not None:
            _held.add(task)
        # Never resolved: the workflow waits here until shutdown stops it (it stays PENDING).
        await asyncio.get_running_loop().create_future()


def step(fn: Callable[P, Awaitable[R]]) -> Callable[P, Awaitable[R]]:
    """A non-interruptible, repeatable unit of a workflow (rules 2 and 4), with the boundary
    before it."""
    name = f"{fn.__module__}.{fn.__qualname__}"
    recorded = DBOS.step(name=name)(fn)

    @functools.wraps(fn)
    async def run(*args: P.args, **kwargs: P.kwargs) -> R:
        await boundary()
        with hold(name):
            return await recorded(*args, **kwargs)

    return run


# Every registered workflow's name. tests/test_durable_work.py requires a restart test for each.
WORKFLOWS: list[str] = []


def workflow(name: str) -> Callable[[Callable[P, Awaitable[R]]], Callable[P, Awaitable[R]]]:
    """A durable job. ``name`` is its registered name: renaming one orphans its pending runs."""

    def wrap(fn: Callable[P, Awaitable[R]]) -> Callable[P, Awaitable[R]]:
        WORKFLOWS.append(name)

        @functools.wraps(fn)
        async def body(*args: P.args, **kwargs: P.kwargs) -> R:
            task = asyncio.current_task()
            if task is not None:
                _running.add(task)
            try:
                await boundary()  # a workflow that starts while draining does nothing yet
                return await fn(*args, **kwargs)
            finally:
                if task is not None:
                    _running.discard(task)

        return DBOS.workflow(name=name)(body)

    return wrap


async def submit(
    fn: Callable[..., Coroutine[Any, Any, Any]], *args: Any, workflow_id: str | None = None
) -> str:
    """Start a workflow now; return its id (``workflow_id``, or a new one). Its record is
    written before it runs."""
    if not _launched:
        raise NotRunning("the durable work store isn't running")
    if _draining:
        raise Draining("the server is restarting; submit again shortly")
    workflow_id = workflow_id or uuid.uuid4().hex[:12]
    with SetWorkflowID(workflow_id):
        await DBOS.start_workflow_async(fn, *args)
    return workflow_id


# What jobs leave under the data dir, and how long it is kept: rerun markers (``rerun``) only
# while their workflow could still resume; validation raw captures (testing/workflows.py) and
# firmware copied for a fleet flash (routes_firmware.py) a month.
RERUN_MARKER_KEEP_S = 7 * 24 * 3600
JOB_FILES_KEEP_S = 30 * 24 * 3600
_KEPT = (
    ("work-attempts", RERUN_MARKER_KEEP_S),
    ("test-raw", JOB_FILES_KEEP_S),
    ("job-firmware", JOB_FILES_KEEP_S),
)


def _prune_job_files(data_dir: Path) -> None:
    """At start: remove what jobs left under the data dir once it is older than _KEPT says."""
    import shutil

    now = time.time()
    for name, keep_s in _KEPT:
        root = data_dir / name
        if not root.is_dir():
            continue
        for entry in root.iterdir():
            try:
                if entry.stat().st_mtime < now - keep_s:
                    if entry.is_dir():
                        shutil.rmtree(entry)
                    else:
                        entry.unlink()
            except OSError as e:
                log.warning("could not prune %s: %s", entry, e)


def rerun(key: str) -> bool:
    """Inside a step: True when this step already ran in this workflow and was cut off (rule
    2), so check-then-act code checks first. ``key`` names the step within its workflow.
    Outside a workflow, or before ``configure``, always False."""
    wid = DBOS.workflow_id
    if wid is None or _data_dir is None:
        return False
    marker = _data_dir / "work-attempts" / wid / key
    if marker.exists():
        return True
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.touch()
    return False


def in_flight() -> list[dict[str, Any]]:
    now = time.time()
    return [{**s, "running_s": round(now - s["since"], 1)} for s in _in_flight.values()]


async def drain(notify: Callable[[str], None], extend: Callable[[float], None]) -> bool:
    """Stop new work and wait for every step in flight to finish. True when none is left;
    False after DRAIN_MAX_S, with the stragglers logged (they run again after the restart).

    ``notify`` reports what it waits for (systemd STATUS=); ``extend`` asks systemd for more
    stop time (EXTEND_TIMEOUT_USEC=), so a long step holds the restart instead of being killed.
    """
    global _draining
    _draining = True
    deadline = time.monotonic() + DRAIN_MAX_S
    while _in_flight:
        steps = in_flight()
        what = "; ".join(
            f"{s['workflow_id']} {s['step'].rsplit('.', 1)[-1]} ({s['running_s']:.0f} s)"
            for s in steps
        )
        if time.monotonic() > deadline:
            log.error(
                "drain gave up after %d s with steps still running: %s. They run again "
                "after the restart.",
                DRAIN_MAX_S,
                what,
            )
            return False
        notify(f"draining: waiting for {what}")
        extend(DRAIN_EXTEND_S)
        await asyncio.sleep(DRAIN_TICK_S)
    notify("drained: every workflow is at a step boundary")
    return True
