"""Drum-hit scoring (#294): matching, the metrics, and their plumbing into device runs."""

from __future__ import annotations

import math
from pathlib import Path

from blinky_server.testing.drum_scoring import clean_kick_weighted, match_count, score_drums
from blinky_server.testing.scoring import format_score_summary, score_device_run
from blinky_server.testing.track_discovery import (
    discover_tracks,
    load_ground_truth,
    segment_ground_truth,
)
from blinky_server.testing.types import (
    DrumLabels,
    GroundTruth,
    GroundTruthHit,
    TestData,
    TransientEvent,
)

CORPUS = Path(__file__).resolve().parents[2] / "blinky-test-player" / "music" / "edm"

# A two-bar groove at 120 BPM: kick on 1 and 3, snare on 2 and 4, hats on the off-beats.
KICKS = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
SNARES = [1.5, 2.5, 3.5, 4.5, 5.5, 6.5]
HATS = [1.25, 1.75, 2.25, 2.75, 3.25, 3.75]
LABELS = DrumLabels(KICKS, SNARES, HATS)


def det(times: list[float]) -> list[tuple[float, float]]:
    return [(t, 1.0) for t in times]


def test_match_count_is_one_to_one_and_maximal() -> None:
    assert match_count([1.00, 1.01], [1.0], 0.05) == 1  # two detections, one label
    assert match_count([1.0], [0.98, 1.02], 0.05) == 1  # one detection, two labels
    # Greedy nearest would pair 1.04 with 1.06 and strand 1.0; the maximum matching is 2.
    assert match_count([1.04, 1.10], [1.0, 1.06], 0.05) == 2
    assert match_count([], [1.0], 0.05) == 0


def test_a_drum_only_detector_scores_perfectly_through_a_constant_lag() -> None:
    # 40 ms late throughout: a pipeline delay, removed before scoring.
    s = score_drums(det([t + 0.04 for t in sorted(KICKS + SNARES)]), LABELS, 7.0)
    assert s.lag_ms is not None and abs(s.lag_ms - 40) < 5
    assert s.kick_r50 == 1.0 and s.kick_r30 == 1.0 and s.snare_r50 == 1.0
    assert s.on_drum == 1.0 and s.drum_f == 1.0
    assert s.lone_hat == 0.0


def test_a_hat_detector_is_off_drum() -> None:
    # Fires on every lone hat and nothing else; its lag is given (no drums to estimate it on).
    s = score_drums(det(HATS), LABELS, 7.0, lag_ms=0.0)
    assert s.kick_r50 == 0.0 and s.on_drum == 0.0 and s.drum_f == 0.0
    assert s.lone_hat == 1.0


def test_kicks_only_and_spurious_detections() -> None:
    s = score_drums(det([*KICKS, 0.3, 0.6]), LABELS, 7.0, lag_ms=0.0)
    assert s.kick_r50 == 1.0 and s.snare_r50 == 0.0
    assert math.isclose(s.on_drum, 6 / 8)
    p, r = 6 / 8, 6 / 12
    assert math.isclose(s.drum_f, 2 * p * r / (p + r))
    assert s.provisional == ["snare_r50"]  # snare labels are unreliable until #284


def test_no_lag_estimate_is_nan_not_zero() -> None:
    s = score_drums([], LABELS, 7.0)
    assert s.lag_ms is None and math.isnan(s.kick_r50)
    assert s.as_dict()["kickR50"] is None


def test_a_zero_length_run_serialises() -> None:
    import json

    json.dumps(score_drums([], LABELS, 0.0).as_dict(), allow_nan=False)


def test_cleaning_needs_two_systems_within_70ms() -> None:
    kw = {
        "onsets": [
            {"time": 1.0, "type": "kick"},
            {"time": 2.0, "type": "kick"},
            {"time": 3.0, "type": "snare"},
        ]
    }
    cons = {
        "onsets": [
            {"time": 1.05, "systems": 3},
            {"time": 2.0, "systems": 1},
            {"time": 3.2, "systems": 5},
        ]
    }
    assert [o["time"] for o in clean_kick_weighted(kw, cons)] == [1.0]


def test_segment_shifts_drum_labels_with_the_seek() -> None:
    gt = GroundTruth(pattern="t", duration_ms=10_000, hits=[], onsets=[], drums=LABELS)
    seg = segment_ground_truth(gt, seek_sec=2.0, duration_ms=2000)
    assert seg.drums is not None
    assert seg.drums.kicks == [0.0, 1.0] and seg.drums.snares == [0.5, 1.5]


def test_the_corpus_has_drum_labels_and_they_load() -> None:
    tracks = discover_tracks(CORPUS)
    assert len(tracks) == 18 and all("drum_labels" in t for t in tracks)
    t = tracks[0]
    gt = load_ground_truth(t["ground_truth"], t.get("human_edits"), t["drum_labels"])
    assert gt.drums is not None and len(gt.drums.kicks) > 50 and gt.drums.dropped > 0


def test_device_runs_report_drums() -> None:
    start = 1_000_000.0
    gt = GroundTruth(
        pattern="t",
        duration_ms=7000,
        hits=[GroundTruthHit(time=t, type="onset", strength=1.0) for t in KICKS + SNARES],
        drums=LABELS,
    )
    data = TestData(
        duration=7000.0,
        start_time=start,
        transients=[
            TransientEvent(timestamp_ms=start + t * 1000 + 30, type="onset", strength=1.0)
            for t in KICKS
        ],
    )
    summary = format_score_summary(score_device_run(data, start, gt))
    assert summary["drums"]["kickR50"] == 1.0 and summary["drums"]["snareR50"] == 0.0
    no_drums = GroundTruth(pattern="t", duration_ms=7000, hits=gt.hits)
    assert format_score_summary(score_device_run(data, start, no_drums))["drums"] is None


def test_tuning_targets_drum_metrics_and_skips_missing_values() -> None:
    from blinky_server.testing.test_runner import _extract_metric

    assert _extract_metric({"drums": {"kickR50": 0.8}}, "kickR50") == 0.8
    assert _extract_metric({"drums": None}, "kickR50") is None  # track without drum labels
    assert _extract_metric({"drums": {"kickR50": None}}, "kickR50") is None  # no lag estimate
    assert _extract_metric({"onsetTracking": {"f1": 0.6}}, "onsetF1") == 0.6
