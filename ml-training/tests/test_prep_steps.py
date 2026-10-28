"""spark_entry's prep progress -> STEP lines (#315). No torch: runs in the light CI step.

The Spark's watchdog counts only `STEP ` lines, so prep's own progress lines are what keep a
long prep alive, and they must carry real numbers.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

ML = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ML))
_spec = importlib.util.spec_from_file_location("spark_entry", ML / "spark_entry.py")
assert _spec and _spec.loader
entry = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(entry)


@pytest.mark.parametrize(
    "line,want",
    [
        ("  [train] 120/5000 files | 0 errors | 3s elapsed | ETA 9s", ("train files", 120, 5000)),
        ("  [val] 1/1 files | 0 errors", ("val files", 1, 1)),
        ("  [train] merged 3/40 shards", ("train merge", 3, 40)),
        ("Disk precheck: 2.3 GB free", None),
        ("  Resuming train: 40 files done, shard_idx=2", None),
        ("STEP 3/4", None),  # prep never speaks STEP itself
    ],
)
def test_prep_progress_lines(line: str, want: object) -> None:
    assert entry.prep_progress(line) == want


def steps(out: str) -> list[str]:
    return [ln for ln in out.splitlines() if ln.startswith("STEP ")]


def test_phases_get_their_own_totals_and_other_output_never_advances(
    capsys: pytest.CaptureFixture[str],
) -> None:
    s = entry.PrepSteps()
    s.line("  [train] 10/500 files | ...")  # new phase: printed at once
    s.line("  [train] 20/500 files | ...")  # same phase, inside the rate limit: held back
    s.line("some other prep output")  # repeats the last count, rate-limited
    s.line("  [train] merged 1/4 shards")  # new phase
    s.line("  [val] 3/90 files | ...")  # new phase
    assert steps(capsys.readouterr().out) == ["STEP 0/1", "STEP 10/500", "STEP 1/4", "STEP 3/90"]


def test_after_the_interval_progress_and_chatter_both_print(
    capsys: pytest.CaptureFixture[str],
) -> None:
    now = [0.0]
    s = entry.PrepSteps(clock=lambda: now[0])
    s.line("  [train] 10/500 files")
    now[0] = 61
    s.line("  [train] 20/500 files")
    now[0] = 122
    s.line("merging takes a while")  # liveness: the last count again, not a new one
    assert steps(capsys.readouterr().out) == ["STEP 0/1", "STEP 10/500", "STEP 20/500",
                                              "STEP 20/500"]
