"""The staging suite's choice of the track the music and beat-pace checks continue (#370)."""

from __future__ import annotations

from typing import Any

from blinky_server.testing import staging

LIMITS = {**staging.DEFAULTS}  # 35 s validation + 15 s music + 60 s pace + 5 s = 115 s


def tracks(*specs: tuple[str, float, float]) -> tuple[list[dict[str, Any]], Any]:
    """[(name, seconds, seek)] -> the tracks and a seconds() lookup."""
    ts = [{"name": n, "seek": seek} for n, _, seek in specs]
    lengths = {n: s for n, s, _ in specs}
    return ts, lambda t: lengths[t["name"]]


def test_the_first_long_enough_track_is_chosen() -> None:
    ts, secs = tracks(("a", 120.0, 0.0), ("b", 200.0, 0.0))
    assert staging.choose_pace_track(ts, LIMITS, secs) == 0


def test_a_short_first_track_is_skipped() -> None:
    ts, secs = tracks(("short", 91.0, 0.0), ("long", 200.0, 0.0))
    assert staging.choose_pace_track(ts, LIMITS, secs) == 1


def test_the_seek_counts_against_the_room() -> None:
    # 130 s long, but played from 20 s: 110 s left, short of 115 s.
    ts, secs = tracks(("seeked", 130.0, 20.0), ("next", 116.0, 0.0))
    assert staging.choose_pace_track(ts, LIMITS, secs) == 1


def test_no_track_long_enough_fails_music_and_pace_and_the_report() -> None:
    ts, secs = tracks(("a", 91.0, 0.0), ("b", 100.0, 0.0))
    assert staging.choose_pace_track(ts, LIMITS, secs) is None
    music = staging.no_pace_track(["dev-a"])
    assert music["dev-a"]["music"]["pass"] is False
    assert music["dev-a"]["beatpace"]["pass"] is False
    report = {"deploy": {"pass": True}, "music": music, "validation": {"pass": True}}
    assert staging._passed(report) is False
