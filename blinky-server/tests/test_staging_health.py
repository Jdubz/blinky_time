"""The staging suite's health step: it reads every loop window from the deploy on, and the
strict rules count every one of them."""

from __future__ import annotations

import time
from typing import Any

import pytest

from blinky_server.testing import staging, strict_rules


health = staging.health.__wrapped__  # type: ignore[attr-defined]  # the step's body, no DBOS


async def test_health_reads_from_the_start_and_every_window_counts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Health reads from right after the deploy, and the frame-time rule judges every window it
    read: the first one too (a save no longer stalls the loop, blinky-firmware#49), and a real
    stall (the 1118 ms window of the box's first full run) is a warning."""
    monkeypatch.setattr(staging, "HEALTH_POLL_S", 0.0)
    now = time.time()
    # Read times: right after the deploy, then 10 s apart (the poll, compressed).
    reads = iter([now, now + 11.0, now + 22.0, now + 33.0])
    frames = iter([113, 31, 1118, 30])
    t_mono = iter([0.0, 0.0, 0.0, 0.0, 99.0])  # the deadline, then three polls, then past it

    class Clock:  # staging's clock only: the event loop keeps the real one
        time = staticmethod(lambda: next(reads))
        monotonic = staticmethod(lambda: next(t_mono))

    monkeypatch.setattr(staging, "time", Clock)
    ms = iter(range(60_000, 200_000, 11_000))

    async def info(_device_id: str) -> dict[str, Any]:
        return {"millis": next(ms), "fps": 110.0, "maxFrameMs": next(frames), "audioOverruns": 0}

    monkeypatch.setattr(staging, "_info", info)
    out = await health(["chip"], {**staging.DEFAULTS, "stable_minutes": 1.0})
    ws = out["chip"]["windows"]
    assert [w["max_frame_ms"] for w in ws] == [113, 31, 1118, 30]
    assert out["chip"]["stability"] == {"pass": True, "rebooted": False, "overrun_growth": 0}

    rows = {r["rule"]: r for r in strict_rules.evaluate(ws)}
    frame = rows["frame_time"]
    assert frame["status"] == strict_rules.WARNED
    assert frame["worst"] == 1118 and frame["samples"] == 4
    # Without the late stall, the first window (113 ms, a stall right after the deploy) still
    # counts: nothing is excused.
    calm = [{**w, "max_frame_ms": 30} if w["max_frame_ms"] == 1118 else w for w in ws]
    calm_frame = {r["rule"]: r for r in strict_rules.evaluate(calm)}["frame_time"]
    assert calm_frame["worst"] == 113 and calm_frame["status"] == strict_rules.WARNED
