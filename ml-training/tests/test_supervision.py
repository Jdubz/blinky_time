"""The Spark gpu_task contract helpers (#304). No torch: runs in the light CI step."""

from __future__ import annotations

import io
import os
import signal
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts import supervision as sv  # noqa: E402


class Clock:
    def __init__(self) -> None:
        self.t = 0.0

    def __call__(self) -> float:
        return self.t


def test_progress_is_rate_limited_but_forced_lines_always_print() -> None:
    out, clock = io.StringIO(), Clock()
    p = sv.Progress(100, out=out, min_interval_s=60, clock=clock)
    p.report(0)  # the first line always prints
    p.report(5)  # too soon
    clock.t = 61
    p.report(10)
    p.report(11, force=True)
    p.report(500, force=True)  # clamped: never past the total
    assert out.getvalue().splitlines() == ["STEP 0/100", "STEP 10/100", "STEP 11/100",
                                           "STEP 100/100"]
    assert all(line.startswith(sv.PROGRESS_TOKEN) for line in out.getvalue().splitlines())


def test_sigterm_sets_the_flag_and_exits_42(capsys: pytest.CaptureFixture[str]) -> None:
    old = signal.getsignal(signal.SIGTERM)
    try:
        pre = sv.Preemption().install()
        pre.exit_if_requested("before")  # nothing pending: returns
        os.kill(os.getpid(), signal.SIGTERM)
        assert pre.requested
        with pytest.raises(SystemExit) as e:
            pre.exit_if_requested("in epoch 3")
        assert e.value.code == sv.PREEMPTED_EXIT == 42
        assert "PREEMPTED" in capsys.readouterr().out
    finally:
        signal.signal(signal.SIGTERM, old)


def test_fingerprint_is_order_independent_and_sensitive() -> None:
    a = sv.fingerprint({"lr": 1e-3, "cfg": {"x": 1, "y": [1, 2]}})
    assert a == sv.fingerprint({"cfg": {"y": [1, 2], "x": 1}, "lr": 1e-3})
    assert a != sv.fingerprint({"lr": 2e-3, "cfg": {"x": 1, "y": [1, 2]}})


def test_resume_check(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    path = tmp_path / "training_checkpoint.pt"
    sv.check_resume({"fingerprint": "abc"}, "abc", path)
    with pytest.raises(sv.ResumeMismatch, match="different run"):
        sv.check_resume({"fingerprint": "abc"}, "abd", path)
    monkeypatch.delenv("SPARK_JOB_ID", raising=False)
    sv.check_resume({}, "abc", path)  # a pre-fingerprint checkpoint, locally: allowed
    monkeypatch.setenv("SPARK_JOB_ID", "j1")
    with pytest.raises(sv.ResumeMismatch, match="no run fingerprint"):
        sv.check_resume({}, "abc", path)


def test_paths_come_from_the_environment(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    for k in list(os.environ):
        if k.startswith("SPARK_"):
            monkeypatch.delenv(k)
    assert not sv.on_spark()
    assert sv.spark_input("data") is None and sv.spark_output("model") is None
    assert sv.checkpoint_dir(tmp_path) == tmp_path
    monkeypatch.setenv("SPARK_JOB_ID", "j1")
    monkeypatch.setenv("SPARK_INPUT_DATA", "/inputs/data")
    monkeypatch.setenv("SPARK_OUTPUT_MODEL", "/work/outputs/model")
    monkeypatch.setenv("SPARK_CHECKPOINT_DIR", "/work/checkpoints")
    assert sv.on_spark()
    assert sv.spark_input("data") == Path("/inputs/data")
    assert sv.spark_output("model") == Path("/work/outputs/model")
    assert sv.checkpoint_dir(tmp_path) == Path("/work/checkpoints")
