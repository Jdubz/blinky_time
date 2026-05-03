"""Tests for ground-truth discovery + human edit overlay merging.

Mirrors `ml-training/tests/test_onset_label_merge.py` so the validation- and
training-side merge stay in sync. If you change one, change the other.
"""

from __future__ import annotations

import json

import pytest

from blinky_server.testing.onset_label_merge import (
    HumanEditDriftError,
    apply_human_edits,
)
from blinky_server.testing.track_discovery import discover_tracks, load_ground_truth


@pytest.fixture
def auto_onsets():
    return [
        {"time": 1.0, "strength": 1.0, "systems": 5},
        {"time": 2.0, "strength": 0.6, "systems": 3},
        {"time": 3.0, "strength": 0.4, "systems": 2},
    ]


def _write_track(d, stem, onsets, edits_doc=None):
    (d / f"{stem}.mp3").write_bytes(b"\x00")
    (d / f"{stem}.onsets_consensus.json").write_text(json.dumps({"onsets": onsets}))
    if edits_doc is not None:
        (d / f"{stem}.onsets_human.json").write_text(json.dumps(edits_doc))


def test_discover_includes_human_edits_when_present(tmp_path, auto_onsets):
    _write_track(tmp_path, "foo", auto_onsets, edits_doc={
        "stem": "foo", "source_count": 3, "edits": {}, "created": [],
    })
    _write_track(tmp_path, "bar", auto_onsets)

    tracks = {t["name"]: t for t in discover_tracks(tmp_path)}
    assert tracks["foo"]["human_edits"].endswith("foo.onsets_human.json")
    assert "human_edits" not in tracks["bar"]


def test_load_ground_truth_applies_overlay(tmp_path, auto_onsets):
    edits = {
        "stem": "foo",
        "source_count": 3,
        "edits": {"0": {"time": 1.05, "edited": True}, "2": {"removed": True}},
        "created": [{"time": 0.4, "strength": 1.0}],
    }
    _write_track(tmp_path, "foo", auto_onsets, edits_doc=edits)
    tracks = discover_tracks(tmp_path)
    gt = load_ground_truth(tracks[0]["ground_truth"], tracks[0]["human_edits"])

    assert [round(o.time, 3) for o in gt.onsets] == [0.4, 1.05, 2.0]
    assert [o.source for o in gt.onsets] == ["human", "auto_edited", "auto"]
    # hits parallel onsets for legacy scoring code paths
    assert [round(h.time, 3) for h in gt.hits] == [0.4, 1.05, 2.0]
    assert [h.source for h in gt.hits] == ["human", "auto_edited", "auto"]


def test_load_ground_truth_no_overlay_unchanged(tmp_path, auto_onsets):
    _write_track(tmp_path, "foo", auto_onsets)
    tracks = discover_tracks(tmp_path)
    gt = load_ground_truth(tracks[0]["ground_truth"])
    assert [o.time for o in gt.onsets] == [1.0, 2.0, 3.0]
    assert all(o.source == "auto" for o in gt.onsets)


def test_load_ground_truth_drift_loud_fails(tmp_path, auto_onsets):
    bad_edits = {
        "stem": "foo", "source_count": 99,  # auto has 3, doc says 99
        "edits": {}, "created": [],
    }
    _write_track(tmp_path, "foo", auto_onsets, edits_doc=bad_edits)
    tracks = discover_tracks(tmp_path)
    with pytest.raises(HumanEditDriftError):
        load_ground_truth(tracks[0]["ground_truth"], tracks[0]["human_edits"])


def test_apply_human_edits_unit_smoke(auto_onsets):
    """Cheap parity check that the duplicated helper matches expectations."""
    merged, stats = apply_human_edits(auto_onsets, None)
    assert all(o["source"] == "auto" for o in merged)
    assert stats == {"total": 3, "edited": 0, "removed": 0, "created": 0}
