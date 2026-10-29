"""The release gate's beat-pace arithmetic (#258): beats counted vs the tempo's prediction."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from release_gate import beat_pace  # noqa: E402


def steady(bpm: float, seconds: int, start: int = 0, lag: float = 0.0) -> list:
    """One sample a second; `lag` = fraction of beats the phase falls behind."""
    return [(float(t), bpm, int(start + bpm / 60 * t * (1 - lag)) % 65536)
            for t in range(seconds + 1)]


def test_a_phase_that_keeps_absolute_time_matches_the_tempo() -> None:
    p = beat_pace(steady(125.0, 60))
    assert p["counted"] == 125 and p["expected"] == 125.0 and p["error"] == 0.0


def test_a_phase_that_falls_behind_is_measured() -> None:
    p = beat_pace(steady(125.0, 60, lag=0.2))  # 20 % of frames' phase lost under load
    assert p["counted"] == 100 and p["error"] == pytest.approx(0.2)


def test_the_uint16_beat_count_wraps() -> None:
    samples = steady(120.0, 30, start=65530)
    assert samples[-1][2] == 54  # the firmware's count wrapped past 65535
    assert beat_pace(samples)["counted"] == 60


def test_a_changing_tempo_is_integrated_over_time() -> None:
    samples = [(0.0, 120.0, 0), (30.0, 120.0, 60), (60.0, 180.0, 135)]
    p = beat_pace(samples)
    assert p["expected"] == pytest.approx(60 + 75)  # 30 s at 120, then 30 s ramping 120 -> 180


def test_no_tempo_or_too_few_samples_is_no_result() -> None:
    assert beat_pace([(0.0, 120.0, 0)]) is None
    assert beat_pace([(0.0, 0.0, 0), (10.0, 0.0, 0)]) is None
