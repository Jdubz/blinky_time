"""The strict rules (testing/strict_rules.py): non-blocking warnings until the owner makes each
one block, and the readiness tracker."""

from __future__ import annotations

import dataclasses
import time
from typing import Any

import pytest

from blinky_server import telemetry
from blinky_server.device.device import Device
from blinky_server.device.protocol import DeviceProtocol
from blinky_server.testing import dev_run, hardware_checks, strict_rules

from .mock_transport import MockTransport

T0 = 1_800_000_000.0


def win(t: float, frame: int = 20, fps: float = 120.0, over: int = 0, ms: int | None = None,
        where: str = "health") -> dict[str, Any]:  # fmt: skip
    return {
        "t": t,
        "ms": int(ms if ms is not None else (t - T0) * 1000 + 60_000),
        "fps": fps,
        "max_frame_ms": frame,
        "overruns": over,
        "where": where,
    }


def rows(windows: list[dict[str, Any]], **kw: Any):
    return {r["rule"]: r for r in strict_rules.evaluate(windows, **kw)}


def block(monkeypatch: pytest.MonkeyPatch, name: str) -> None:
    """The owner's one-line switch, in a test: the rule's mode to BLOCK."""
    rules = tuple(
        dataclasses.replace(r, mode=strict_rules.BLOCK) if r.name == name else r
        for r in strict_rules.RULES
    )
    monkeypatch.setattr(strict_rules, "RULES", rules)
    monkeypatch.setattr(strict_rules, "BY_NAME", {r.name: r for r in rules})


def test_the_table_is_the_release_gates_limits_all_warning_today() -> None:
    got = {r.name: (r.threshold, r.mode) for r in strict_rules.RULES}
    assert got["frame_time"] == (100.0, "warn")  # release_gate --max-frame-ms
    assert got["fps"] == (60.0, "warn")  # release_gate --min-fps
    assert got["overrun_growth"] == (0.0, "warn")
    assert got["beat_pace"] == (0.03, "warn")
    assert {r.mode for r in strict_rules.RULES} == {"warn"}
    assert got["stream_dropped"] == (0.0, "warn")
    # blinky-firmware#23 (the stream stalls) is fixed: no rule waits on an open issue.
    assert strict_rules.BY_NAME["frame_time"].issues == ()


# -- frame time: every window counts --------------------------------------------------------


def test_every_window_counts_a_save_is_not_excused() -> None:
    """Since blinky-firmware#49 a settings save only writes flash: no window is left out, and
    the slowest one is the rule's value."""
    ws = [win(T0 + 5.0, frame=19), win(T0 + 12.9, frame=113), win(T0 + 60.0, frame=31)]
    frame = rows(ws)["frame_time"]
    assert frame["status"] == strict_rules.WARNED and frame["worst"] == 113
    assert frame["samples"] == 3 and "excluded" not in frame
    assert rows([])["frame_time"]["status"] == strict_rules.NO_DATA


def info(t: float, end_before_s: float, length_s: float, frame: int = 20) -> dict[str, Any]:
    """A ``json info`` from firmware that says when its window closed (blinky-firmware#80):
    read at host time ``t``, the window closed ``end_before_s`` earlier and ran ``length_s``."""
    now = 600_000
    return {
        "millis": now,
        "maxFrameMs": frame,
        "fps": 120.0,
        "frameWindowEndMs": now - round(end_before_s * 1000),
        "frameWindowMs": round(length_s * 1000),
    }


def test_the_reported_close_time_places_the_window_in_host_time() -> None:
    w = strict_rules.window(info(T0, 2.0, 5.2), T0, "health")
    assert (w["w_start"], w["w_end"]) == (round(T0 - 7.2, 3), round(T0 - 2.0, 3))
    # Older firmware, or no window closed yet: no span.
    assert "w_start" not in strict_rules.window({"millis": 600_000, "maxFrameMs": 20}, T0, "x")
    none_yet = {"millis": 900, "frameWindowEndMs": 0, "frameWindowMs": 0}
    assert strict_rules.window_span(none_yet, T0) is None


async def test_a_reconnected_protocol_still_records_alerts() -> None:
    """The fleet manager gives a reconnected device a new protocol: wire_protocol routes all of
    its callbacks, the alert record included."""
    d = Device("CHIP", "/dev/x", "nrf52840", MockTransport())
    t2 = MockTransport()
    d.protocol = DeviceProtocol(t2)
    d.wire_protocol()
    await t2.connect()
    d.protocol._handle_line("[ASSERT] after the reconnect")
    assert d.alerts[-1] == "[ASSERT] after the reconnect"


# -- the other rules ---------------------------------------------------------------------------


def test_stream_dropped_counts_skipped_lines_and_needs_the_field() -> None:
    def dropped(t: float, n: int | None, ms: int | None = None) -> dict[str, Any]:
        return {**win(t, ms=ms), "stream_dropped": n}

    clean = [dropped(T0, 0), dropped(T0 + 4, 0), dropped(T0 + 8, 0)]
    assert rows(clean)["stream_dropped"]["status"] == "pass"
    skipped = [dropped(T0, 0), dropped(T0 + 4, 3), dropped(T0 + 8, 5)]
    got = rows(skipped)["stream_dropped"]
    assert got["status"] == "warn" and got["worst"] == 5.0
    # Firmware before blinky-firmware#85 doesn't report it: no data, never a pass.
    old = [win(T0), win(T0 + 4)]
    assert rows(old)["stream_dropped"]["status"] == strict_rules.NO_DATA
    # Across a reboot the counter restarts, like audioOverruns.
    rebooted = [dropped(T0, 7), dropped(T0 + 4, 0, ms=1000), dropped(T0 + 8, 0, ms=5000)]
    assert rows(rebooted)["stream_dropped"]["worst"] == 0.0


def test_fps_overruns_and_music_rules() -> None:
    ws = [win(T0, fps=118.0, over=4), win(T0 + 4, fps=58.5, over=4), win(T0 + 8, fps=120, over=6)]
    got = rows(ws)
    assert got["fps"]["status"] == strict_rules.WARNED and got["fps"]["worst"] == 58.5
    assert got["overrun_growth"]["worst"] == 2.0 and got["overrun_growth"]["status"] == "warn"
    # Across a reboot the counter restarts: that pair is skipped (the reboot is judged
    # elsewhere), not read as a negative or huge growth.
    rebooted = [win(T0, over=9), win(T0 + 4, over=0, ms=1000), win(T0 + 8, over=0, ms=5000)]
    assert rows(rebooted)["overrun_growth"]["worst"] == 0.0
    music = {"level_mean": 0.3, "onsets_per_s": 0.8}
    pace = {"counted": 113, "expected": 115.0, "error": 0.017}
    got = rows(ws, music=music, pace=pace)
    assert got["music_level"]["status"] == "pass" and got["music_onsets"]["status"] == "warn"
    assert got["beat_pace"]["status"] == "pass"
    # Two beats either way is sampling at the window's ends, even past 3 %.
    short = {"counted": 48, "expected": 50.0, "error": 0.04}
    assert rows(ws, pace=short)["beat_pace"]["status"] == "pass"
    assert rows(ws, pace={"counted": 40, "expected": 50.0, "error": 0.2})["beat_pace"][
        "status"
    ] == ("warn")
    assert rows(ws)["beat_pace"]["status"] == strict_rules.NO_DATA


def test_the_summary_names_the_worst_device() -> None:
    out = strict_rules.report(
        {"A": [win(T0, frame=30), win(T0 + 4, frame=40)], "B": [win(T0, frame=150)]}
    )
    frame = next(r for r in out["rules"] if r["rule"] == "frame_time")
    assert frame["device"] == "B" and frame["worst"] == 150 and frame["status"] == "warn"
    assert set(out["devices"]) == {"A", "B"}


def test_a_warning_never_fails_a_run_and_a_blocking_rule_does(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    ws = {"C": [win(T0, frame=150), win(T0 + 4)]}
    warned = strict_rules.report(ws)
    assert strict_rules.failures(warned) == [] and len(strict_rules.warnings(warned)) == 1
    hw = {"rig_error": None, "plan": ["upload"], "phases": [{"name": "upload", "result": "yes"}]}
    assert hardware_checks.verdict({**hw, "strict": warned})
    assert hardware_checks.strict_checks(warned) == []
    assert dev_run.verdict({"checks": [], "final": {}, "strict": warned})

    block(monkeypatch, "frame_time")
    blocked = strict_rules.report(ws)
    (failed,) = strict_rules.failures(blocked)
    assert failed["rule"] == "frame_time" and failed["status"] == strict_rules.FAILED
    assert not hardware_checks.verdict({**hw, "strict": blocked})
    (c,) = hardware_checks.strict_checks(blocked)
    assert (c["phase"], c["name"], c["result"], c["device"]) == ("strict", "frame_time", "no", "C")
    assert not dev_run.verdict({"checks": [], "final": {}, "strict": blocked})


def test_the_metrics_carry_each_rule() -> None:
    strict_rules.judge({"C": [win(T0, frame=150), win(T0 + 4, fps=55)]}, "dev")

    def v(name: str, **labels: str) -> float | None:
        return telemetry.REGISTRY.get_sample_value(name, labels)

    assert v("blinky_strict_rule_pass", rule="frame_time", source="dev") == 0
    assert v("blinky_strict_rule_worst", rule="frame_time", source="dev") == 150
    assert v("blinky_strict_rule_worst", rule="fps", source="dev") == 55
    assert v("blinky_strict_rule_pass", rule="beat_pace", source="dev") is None  # no data
    assert v("blinky_strict_rule_blocking", rule="frame_time") == 0


# -- readiness ---------------------------------------------------------------------------------


def run(status: str, sha: str, at: float, rule: str = "frame_time", **kw: Any) -> None:
    strict = {"rules": [{"rule": rule, "status": status, "worst": 20.0}]}
    strict_rules.record_run(
        strict, source=kw.pop("source", "quality"), sha=sha, version=f"v-{sha}", at=at, **kw
    )


def test_ready_after_n_distinct_builds_in_a_row_with_no_warning_between() -> None:
    for n in range(4):
        run("pass", f"s{n}", T0 + n)
    r = strict_rules.readiness()["frame_time"]
    assert (r["streak"], r["ready"], r["needed"]) == (4, False, 5)
    run("pass", "s4", T0 + 4)
    r = strict_rules.readiness()["frame_time"]
    assert r["ready"] and [b["build"] for b in r["builds"]] == ["s4", "s3", "s2", "s1", "s0"]
    run("warn", "s5", T0 + 5)
    r = strict_rules.readiness()["frame_time"]
    assert r["streak"] == 0 and not r["ready"] and r["broken_by"]["sha"] == "s5"
    run("pass", "s6", T0 + 6)
    assert strict_rules.readiness()["frame_time"]["streak"] == 1
    assert (
        telemetry.REGISTRY.get_sample_value("blinky_strict_rule_streak", {"rule": "frame_time"})
        == 1
    )


def test_a_build_counts_once_and_any_warning_of_it_counts() -> None:
    """A PR head's hardware checks and the staging commit with its tree (build_of) are one
    build; a retest replaces its own line; a run without data counts neither way."""
    run("pass", "h1", T0, source="hardware-checks")
    run("pass", "m1", T0 + 1, build="h1")
    assert strict_rules.readiness()["frame_time"]["streak"] == 1
    run("warn", "h2", T0 + 2, source="hardware-checks")
    run("pass", "m2", T0 + 3, build="h2")
    assert strict_rules.readiness()["frame_time"]["streak"] == 0  # h2 warned once
    run("pass", "h2", T0 + 4, source="hardware-checks")  # its retest
    assert strict_rules.readiness()["frame_time"]["streak"] == 2
    assert len([r for r in strict_rules.records() if r["sha"] == "h2"]) == 1
    run("no data", "h3", T0 + 5)
    assert strict_rules.readiness()["frame_time"]["streak"] == 2


def test_n_is_configurable() -> None:
    for n in range(3):
        run("pass", f"s{n}", T0 + n)
    assert strict_rules.readiness(n=3)["frame_time"]["ready"]
    assert not strict_rules.readiness()["frame_time"]["ready"]


async def test_the_status_route(api_client: Any) -> None:
    run("pass", "s0", T0)
    resp = await api_client.get("/api/test/strict-rules")
    assert resp.status_code == 200
    got = resp.json()
    assert got["ready_after"] == 5
    assert [r["name"] for r in got["rules"]] == [r.name for r in strict_rules.RULES]
    assert got["readiness"]["frame_time"]["streak"] == 1
    assert got["readiness"]["fps"]["streak"] == 0


def test_the_clock_carries_rounded_milliseconds_into_the_second() -> None:
    t = float(int(T0)) + 59.9996
    assert strict_rules.clock(t) == time.strftime("%H:%M:%S", time.localtime(int(T0) + 60)) + ".000"
    assert strict_rules.clock(float(int(T0)) + 0.25).endswith(".250")
