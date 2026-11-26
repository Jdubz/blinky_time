"""The staging suite's health step: it reads every loop window from the deploy on, and the
strict rules exclude only the windows that may hold the server's own settings save."""

from __future__ import annotations

import time
from typing import Any

import pytest

from blinky_server.device.device import Device
from blinky_server.testing import staging, strict_rules

from .mock_transport import MockTransport

health = staging.health.__wrapped__  # type: ignore[attr-defined]  # the step's body, no DBOS


async def test_health_reads_from_the_start_and_the_deploys_save_is_excluded_not_skipped(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The deploy's save stalls the window it lands in (113 ms on the box's first run): health
    reads it, and the frame-time rule lists it as excluded with the save's time. A real stall
    later in the run (the 1118 ms window of the first full run) is not excluded: a warning."""
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

    save = {"t": now - 2.0, "t_end": now - 1.9}  # restore + save, just before health
    rows = {r["rule"]: r for r in strict_rules.evaluate(ws, [save])}
    frame = rows["frame_time"]
    assert frame["status"] == strict_rules.WARNED  # the 1118 ms stall: never excluded
    assert frame["worst"] == 1118 and frame["samples"] == 3
    (ex,) = frame["excluded"]
    assert ex["max_frame_ms"] == 113
    assert ex["reason"] == f"settings save at {strict_rules.clock(now - 2.0)} (blinky-firmware#49)"
    # Without the stall the run passes: the save's window alone never warns.
    calm = [{**w, "max_frame_ms": 30} if w["max_frame_ms"] == 1118 else w for w in ws]
    assert {r["rule"]: r for r in strict_rules.evaluate(calm, [save])}["frame_time"][
        "status"
    ] == strict_rules.PASS


async def test_the_save_record_is_what_the_server_sent(monkeypatch: pytest.MonkeyPatch) -> None:
    """Every `save` the server sends a chip is recorded with its host time (the deploy's
    restore + save goes through the same protocol), and nothing else is."""
    t = MockTransport()
    d = Device("CHIP", "/dev/x", "nrf52840", t)
    await t.connect()
    before = time.time()
    await d.protocol.send_command("json info")
    await d.protocol.send_command("save")
    from blinky_server.device.device import saves_between

    got = saves_between(["CHIP"], before - 1, time.time() + 1)
    (s,) = got["CHIP"]
    assert before - 0.001 <= s["t"] <= s["t_end"] <= time.time() + 0.001  # stored to 1 ms
    assert saves_between(["OTHER"], before - 1, time.time() + 1) == {}
