"""Tests for scoring helpers — focused on the per-signal gap pipeline.

The scoring module has many responsibilities (onset F1, PLP metrics, latency
estimation). These tests exercise just the bits that PR 130 introduced:
per-signal onset-vs-non-onset stats in both frame and peak mode.
"""

from __future__ import annotations

from blinky_server.testing.scoring import score_device_run
from blinky_server.testing.types import (
    GroundTruth,
    GroundTruthHit,
    GroundTruthOnset,
    SignalFrame,
    TestData,
    TransientEvent,
)


def _mk_gt(onset_times: list[float], duration_ms: float = 2000.0) -> GroundTruth:
    return GroundTruth(
        pattern="test",
        duration_ms=duration_ms,
        hits=[
            GroundTruthHit(time=t, type="kick", strength=1.0, expect_trigger=True)
            for t in onset_times
        ],
        onsets=[GroundTruthOnset(time=t, strength=1.0) for t in onset_times],
    )


def _mk_frames(
    audio_start_ms: float,
    frames: list[tuple[float, dict[str, float]]],
) -> list[SignalFrame]:
    """Build SignalFrame list from (t_sec_from_audio_start, values_dict) tuples."""
    return [
        SignalFrame(
            timestamp_ms=audio_start_ms + t * 1000,
            activation=values.get("activation", 0.0),
            values={k: v for k, v in values.items() if k != "activation"},
        )
        for t, values in frames
    ]


def _transients_matching_gt(
    audio_start_ms: float, gt_onset_secs: list[float]
) -> list[TransientEvent]:
    """Return transients aligned to GT onsets so latency correction ≈ 0 ms.

    Without this, score_device_run falls back to the default 100 ms latency
    correction (no detections → can't estimate), which shifts every signal
    frame by 100 ms and breaks the onset-window classification in tests.

    estimate_audio_latency requires ≥3 matched offsets to return a non-None
    value, so we always emit at least 3 transients (repeating the GT onsets
    if fewer are provided).
    """
    out: list[TransientEvent] = []
    for t in gt_onset_secs:
        out.append(
            TransientEvent(
                timestamp_ms=audio_start_ms + t * 1000.0,
                type="onset",
                strength=0.9,
            )
        )
    # Duplicate the last onset into extra transients so we reach the
    # ≥3-offset threshold in estimate_audio_latency even for 2-onset GTs.
    if out:
        while len(out) < 3:
            out.append(
                TransientEvent(
                    timestamp_ms=out[-1].timestamp_ms + 1.0,  # same offset bucket
                    type="onset",
                    strength=0.9,
                )
            )
    return out


def test_signal_gaps_peak_mode_captures_onset_max() -> None:
    """Peak-mode keeps one sample per onset — the max within ±50 ms.

    Two GT onsets at t=0.5 and t=1.5. Three frames near each onset with rising
    then falling values for `sig`. Peak-mode should collect the max (3.0) from
    each onset window.
    """
    audio_start = 1_700_000_000_000.0
    frames = _mk_frames(
        audio_start,
        [
            # Non-onset frame well before first onset (for non_mean baseline)
            (0.1, {"sig": 0.1}),
            (0.2, {"sig": 0.1}),
            # Onset 1 window (t=0.5 ± 50 ms)
            (0.48, {"sig": 1.0}),
            (0.50, {"sig": 3.0}),
            (0.52, {"sig": 2.0}),
            # Non-onset gap
            (0.9, {"sig": 0.1}),
            (1.0, {"sig": 0.1}),
            # Onset 2 window (t=1.5 ± 50 ms)
            (1.48, {"sig": 1.5}),
            (1.50, {"sig": 3.0}),
            (1.52, {"sig": 2.5}),
            # Non-onset tail
            (1.9, {"sig": 0.1}),
        ],
    )
    gt_times = [0.5, 1.5]
    data = TestData(
        duration=2000.0,
        start_time=audio_start,
        transients=_transients_matching_gt(audio_start, gt_times),
        music_states=[],
        signal_frames=frames,
    )
    gt = _mk_gt(gt_times)
    score = score_device_run(data, audio_start, gt)

    peak_rows = [g for g in score.signals if g.mode == "peak"]
    assert len(peak_rows) == 1, "expected one peak-mode row for 'sig'"
    row = peak_rows[0]
    # Peak of each onset window = 3.0; mean over two onsets = 3.0
    assert row.onset_mean == 3.0
    assert row.n_onset == 2
    # Non-onset mean is pulled from frames outside onset windows (~0.1)
    assert abs(row.non_mean - 0.1) < 0.05
    # Effect size should be strongly positive (peak well above baseline)
    assert row.cohens_d > 0.5


def test_signal_gaps_frame_mode_vs_peak_mode() -> None:
    """Frame and peak modes both report but peak-mode |d| >= frame-mode |d|.

    Per PR 130's motivation for adding peak mode: frame mode dilutes sharp-
    attack signals because the ±50 ms window spans multiple post-attack frames
    with decaying values; peak mode takes the max of each window.
    """
    audio_start = 1_700_000_000_000.0
    # Each onset: one sharp spike (value 5) flanked by near-zero frames. Frame
    # mode averages spike+flanks = ~1.7; peak mode keeps the 5. Frame mode's
    # onset pool has significant variance (mix of spike + flanks) while peak
    # mode's onset pool is tightly clustered — we verify the gap (onset-mean
    # minus non-mean), which is a cleaner comparison than cohens_d here.
    frames = _mk_frames(
        audio_start,
        [
            (0.46, {"sig": 0.1}),
            (0.48, {"sig": 0.1}),
            (0.50, {"sig": 5.0}),
            (0.52, {"sig": 0.1}),
            (0.54, {"sig": 0.1}),
            (0.90, {"sig": 0.1}),
            (1.00, {"sig": 0.11}),  # tiny variance in non-onset pool
            (1.46, {"sig": 0.1}),
            (1.48, {"sig": 0.1}),
            (1.50, {"sig": 5.0}),
            (1.52, {"sig": 0.1}),
            (1.54, {"sig": 0.1}),
            (1.90, {"sig": 0.12}),
        ],
    )
    gt_times = [0.5, 1.5]
    data = TestData(
        duration=2000.0,
        start_time=audio_start,
        transients=_transients_matching_gt(audio_start, gt_times),
        music_states=[],
        signal_frames=frames,
    )
    gt = _mk_gt(gt_times)
    score = score_device_run(data, audio_start, gt)

    frame_rows = {g.signal: g for g in score.signals if g.mode == "frame"}
    peak_rows = {g.signal: g for g in score.signals if g.mode == "peak"}
    assert "sig" in frame_rows
    assert "sig" in peak_rows
    # Peak captures the max (5.0); frame averages the window (much less).
    assert peak_rows["sig"].onset_mean == 5.0
    assert frame_rows["sig"].onset_mean < 2.5
    # Gap (onset_mean - non_mean) should be larger in peak mode than frame mode —
    # frame mode dilutes the sharp attack; peak mode preserves the max.
    assert peak_rows["sig"].gap > frame_rows["sig"].gap


def test_signal_gaps_empty_when_no_onsets() -> None:
    """No GT onsets → no signal_gaps rows (nothing to compare against)."""
    audio_start = 1_700_000_000_000.0
    frames = _mk_frames(audio_start, [(0.5, {"sig": 1.0}), (1.0, {"sig": 1.0})])
    data = TestData(
        duration=2000.0,
        start_time=audio_start,
        transients=[],
        music_states=[],
        signal_frames=frames,
    )
    gt = _mk_gt([])  # no onsets
    score = score_device_run(data, audio_start, gt)
    assert score.signals == []


def test_signal_gaps_empty_when_no_frames() -> None:
    """No captured signal frames → no gap rows, but score itself still works."""
    audio_start = 1_700_000_000_000.0
    data = TestData(
        duration=2000.0,
        start_time=audio_start,
        transients=[],
        music_states=[],
        signal_frames=[],
    )
    gt = _mk_gt([0.5, 1.5])
    score = score_device_run(data, audio_start, gt)
    assert score.signals == []
    assert score.signal_frames_captured == 0
