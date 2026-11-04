"""Repetition consistency (#340) on synthetic audio whose repeats are known."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from consistency import SR, consistency, find_repetition  # noqa: E402

LOOP_S = 2.0
HITS = [0.0, 0.5, 0.75, 1.0, 1.5]  # one bar's hits, seconds into the loop
N_LOOPS = 12


def looped(seed: int = 0) -> np.ndarray:
    """A 2 s bar of decaying tone bursts over a pad, repeated N_LOOPS times, plus faint noise."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(LOOP_S * SR)) / SR
    bar = 0.05 * np.sin(2 * np.pi * 220 * t) * (1 + 0.5 * np.sin(2 * np.pi * t / LOOP_S))
    for k, h in enumerate(HITS):
        d = t - h
        on = d >= 0
        f = 60.0 if k % 2 == 0 else 1800.0
        bar[on] += np.exp(-d[on] / 0.06) * np.sin(2 * np.pi * f * d[on])
    x = np.tile(bar, N_LOOPS)
    return x + 1e-3 * rng.standard_normal(len(x))


def detections(loops: range, strength=lambda loop, k: 1.0, jitter=lambda loop, k: 0.0):
    times, strengths = [], []
    for n in loops:
        for k, h in enumerate(HITS):
            times.append(n * LOOP_S + h + jitter(n, k))
            strengths.append(strength(n, k))
    return np.array(times), np.array(strengths)


@pytest.fixture(scope="module")
def rep():
    return find_repetition(looped())


def score(rep, times, strengths):
    return consistency(times, strengths, rep, 2.0, N_LOOPS * LOOP_S)


def test_the_repeat_lag_is_the_loop_length(rep) -> None:
    assert rep.lag_s / LOOP_S == pytest.approx(round(rep.lag_s / LOOP_S), abs=0.01)


def test_looped_audio_is_almost_all_repeated_region(rep) -> None:
    t, s = detections(range(N_LOOPS))
    assert score(rep, t, s)["coverage"] > 0.8


def test_audio_that_never_repeats_has_no_repeated_region() -> None:
    noise = np.random.default_rng(1).standard_normal(int(24 * SR)) * 0.1
    r = find_repetition(noise)
    t = np.arange(2.0, 24.0, 0.5)
    assert consistency(t, np.ones_like(t), r, 2.0, 24.0)["coverage"] < 0.1


def test_the_same_hits_every_bar_score_one(rep) -> None:
    out = score(rep, *detections(range(N_LOOPS)))
    assert out["rep"] == pytest.approx(1.0)
    assert out["rep_s"] == pytest.approx(1.0)


def test_a_hit_that_fires_only_in_alternate_bars_costs(rep) -> None:
    # Hit 2 fires in even bars only: in every pair of repeats it shows once.
    t, s = detections(range(N_LOOPS))
    keep = np.array([not (k == 2 and n % 2) for n in range(N_LOOPS) for k in range(len(HITS))])
    full = score(rep, t, s)
    flaky = score(rep, t[keep], s[keep])
    assert flaky["rep"] < full["rep"] - 0.1
    assert flaky["rep_s"] < full["rep_s"] - 0.1


def test_same_hits_at_alternating_strengths_cost_only_strength(rep) -> None:
    t, s = detections(range(N_LOOPS), strength=lambda n, k: 1.0 if n % 2 == 0 else 0.5)
    out = score(rep, t, s)
    assert out["rep"] == pytest.approx(1.0)
    assert out["rep_s"] == pytest.approx(0.5, abs=0.01)


def test_a_one_frame_shift_still_counts_as_the_same_hit(rep) -> None:
    t, s = detections(range(N_LOOPS), jitter=lambda n, k: 0.016 * (n % 2))
    assert score(rep, t, s)["rep"] == pytest.approx(1.0)


def test_no_detections_is_not_a_score(rep) -> None:
    out = score(rep, np.array([]), np.array([]))
    assert np.isnan(out["rep"]) and np.isnan(out["rep_s"])
