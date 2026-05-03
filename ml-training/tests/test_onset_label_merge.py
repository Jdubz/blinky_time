"""Tests for the human-edit overlay merge helper.

The same helper is duplicated in ``blinky-server/blinky_server/testing/
onset_label_merge.py``. If you change the production logic, mirror the change
there and the parallel test in blinky-server/tests/test_track_discovery.py.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from onset_label_merge import (  # noqa: E402
    HumanEditDriftError,
    apply_human_edits,
    find_human_edits_for_stem,
    load_human_edits,
)


@pytest.fixture
def auto_onsets():
    return [
        {"time": 1.0, "strength": 1.0, "systems": 5},
        {"time": 2.0, "strength": 0.6, "systems": 3},
        {"time": 3.0, "strength": 0.4, "systems": 2},
    ]


def test_no_doc_passes_through(auto_onsets):
    merged, stats = apply_human_edits(auto_onsets, None)
    assert [o["time"] for o in merged] == [1.0, 2.0, 3.0]
    assert all(o["source"] == "auto" for o in merged)
    assert stats == {"total": 3, "edited": 0, "removed": 0, "created": 0}


def test_edit_time_only(auto_onsets):
    doc = {"source_count": 3, "edits": {"1": {"time": 2.05, "edited": True}}, "created": []}
    merged, stats = apply_human_edits(auto_onsets, doc)
    edited = next(o for o in merged if abs(o["time"] - 2.05) < 1e-6)
    assert edited["source"] == "auto_edited"
    assert edited["strength"] == 0.6  # carried from auto
    assert stats["edited"] == 1


def test_edit_strength_only(auto_onsets):
    doc = {"source_count": 3, "edits": {"0": {"strength": 0.42, "edited": True}}, "created": []}
    merged, _ = apply_human_edits(auto_onsets, doc)
    assert merged[0]["strength"] == 0.42
    assert merged[0]["time"] == 1.0
    assert merged[0]["source"] == "auto_edited"


def test_remove_drops_entry(auto_onsets):
    doc = {"source_count": 3, "edits": {"2": {"removed": True}}, "created": []}
    merged, stats = apply_human_edits(auto_onsets, doc)
    assert [o["time"] for o in merged] == [1.0, 2.0]
    assert stats == {"total": 2, "edited": 0, "removed": 1, "created": 0}


def test_create_with_default_strength():
    doc = {"source_count": 0, "edits": {}, "created": [{"time": 0.5}]}
    merged, _ = apply_human_edits([], doc)
    assert len(merged) == 1
    assert merged[0]["strength"] == 1.0
    assert merged[0]["systems"] == 0
    assert merged[0]["source"] == "human"


def test_full_mix_sorts_by_time(auto_onsets):
    doc = {
        "source_count": 3,
        "edits": {
            "0": {"time": 1.05, "edited": True},
            "2": {"removed": True},
        },
        "created": [{"time": 0.4, "strength": 0.9}],
    }
    merged, stats = apply_human_edits(auto_onsets, doc)
    assert [round(o["time"], 3) for o in merged] == [0.4, 1.05, 2.0]
    assert [o["source"] for o in merged] == ["human", "auto_edited", "auto"]
    assert stats == {"total": 3, "edited": 1, "removed": 1, "created": 1}


def test_drift_loud_fail(auto_onsets):
    doc = {"source_count": 999, "edits": {}, "created": []}
    with pytest.raises(HumanEditDriftError):
        apply_human_edits(auto_onsets, doc)


def test_missing_source_count_does_not_drift(auto_onsets):
    """No source_count means we can't check; merge is best-effort but still safe."""
    doc = {"edits": {"0": {"removed": True}}, "created": []}
    merged, stats = apply_human_edits(auto_onsets, doc)
    assert len(merged) == 2
    assert stats["removed"] == 1


def test_load_human_edits_roundtrip(tmp_path):
    p = tmp_path / "foo.onsets_human.json"
    payload = {
        "stem": "foo",
        "source": "onsets_consensus",
        "source_count": 2,
        "edits": {"0": {"removed": True}},
        "created": [],
    }
    p.write_text(json.dumps(payload))
    loaded = load_human_edits(p)
    assert loaded == payload


def test_load_human_edits_missing_returns_none(tmp_path):
    assert load_human_edits(tmp_path / "no.json") is None


def test_load_human_edits_malformed_raises(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text(json.dumps({"edits": "not a dict"}))
    with pytest.raises(ValueError):
        load_human_edits(p)


def test_find_human_edits_first_hit_wins(tmp_path):
    a = tmp_path / "a"
    b = tmp_path / "b"
    a.mkdir()
    b.mkdir()
    (a / "foo.onsets_human.json").write_text(json.dumps(
        {"edits": {"0": {"removed": True}}, "created": [], "marker": "a"}
    ))
    (b / "foo.onsets_human.json").write_text(json.dumps(
        {"edits": {}, "created": [], "marker": "b"}
    ))
    hit = find_human_edits_for_stem("foo", [a, b])
    assert hit is not None
    path, doc = hit
    assert doc["marker"] == "a"
    assert path == a / "foo.onsets_human.json"


def test_find_human_edits_no_hit(tmp_path):
    assert find_human_edits_for_stem("nope", [tmp_path]) is None
