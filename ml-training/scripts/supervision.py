"""The Spark's `gpu_task` supervision contract, from the trainer's side (#304).

my-spark runs blinky's own entry point as a `gpu_task` (plans/0239) and enforces this, in
services/job-service/spark_jobs/workers/gpu_task_worker.py:

- **progress:** a `STEP n/total` line on stdout resets a no-progress watchdog (1800 s of
  silence and the job is killed). Exit 0 without ever printing one counts as a FAILURE.
- **checkpoints:** `$SPARK_CHECKPOINT_DIR` is the one directory a requeued attempt gets back.
- **preemption:** exit 42 means "preempted after checkpointing": the job is requeued and resumes.
  Any other non-zero exit is terminal.
- **paths:** inputs arrive as `$SPARK_INPUT_<NAME>` (read-only), outputs go INSIDE
  `$SPARK_OUTPUT_<NAME>` (never replace the directory: a symlink there is refused). The code
  tree is read-only, so nothing may be written beside it.

Off the Spark none of these variables are set and the trainer behaves as it always has.

**What eviction actually sends.** The contract says SIGTERM -> checkpoint -> 42, and the SIGTERM
handler here does that. But the platform's eviction path today `docker stop`s the *worker*
container and then `docker rm -f`s ours (launcher.stop), so a real eviction reaches this
process as SIGKILL. The per-epoch checkpoint is therefore what bounds the loss, not the
handler. Keep the checkpoint interval short for that reason.

No torch here, so the contract is testable where torch isn't installed.
"""

from __future__ import annotations

import hashlib
import json
import os
import signal
import sys
import time
from pathlib import Path
from typing import Any, Callable, TextIO

PREEMPTED_EXIT = 42
PROGRESS_TOKEN = "STEP "

# The watchdog kills after 1800 s without a STEP line. A STEP every minute at most keeps
# the job's event stream readable (each line is an event on the platform) and leaves the
# watchdog a 30x margin, so a slow validation pass can't trip it.
PROGRESS_MIN_INTERVAL_S = 60.0


def on_spark() -> bool:
    return bool(os.environ.get("SPARK_JOB_ID"))


def spark_input(name: str) -> Path | None:
    v = os.environ.get(f"SPARK_INPUT_{name.upper()}")
    return Path(v) if v else None


def spark_output(name: str) -> Path | None:
    v = os.environ.get(f"SPARK_OUTPUT_{name.upper()}")
    return Path(v) if v else None


def checkpoint_dir(default: Path) -> Path:
    """Where the resumable checkpoint lives: the Spark's preserved dir, else `default`."""
    v = os.environ.get("SPARK_CHECKPOINT_DIR")
    return Path(v) if v else default


class Progress:
    """`STEP n/total` lines, rate-limited, flushed. `report(force=True)` always prints, so
    the epoch boundary and the end of the run are never skipped."""

    def __init__(
        self,
        total: int,
        out: TextIO | None = None,
        min_interval_s: float = PROGRESS_MIN_INTERVAL_S,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.total = max(1, int(total))
        self.out = out
        self.min_interval_s = min_interval_s
        self.clock = clock
        self._last: float | None = None

    def report(self, step: int, force: bool = False) -> None:
        now = self.clock()
        if not force and self._last is not None and now - self._last < self.min_interval_s:
            return
        self._last = now
        step = min(max(0, int(step)), self.total)
        print(f"{PROGRESS_TOKEN}{step}/{self.total}", file=self.out or sys.stdout, flush=True)


class Preemption:
    """SIGTERM sets a flag; the training loop checks it between batches and exits 42.

    Only a flag, never work in the handler: a save in progress (atomic, but still a save)
    finishes, and the exit happens at a point where the last checkpoint is consistent. Under
    `--entrypoint python3` the trainer is PID 1 in its container, where an unhandled SIGTERM
    is ignored outright, so the handler is also what makes SIGTERM do anything at all.
    """

    def __init__(self) -> None:
        self.requested = False

    def install(self) -> Preemption:
        signal.signal(signal.SIGTERM, self._handle)
        return self

    def _handle(self, signum: int, frame: Any) -> None:
        self.requested = True

    def exit_if_requested(self, where: str) -> None:
        if self.requested:
            print(
                f"PREEMPTED (SIGTERM) {where}: exiting {PREEMPTED_EXIT}; the requeued attempt "
                f"resumes from the last checkpoint",
                flush=True,
            )
            sys.exit(PREEMPTED_EXIT)


def fingerprint(identity: dict[str, Any]) -> str:
    """A stable hash of everything a checkpoint's state depends on."""
    blob = json.dumps(identity, sort_keys=True, default=str, separators=(",", ":"))
    return hashlib.sha256(blob.encode()).hexdigest()


class ResumeMismatch(RuntimeError):
    pass


def check_resume(ckpt: dict[str, Any], expected: str, path: Path) -> None:
    """Refuse a checkpoint written for a different run, BEFORE any state is loaded.

    `load_state_dict` on the wrong checkpoint either fails on a shape (after the optimizer
    may already be half-loaded) or, worse, succeeds and silently continues someone else's
    run (my-spark #1535 found the order matters). A checkpoint with no fingerprint predates
    this check: accepted locally with a warning, refused on the Spark, where none can exist
    legitimately.
    """
    got = ckpt.get("fingerprint")
    if got is None:
        if on_spark():
            raise ResumeMismatch(f"{path} has no run fingerprint; refusing to resume it")
        print(f"  WARNING: {path} predates run fingerprints; resuming without checking it")
        return
    if got != expected:
        raise ResumeMismatch(
            f"{path} belongs to a different run (fingerprint {got[:12]} != {expected[:12]}): "
            f"the config, CLI overrides or dataset changed. Refusing to resume it; delete it "
            f"or use a fresh --output-dir to start over."
        )
