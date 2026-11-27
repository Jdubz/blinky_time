"""What the stable proposals read from a staging quality suite (docs/DEV_LOOP.md, "Stable
candidates from scores"): the tempo score, the health record, and one score line per run."""

from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from blinky_server.testing import repetition, staging, tempo


def states(periods: list[int | None], start_ms: float = 0.0) -> list[Any]:
    return [
        SimpleNamespace(timestamp_ms=start_ms + 16 * i, plp_period=p) for i, p in enumerate(periods)
    ]


def test_tempo_is_the_median_period_of_the_second_half_as_bpm() -> None:
    # First half locking on at a wrong period, second half at 31 frames (121 BPM at 62.5 Hz).
    got = tempo.tempo_score(states([45] * 50 + [31] * 50), 120.0)
    assert got["bpm"] == pytest.approx(120.97, abs=0.01)
    assert got["err"] == pytest.approx(0.0081, abs=1e-4)
    assert got["acc2"] is True and got["frames"] == 50


@pytest.mark.parametrize("bpm_period,ref", [(62, 120.0), (16, 120.0), (31, 60.0), (21, 60.0)])
def test_half_double_and_triple_time_are_equally_valid(bpm_period: int, ref: float) -> None:
    # 62 frames = 60.5 BPM (half of 121), 16 = 234 (double), 21 = 178.6 (triple of 60).
    got = tempo.tempo_score(states([bpm_period] * 60), ref)
    assert got["err"] < 0.04, got


def test_a_wrong_tempo_reads_as_its_distance_to_the_nearest_level() -> None:
    # 150 BPM against 120: 25% off 120, the nearest of 40, 60, 120, 240 and 360.
    got = tempo.tempo_score(states([25] * 60), 120.0)
    assert got["acc2"] is False
    assert got["err"] == pytest.approx(0.25, abs=1e-3)


def test_no_reference_or_too_few_frames_is_absent_with_the_reason_never_zero() -> None:
    assert tempo.tempo_score(states([31] * 60), None) == {
        "bpm": None,
        "why": "no ground-truth tempo for this track",
    }
    got = tempo.tempo_score(states([31] * 10 + [None] * 50), 120.0)
    assert got["bpm"] is None and "frames with a period" in got["why"]


def test_the_reference_is_the_pattern_files_ground_truth_bpm(tmp_path: Path) -> None:
    (tmp_path / "t.pattern_gt.json").write_text(json.dumps({"groundTruthBpm": 115.4}))
    assert tempo.ground_truth_bpm(tmp_path / "t.mp3") == 115.4
    assert tempo.ground_truth_bpm(tmp_path / "missing.mp3") is None


def report(**extra: Any) -> dict[str, Any]:
    scores = {
        "CHIP": {
            "drums": {"drumF": 0.6, "kickR50": 0.9, "lagSource": "measured"},
            "tempo": {"bpm": 121.0, "err": 0.008},
            "repetition": {"repS": 0.71, "rep": 0.8, "coverage": 0.6, "n": 90, "lagS": 1.875},
        }
    }
    return {
        "sha": "s" * 40,
        "build": "h" * 40,
        "run": "job1",
        "version": "b300-dev-ssssssss",
        "corpus": "c" * 40,
        "devices": ["CHIP"],
        "limits": {"stable_minutes": 5.0},
        "deploy": {"pass": True},
        "validation": {"results": [{"track": "a", "run": 1, "scores": scores}]},
        "health": {"CHIP": {"stability": {"pass": True, "rebooted": False}}},
        "final": {"CHIP": {"no_reboot": {"pass": True}}},
        "strict": {
            "rules": [
                {"rule": "frame_time", "mode": "block", "status": "pass", "worst": 18.0},
                {"rule": "beat_pace", "mode": "warn", "status": "warn", "worst": 0.05},
            ]
        },
        "passed": True,
        **extra,
    }


def test_the_score_line_has_the_run_build_tempo_and_health() -> None:
    rec = staging.score_record(report())
    assert rec["run"] == "job1" and rec["build"] == "h" * 40 and "release" not in rec
    row = rec["tracks"][0]
    assert row["drumF"] == 0.6 and row["tempoErr"] == 0.008 and row["tempoBpm"] == 121.0
    assert row["repS"] == 0.71 and row["repN"] == 90 and row["repCoverage"] == 0.6
    assert rec["health"] == {
        "passed": True,
        "rebooted": False,
        "strict": {
            "frame_time": {"status": "pass", "worst": 18.0, "mode": "block"},
            "beat_pace": {"status": "warn", "worst": 0.05, "mode": "warn"},
        },
    }


def _map(tmp_path: Path, regions: list[list[int]]) -> str:
    # A 2 s repeat over 60 s of 8 ms frames.
    p = tmp_path / "t.json"
    p.write_text(
        json.dumps(
            {"schema": 1, "lag_s": 2.0, "frame_s": 0.008, "n_frames": 7500, "regions": regions}
        )
    )
    return str(p)


def hits(start_ms: float, lag_ms: float, seek: float, strength=lambda n: 1.0) -> list[Any]:
    """A 2 s bar of five hits over the 35 s window and one bar past it, as the chip reports
    them (epoch ms: the audio start, plus the time into the window, plus the rig's lag)."""
    bar = [0.0, 0.5, 0.75, 1.0, 1.5]
    out = []
    for n in range(18):
        for h in bar:
            t = n * 2.0 + h
            out.append(
                SimpleNamespace(timestamp_ms=start_ms + lag_ms + t * 1000, strength=strength(n))
            )
    return out


def test_repetition_puts_detections_on_the_tracks_clock(tmp_path: Path) -> None:
    track = {"seek": 10.0, "repetition": _map(tmp_path, [[0, 7250]])}
    got = repetition.repetition_score(hits(5e5, 220.0, 10.0), 5e5, track, 35000.0, 220.0)
    assert got["repS"] == pytest.approx(1.0) and got["rep"] == pytest.approx(1.0)
    assert got["n"] >= 20 and got["coverage"] == pytest.approx(1.0) and got["lagS"] == 2.0
    # The same hits at alternating strengths: every hit repeats, the strengths don't.
    alt = hits(5e5, 220.0, 10.0, strength=lambda n: 0.5 if n % 2 else 1.0)
    got = repetition.repetition_score(alt, 5e5, track, 35000.0, 220.0)
    assert got["rep"] == pytest.approx(1.0) and got["repS"] < 0.7


def test_repetition_without_a_map_a_lag_or_repeated_audio_is_absent_with_the_reason(
    tmp_path: Path,
) -> None:
    det = hits(0.0, 0.0, 0.0)
    assert repetition.repetition_score(det, 0.0, {"seek": 0}, 35000.0, 0.0) == {
        "repS": None,
        "why": "no repetition map for this track",
    }
    track = {"seek": 0.0, "repetition": _map(tmp_path, [[0, 7250]])}
    assert repetition.repetition_score(det, 0.0, track, 35000.0, None)["why"] == (
        "no lag for the run"
    )
    none = {"seek": 0.0, "repetition": _map(tmp_path, [])}
    got = repetition.repetition_score(det, 0.0, none, 35000.0, 0.0)
    assert (
        got["repS"] is None and got["n"] == 0 and got["coverage"] == 0.0 and "too few" in got["why"]
    )


def test_a_reboot_in_health_or_at_the_end_is_recorded() -> None:
    r = report(health={"CHIP": {"stability": {"rebooted": True}}})
    assert staging.score_record(r)["health"]["rebooted"] is True
    r = report(final={"CHIP": {"no_reboot": {"pass": False}}})
    assert staging.score_record(r)["health"]["rebooted"] is True


@pytest.fixture
def data(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setattr(staging, "data_dir", lambda: tmp_path)
    calls: list[dict[str, Any]] = []
    monkeypatch.setattr(staging.strict_rules, "record_run", lambda *a, **k: calls.append(k))
    monkeypatch.setattr(staging.telemetry, "record_staging", lambda r: calls.append({"tel": 1}))
    monkeypatch.setattr(staging, "_calls", calls, raising=False)
    return tmp_path


def lines(d: Path) -> list[dict[str, Any]]:
    return [json.loads(ln) for ln in (d / "staging-scores.jsonl").read_text().splitlines()]


@pytest.mark.asyncio
async def test_a_second_run_of_a_commit_is_a_second_line_a_repeated_step_is_not(
    data: Path,
) -> None:
    fn = getattr(staging.write_report, "__wrapped__", staging.write_report)
    (data / "staging-scores.jsonl").write_text(json.dumps({"sha": "s" * 40, "tracks": []}) + "\n")
    await fn("s" * 40, report(run="job1"))
    assert [x.get("run") for x in lines(data)] == ["job1"]  # the old run-less line replaced
    await fn("s" * 40, report(run="job1"))  # the step again (a restart): rewritten
    await fn("s" * 40, report(run="job2"))  # another run: kept beside it
    assert [x.get("run") for x in lines(data)] == ["job1", "job2"]


@pytest.mark.asyncio
async def test_a_stable_release_run_never_counts_for_readiness_or_the_staging_gauges(
    data: Path,
) -> None:
    fn = getattr(staging.write_report, "__wrapped__", staging.write_report)
    await fn("r" * 40, report(sha="r" * 40, release="b217", build="b217"))
    assert staging._calls == []  # type: ignore[attr-defined]
    assert lines(data)[0]["release"] == "b217"
    await fn("s" * 40, report())
    assert len(staging._calls) == 2  # type: ignore[attr-defined]
