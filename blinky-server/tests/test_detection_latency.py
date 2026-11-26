"""The chip's detection latency vs the rig's calibration (testing/detection_latency.py) and its
strict rule (testing/strict_rules.py ``detection_latency``): per track ``lagCheckMs - lagMs``,
the median over the tracks that count, and the exclusion of the ones the music can't check."""

from __future__ import annotations

from typing import Any

import pytest

from blinky_server import telemetry
from blinky_server.testing import detection_latency, staging, strict_rules

# The box's 28 measured runs of 2026-11-25 (firmware b211-dev-c5f4d51a, AUDIO_PATH_MS 45):
# (track, kickR50, lagMs, lagCheckMs). The calibration evidence the rule's limit and the
# kickR50 cut come from.
CALIBRATION = [
    ("afrobeat-feelgood-groove", 1.0, 206.5, 202.5),
    ("amapiano-vibez", 0.9556, 223.1, 225.4),
    ("breakbeat-background", 1.0, 217.6, 216.6),
    ("breakbeat-drive", 0.9787, 213.6, 211.4),
    ("dnb-energetic-breakbeat", 0.6744, 214.6, 213.4),
    ("dnb-liquid-jungle", 0.9804, 200.3, 199.3),
    ("dubstep-edm-halftime", 0.25, 206.9, 264.1),
    ("edm-trap-electro", 0.3111, 235.5, 220.0),
    ("garage-uk-2step", 0.9483, 211.1, 210.3),
    ("reggaeton-fuego-lento", 1.0, 225.7, 226.4),
    ("techno-deep-ambience", 0.7941, 240.1, 234.0),
    ("techno-dub-groove", 0.5825, 215.5, 219.7),
    ("techno-machine-drum", 0.6719, 212.5, 214.5),
    ("techno-machine-drum", 0.7188, 242.3, 243.3),
    ("techno-machine-drum", 0.7344, 235.0, 235.3),
    ("techno-machine-drum", 0.7344, 232.1, 230.9),
    ("techno-machine-drum", 0.7188, 221.7, 224.6),
    ("techno-machine-drum", 0.7344, 215.7, 214.3),
    ("techno-minimal-01", 0.8447, 206.4, 210.1),
    ("techno-minimal-01", 0.835, 239.7, 240.6),
    ("techno-minimal-01", 0.8447, 240.1, 246.7),
    ("techno-minimal-01", 0.8544, 212.1, 212.5),
    ("techno-minimal-01", 0.8447, 245.9, 247.8),
    ("techno-minimal-01", 0.8447, 216.8, 217.5),
    ("techno-minimal-emotion", 0.6044, 216.9, 219.8),
    ("trance-goa-mantra", 1.0, 231.1, 230.8),
    ("trance-infected-vibes", 0.9381, 201.5, 207.6),
    ("trance-party", 0.9605, 208.8, 207.1),
]


def drums(k: float | None, lag: float | None, check: float | None, src: str = "measured"):
    return {"kickR50": k, "lagMs": lag, "lagCheckMs": check, "lagSource": src}


def results(rows: list[tuple[str, float, float, float]], dev: str = "CHIP") -> list[dict[str, Any]]:
    return [{"track": t, "scores": {dev: {"drums": drums(k, lag, chk)}}} for t, k, lag, chk in rows]


def test_a_run_counts_only_with_a_measured_lag_a_music_check_and_kicks_detected() -> None:
    f = detection_latency.run_offset
    assert f(drums(0.9, 210.0, 212.4)) == {"offset_ms": 2.4, "counted": True, "why": None}
    assert f(None)["why"] == "no drum score"
    assert f(drums(0.9, 210.0, None, src="estimated"))["why"] == "lag not measured"
    assert f(drums(0.9, 210.0, None))["why"] == "the music gave no check"
    low = f(drums(0.31, 235.5, 220.0))
    # Left out, never hidden: the offset is still reported, with why it doesn't count.
    assert low["offset_ms"] == -15.5 and low["counted"] is False and "kickR50 0.31" in low["why"]
    assert f(drums(None, 210.0, 212.0))["counted"] is False
    assert f(drums(0.5, 210.0, 212.0))["counted"] is True  # the cut is inclusive


def test_the_calibration_runs_pass_and_only_the_two_sparse_tracks_are_left_out() -> None:
    got = detection_latency.summarize(results(CALIBRATION))["CHIP"]
    left = [r["track"] for r in got["runs"] if not r["counted"]]
    assert left == ["dubstep-edm-halftime", "edm-trap-electro"]
    assert got["counted"] == 26 and got["median_ms"] == pytest.approx(0.55, abs=0.06)
    counted = [r["offset_ms"] for r in got["runs"] if r["counted"]]
    # Every counted run sits well inside the rule's limit: its spread set the limit.
    assert max(abs(x) for x in counted) < strict_rules.threshold("detection_latency") * 0.7
    row = {r["rule"]: r for r in strict_rules.evaluate([], [], latency=got)}["detection_latency"]
    assert row["status"] == strict_rules.PASS and row["samples"] == 26
    assert row["where"] == "median of 26 of 28 tracks"
    assert [x["track"] for x in row["left_out"]] == left
    # Without the cut, the sparse dubstep run alone would be enough to move a short run.
    assert detection_latency.run_offset(drums(0.25, 206.9, 264.1))["offset_ms"] == 57.2


@pytest.mark.parametrize(
    ("offset", "status"),
    [(0.0, "pass"), (9.9, "pass"), (-10.0, "pass"), (10.4, "warn"), (-12.0, "warn")],
)
def test_the_rule_is_a_band_around_the_calibration(offset: float, status: str) -> None:
    latency = detection_latency.summarize(results([("t", 0.9, 210.0, 210.0 + offset)]))["CHIP"]
    row = {r["rule"]: r for r in strict_rules.evaluate([], [], latency=latency)}
    assert row["detection_latency"]["status"] == status
    assert row["detection_latency"]["worst"] == pytest.approx(offset)


def test_no_counted_track_is_no_data_never_a_pass() -> None:
    sparse = detection_latency.summarize(results([("dubstep", 0.25, 206.9, 264.1)]))["CHIP"]
    assert sparse["median_ms"] is None and sparse["counted"] == 0
    row = {r["rule"]: r for r in strict_rules.evaluate([], [], latency=sparse)}
    assert row["detection_latency"]["status"] == strict_rules.NO_DATA
    assert row["detection_latency"]["where"] == "none of 1 tracks counted"
    assert row["detection_latency"]["left_out"][0]["offset_ms"] == 57.2
    # A run that scored no tracks (the hardware checks) has no row data either.
    assert {r["rule"]: r for r in strict_rules.evaluate([], [])}["detection_latency"][
        "status"
    ] == strict_rules.NO_DATA


def test_the_summary_names_the_worst_device_by_distance_either_way() -> None:
    latency = {
        **detection_latency.summarize(results([("t", 0.9, 210.0, 213.0)], "A")),
        **detection_latency.summarize(results([("t", 0.9, 210.0, 202.0)], "B")),
    }
    out = strict_rules.report({}, {}, None, latency)
    row = {r["rule"]: r for r in out["rules"]}["detection_latency"]
    assert row["device"] == "B" and row["worst"] == -8.0 and row["status"] == "pass"
    assert set(out["devices"]) == {"A", "B"}


def test_the_table_has_the_detection_latency_rule_warning() -> None:
    r = strict_rules.BY_NAME["detection_latency"]
    assert (r.threshold, r.unit, r.worst, r.mode) == (10.0, "ms", "abs", strict_rules.WARN)
    assert detection_latency.MIN_KICK_R50 == 0.5


def test_the_median_is_exported_per_device_and_source() -> None:
    def v(name: str) -> float | None:
        return telemetry.REGISTRY.get_sample_value(name, {"device": "CHIP", "source": "dev"})

    telemetry.record_detection_latency("dev", detection_latency.summarize(results(CALIBRATION[:3])))
    assert v("blinky_detection_latency_offset_ms") == pytest.approx(-1.0)
    assert v("blinky_detection_latency_tracks") == 3
    # A run where nothing counted removes the median, never leaves the last one standing.
    telemetry.record_detection_latency(
        "dev", detection_latency.summarize(results([("dubstep", 0.25, 206.9, 264.1)]))
    )
    assert v("blinky_detection_latency_offset_ms") is None
    assert v("blinky_detection_latency_tracks") == 0


def test_the_score_record_keeps_the_check_and_the_median() -> None:
    res = results(CALIBRATION[:2])
    report = {
        "sha": "s" * 40,
        "validation": {"results": res},
        "detection_latency": detection_latency.summarize(res),
    }
    rec = staging.score_record(report)
    assert rec["tracks"][0]["lagCheckMs"] == 202.5
    assert rec["detection_latency"] == {"CHIP": {"median_ms": -0.9, "counted": 2}}


def test_the_summary_is_keyed_by_the_full_chip_id_like_the_strict_rules_windows() -> None:
    # Scores carry the id's first 12 characters (workflows.validation_run); the strict rules'
    # windows and music the full id. Re-keyed, the device has one row, with data.
    chip = "87DE9B58C90AFE2D"
    latency = detection_latency.summarize(results(CALIBRATION[:3], chip[:12]), [chip])
    assert list(latency) == [chip]
    out = strict_rules.report({chip: []}, {}, None, latency)
    assert list(out["devices"]) == [chip]
    row = {r["rule"]: r for r in out["rules"]}["detection_latency"]
    assert row["device"] == chip and row["status"] == strict_rules.PASS


def test_a_dev_run_takes_every_played_track_of_its_validate_checks() -> None:
    from blinky_server.testing import dev_run

    played = results(CALIBRATION[:2])
    checks = [
        {"kind": "info", "devices": {}},
        {"kind": "validate", "results": [played[0], {"track": "x", "failed": True}]},
        {"kind": "validate", "results": [played[1]]},
    ]
    assert dev_run.validated(checks) == played
