"""camera_analysis on synthetic data: no camera needed (CI)."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent))
import camera_analysis as ca

PERIOD = 14_816_000  # ns


def _grid_var(rows: int = 3, cols: int = 4, pitch: int = 16) -> np.ndarray:
    """Variance of a survey: sensor noise plus a soft blob per LED, each LED at its own
    activity (the generator lights some far more often than others)."""
    rng = np.random.default_rng(0)
    var = rng.uniform(0.5, 2.0, (120, 160))
    gy, gx = np.mgrid[0:120, 0:160]
    for r in range(rows):
        for c in range(cols):
            y, x = 20 + r * pitch, 30 + c * pitch
            peak_sd = 90.0 / (1 + 3 * r)  # top row busiest, bottom row 7x quieter
            var += (peak_sd * np.exp(-((gy - y) ** 2 + (gx - x) ** 2) / (2 * 2.5**2))) ** 2
    return var


def test_regions_are_found_and_numbered_row_by_row() -> None:
    labels, regions = ca.find_regions(_grid_var())
    assert len(regions) == 12  # the quiet bottom row too
    assert all(15 <= r.area <= 81 for r in regions)
    assert len({r.bbox for r in regions}) == 12
    assert [r.id for r in regions] == list(range(1, 13))
    for band in range(3):  # top row first, each row left to right
        row = regions[band * 4 : band * 4 + 4]
        assert max(r.row for r in row) - min(r.row for r in row) < 1
        assert [r.col for r in row] == sorted(r.col for r in row)
    assert regions[0].row < regions[4].row < regions[8].row
    assert labels.dtype == np.uint16 and int(labels.max()) == 12


def test_specks_below_the_minimum_area_are_not_regions() -> None:
    var = _grid_var(rows=1, cols=1)
    var[100, 100] = 4000.0  # a hot pixel
    _, regions = ca.find_regions(var)
    assert len(regions) == 1


def test_a_survey_where_nothing_varies_is_invalid() -> None:
    with pytest.raises(ca.Invalid, match="no region varies"):
        ca.find_regions(np.ones((60, 80)))


def test_components_are_four_connected() -> None:
    m = np.zeros((5, 5), bool)
    m[1, 1] = m[2, 2] = True  # diagonal neighbours: two components
    m[3, 3] = m[3, 4] = True
    assert ca.label_components(m).max() == 3


def _flashes(n: int, at: list[int], decay: float = 0.6, seed: int = 1) -> np.ndarray:
    rng = np.random.default_rng(seed)
    x = 30.0 + rng.normal(0, 0.5, n)
    level = 0.0
    hits = set(at)
    for i in range(n):
        level = 1000.0 if i in hits else level * decay
        x[i] += level
    return x


def test_visual_onsets_find_each_flash_at_its_first_bright_frame() -> None:
    n = 400
    at = [40, 100, 103, 170, 260, 330]  # 103 is 3 frames (44 ms) after 100: inside the gap
    ts = np.arange(n, dtype=np.int64) * PERIOD
    on = ca.visual_onsets(_flashes(n, at), ts)
    assert [o["frame"] for o in on] == [40, 100, 170, 260, 330]
    assert on[0]["t_ns"] == 40 * PERIOD


def test_the_row_offset_moves_onset_times_by_the_line_time() -> None:
    n = 200
    ts = np.arange(n, dtype=np.int64) * PERIOD
    x = _flashes(n, [50, 120])
    top = ca.visual_onsets(x, ts, row=0)
    low = ca.visual_onsets(x, ts, row=400)
    assert [b["t_ns"] - a["t_ns"] for a, b in zip(top, low, strict=True)] == [400 * ca.LINE_NS] * 2


def test_noise_alone_has_no_onsets() -> None:
    rng = np.random.default_rng(3)
    ts = np.arange(500, dtype=np.int64) * PERIOD
    assert ca.visual_onsets(30 + rng.normal(0, 0.5, 500), ts) == []
    assert ca.visual_onsets(np.full(500, 30.0), ts) == []


def test_dropped_frames_make_the_run_invalid() -> None:
    ts = np.arange(100, dtype=np.int64) * PERIOD
    assert ca.frame_timing(ts)["dropped"] == 0
    with pytest.raises(ca.Invalid, match="2 dropped"):
        ca.frame_timing(np.delete(ts, [10, 11]))
    assert ca.frame_timing(np.delete(ts, [10, 11]), max_dropped=2)["dropped"] == 2


def test_region_map_round_trips_through_npz(tmp_path: Path) -> None:
    import json

    labels, regions = ca.find_regions(_grid_var(rows=1, cols=2))
    settings = {"exposure_us": 1000}
    path = tmp_path / "m.npz"
    np.savez_compressed(path, **ca.region_map(labels, regions, settings))
    back = np.load(path)
    assert json.loads(str(back["settings"])) == settings
    assert (back["labels"] == labels).all()
    assert len(json.loads(str(back["regions"]))) == 2


def test_matching_is_one_to_one_and_reports_unlit_and_orphans() -> None:
    det = np.array([1.000, 2.000, 3.000, 4.000])
    on = np.array([1.010, 1.020, 2.050, 5.000])  # 1.020 is a second onset for one detection
    pairs, unlit, orphans = ca.match_events(det, on)
    assert pairs == [(0, 0), (1, 2)]
    assert unlit == [2, 3]
    assert orphans == [1, 3]


def test_the_distribution_is_median_iqr_and_tails() -> None:
    d = ca.distribution(list(range(101)))
    assert d["n"] == 101 and d["median"] == 50 and d["iqr"] == [25, 75]
    assert d["p10"] == 10 and d["p90"] == 90
    assert ca.distribution([]) == {"n": 0}


def test_the_constant_offset_lines_detections_up_with_onsets() -> None:
    rng = np.random.default_rng(5)
    det = np.sort(rng.uniform(0, 30, 40))
    on = det + 0.812 + rng.uniform(-0.005, 0.005, 40)
    assert ca.best_constant_offset(det, on, (0.0, 2.0)) == pytest.approx(0.812, abs=0.012)


def test_the_budget_lists_each_source_of_spread() -> None:
    b = ca.timing_budget(
        frame_ns=14.816e6, exposure_us=1000, chip_fps=100.0, clock={"rtt_ms": 4.0, "drift_ms": 0.6}
    )
    assert b["camera_frame_ms"]["sd"] == pytest.approx(4.28, abs=0.01)
    assert b["chip_frame_ms"]["spread"] == 10.0
    assert b["clock_sync_ms"]["shift_up_to"] == 2.0
    assert b["clock_drift_ms"] == 0.6
    assert "chip_frame_ms" not in ca.timing_budget(
        frame_ns=14.816e6, exposure_us=1000, chip_fps=None, clock=None
    )


def test_regions_that_follow_the_detections_are_responsive() -> None:
    n = 600
    ts = np.arange(n, dtype=np.int64) * PERIOD
    t_s = ts / 1e9
    hits = [50, 120, 200, 260, 330, 400, 470, 540]
    follows = _flashes(n, hits)
    other = _flashes(n, [80, 160, 300, 450], seed=2)  # flashes, but not on the detections
    sig = np.stack([follows, other], axis=1)
    det_s = t_s[hits] - 0.010  # each hit lit 10 ms after its detection
    recall, precision = ca.responsiveness(sig, ts, [0.0, 0.0], det_s, t_s)
    assert recall[0] == 1.0 and precision[0] == 1.0
    assert recall[1] == 0.0


def test_a_stuck_stretch_is_a_freeze() -> None:
    rng = np.random.default_rng(4)
    x = 100 + rng.normal(0, 20, 300)  # an animating region
    x[100:120] = x[100] + rng.normal(0, 0.5, 20)  # stuck for 20 frames: sensor noise only
    ts = np.arange(300, dtype=np.int64) * PERIOD
    fr = ca.freezes(x, ts, noise=0.5)
    assert len(fr) == 1 and 19 * PERIOD / 1e6 - 0.1 <= fr[0]["ms"] <= 20 * PERIOD / 1e6 + 0.1
    assert ca.freezes(100 + rng.normal(0, 20, 300), ts, noise=0.5) == []


def test_sensor_noise_comes_from_the_dark_frames() -> None:
    rng = np.random.default_rng(6)
    x = 30 + rng.normal(0, 1.0, 1000)
    x[::5] += 500  # flashes on a fifth of the frames
    assert ca.sensor_noise(x) == pytest.approx(1.41, abs=0.3)  # diff of two N(0, 1)
