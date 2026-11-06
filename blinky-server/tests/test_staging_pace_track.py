"""The staging suite's choice of the track the music and beat-pace checks continue (#370)."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Any

import pytest
from blinky_scoring.track_discovery import discover_tracks, load_track_manifest

from blinky_server.testing import audio_player, staging, workflows

EDM = Path(__file__).resolve().parents[2] / "corpus" / "edm"

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


def test_the_real_corpus_has_a_track_for_the_music_check() -> None:
    """The staging suite's own choice over edm/, with real lengths and seeks. The check #370
    shipped without: its labels have no durationMs, so every track read as 0 s and every suite
    failed music and beat-pace. CI installs ffmpeg so this runs there, as on the box."""
    ts = list(discover_tracks(str(EDM)))
    assert len(ts) == 18, "corpus/ submodule not checked out"
    manifest = load_track_manifest(str(EDM))
    for t in ts:
        t["seek"] = manifest.get(t["name"], {}).get("seekOffset", 0)
    assert staging.choose_pace_track(ts, staging.DEFAULTS, workflows.track_seconds) is not None


def test_track_seconds_is_the_audio_length() -> None:
    track = next(t for t in discover_tracks(str(EDM)) if t["name"] == "amapiano-vibez")
    assert 260 < workflows.track_seconds(track) < 266


@pytest.mark.parametrize(
    "result",
    [
        subprocess.CompletedProcess([], 1, "", "No such file"),
        subprocess.CompletedProcess([], 0, "N/A\n", ""),
        subprocess.CompletedProcess([], 0, "0.000000\n", ""),
    ],
)
def test_an_unreadable_length_fails_loudly(
    monkeypatch: pytest.MonkeyPatch, result: subprocess.CompletedProcess[str]
) -> None:
    monkeypatch.setattr(audio_player.subprocess, "run", lambda *a, **k: result)
    with pytest.raises(RuntimeError, match="ffprobe"):
        audio_player.audio_seconds("x.mp3")
