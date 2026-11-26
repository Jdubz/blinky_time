"""state.py: one shared set of labels per track, edited by operations."""

from __future__ import annotations

import json

import pytest
from conftest import DUR

import labelcheck
import state

SHA = "a" * 64


def init(onsets=((1.0, "kick"), (1.5, "snare"), (2.0, "kick")), **kw):
    return {"onsets": [{"time": t, "type": ty} for t, ty in onsets], "reviewed_ranges": [],
            "started_from": {"source": "drums_adtof"}, "seed": "drums_adtof", "audio_sha256": SHA,
            "duration": DUR, "updated": None, **kw}


@pytest.fixture
def store(tmp_path):
    changed = []
    s = state.TrackStore(tmp_path / "tracks", lambda stem: init(), on_change=changed.append)
    s.changed = changed
    return s


def apply(store, *ops, tab="tab-aaaa"):
    return store.apply("a", list(ops), tab, SHA, DUR, DUR)


def test_starts_from_the_initial_labels(store):
    v = store.view("a")
    assert v["rev"] == 0 and not v["edited"]
    assert [(o["id"], o["time"], o["type"]) for o in v["onsets"]] == [
        ("h1", 1.0, "kick"), ("h2", 1.5, "snare"), ("h3", 2.0, "kick")]
    assert not store.edited_since_base("a")


def test_operations(store):
    rev = apply(store, {"op": "add", "id": "tab-aaaa-1", "time": 3.00004, "type": "snare"},
                {"op": "move", "id": "h1", "time": 1.0123}, {"op": "type", "id": "h3", "type": "snare"},
                {"op": "del", "id": "h2"}, {"op": "review", "a": 0, "b": 4, "on": True},
                {"op": "review", "a": 1, "b": 2, "on": False})
    assert rev == 6 and store.changed == ["a"]
    v = store.view("a")
    assert [(o["id"], o["time"], o["type"]) for o in v["onsets"]] == [
        ("h1", 1.0123, "kick"), ("h3", 2.0, "snare"), ("tab-aaaa-1", 3.0, "snare")]
    assert v["reviewed_ranges"] == [[0.0, 1.0], [2.0, 4.0]] and v["edited"]


def test_edits_from_two_pages_both_land(store):
    """Two people on one track: different hits, no overwrite; the same hit, the later edit wins."""
    apply(store, {"op": "move", "id": "h1", "time": 1.01}, tab="tab-aaaa")
    apply(store, {"op": "del", "id": "h3"}, {"op": "move", "id": "h1", "time": 1.02}, tab="tab-bbbb")
    v = store.view("a")
    assert {o["id"]: o["time"] for o in v["onsets"]} == {"h1": 1.02, "h2": 1.5}


def test_stale_operations_change_nothing(store):
    apply(store, {"op": "del", "id": "h1"})
    rev = apply(store, {"op": "move", "id": "h1", "time": 1.2}, {"op": "type", "id": "h1", "type": "snare"},
                {"op": "move", "id": "h2", "time": 1.5})  # gone, gone, no-op
    assert rev == 1 and len(store.view("a")["onsets"]) == 2


def test_a_batch_that_changes_nothing_touches_nothing(store):
    apply(store, {"op": "del", "id": "h1"})
    before = (store.text("a"), store.edited_at("a"), list(store.changed))
    rev = apply(store, {"op": "del", "id": "h1"}, {"op": "move", "id": "h2", "time": 1.5})  # both no-ops
    assert rev == 1 and (store.text("a"), store.edited_at("a"), store.changed) == before


def test_pull(store):
    apply(store, {"op": "move", "id": "h1", "time": 1.1}, tab="tab-aaaa")
    apply(store, {"op": "del", "id": "h2"}, tab="tab-bbbb")
    assert store.since("a", 2) == {"rev": 2, "ops": []}
    got = store.since("a", 0)
    assert got["rev"] == 2 and [(r, by, op["op"]) for r, by, op in got["ops"]] == [
        (1, "tab-aaaa", "move"), (2, "tab-bbbb", "del")]
    assert store.since("a", 1)["ops"][0][0] == 2
    assert store.since("a", 5)["reset"]  # from the future: reload


def test_pull_too_far_behind_reloads(store, monkeypatch):
    monkeypatch.setattr(state, "KEEP_OPS", 3)
    for i in range(6):
        apply(store, {"op": "move", "id": "h1", "time": 1 + i / 100})
    assert store.since("a", 0)["reset"] and not store.since("a", 4).get("reset")


@pytest.mark.parametrize("op,msg", [
    ({"op": "add", "id": "x1", "time": 1.0, "type": "hihat"}, "kick or snare"),
    ({"op": "add", "id": "x1", "time": DUR + 1, "type": "kick"}, "out of range"),
    ({"op": "add", "id": "x1", "time": float("nan"), "type": "kick"}, "out of range"),
    ({"op": "move", "id": "../x", "time": 1.0}, "hit id"),
    ({"op": "review", "a": 3, "b": 2, "on": True}, "review range"),
    ({"op": "review", "a": 1, "b": 2, "on": "yes"}, "review range"),
    ({"op": "drop_table"}, "bad operation"),
])
def test_bad_operations_are_refused_whole(store, op, msg):
    with pytest.raises(state.OpError, match=msg):
        apply(store, {"op": "move", "id": "h1", "time": 1.2}, op)
    assert store.view("a")["rev"] == 0  # all or nothing


def test_audio_and_tab_checks(store):
    with pytest.raises(state.OpError, match="audio changed"):
        store.apply("a", [{"op": "del", "id": "h1"}], "tab-aaaa", "b" * 64, DUR, DUR)
    with pytest.raises(state.OpError, match="tab"):
        store.apply("a", [{"op": "del", "id": "h1"}], "x", SHA, DUR, DUR)
    with pytest.raises(state.OpError):
        store.apply("a", [], "tab-aaaa", SHA, DUR, DUR)


def test_state_survives_a_restart(store, tmp_path):
    apply(store, {"op": "del", "id": "h1"}, {"op": "review", "a": 0, "b": DUR, "on": True})
    again = state.TrackStore(tmp_path / "tracks", lambda stem: pytest.fail("not re-seeded"))
    v = again.view("a")
    assert v["rev"] == 2 and len(v["onsets"]) == 2 and v["reviewed_ranges"] == [[0.0, DUR]]


def test_the_label_file(store, edm):
    apply(store, {"op": "add", "id": "n1", "time": 1.0, "type": "kick"},  # an exact duplicate of h1
          {"op": "review", "a": 0.0004, "b": 5.00049, "on": True},
          {"op": "review", "a": 5.0104, "b": DUR, "on": True})  # rounding closes the gap
    doc = json.loads(store.text("a"))
    assert doc["onsets"] == [{"time": 1.0, "type": "kick"}, {"time": 1.5, "type": "snare"},
                             {"time": 2.0, "type": "kick"}]
    assert doc["complete"] is True and doc["reviewed_ranges"] == [[0.0, DUR]]
    sha_a = labelcheck.file_sha256(edm / "a.mp3")
    assert labelcheck.check_schema({**doc, "audio_sha256": sha_a}, "a", sha_a, DUR) == []


def test_reset_and_import(store):
    apply(store, {"op": "del", "id": "h1"})
    rev = store.reset("a", init(onsets=((4.0, "kick"),), base_blob="abc"))
    assert rev == 2 and store.since("a", 1)["reset"] and store.base_blob("a") == "abc"
    v = store.view("a")
    assert [o["time"] for o in v["onsets"]] == [4.0] and not v["edited"]  # main's version: nothing to publish
    # a page still on the old generation: its operations find no hit and change nothing
    assert apply(store, {"op": "del", "id": "h1"}, {"op": "move", "id": "h2", "time": 1.6}) == rev
    assert [o["time"] for o in store.view("a")["onsets"]] == [4.0] and v["onsets"][0]["id"] == "r2h1"
    store.import_doc("b", init(onsets=((5.0, "snare"),)))
    assert store.edited_since_base("b") and store.view("b")["rev"] == 1  # an import is an edit: it gets published


# --- reviewer notes ----------------------------------------------------------------------------


def test_note_operations(store):
    rev = apply(store, {"op": "note_add", "id": "tab-aaaa-n1", "text": "snares under the sweep all dropped", "range": [1.0, 4.5]},
                {"op": "note_add", "id": "tab-aaaa-n2", "text": "rim shots read as hats", "range": None})
    assert rev == 2 and store.edited_since_base("a")
    apply(store, {"op": "note_edit", "id": "tab-aaaa-n1", "text": "snares under the sweep dropped"},
          {"op": "note_edit", "id": "tab-aaaa-n2", "range": [0.5, 1.0]})
    assert store.view("a")["notes"] == [
        {"id": "tab-aaaa-n1", "text": "snares under the sweep dropped", "range": [1.0, 4.5]},
        {"id": "tab-aaaa-n2", "text": "rim shots read as hats", "range": [0.5, 1.0]}]
    apply(store, {"op": "note_edit", "id": "tab-aaaa-n2", "range": None}, {"op": "note_del", "id": "tab-aaaa-n1"})
    assert store.view("a")["notes"] == [{"id": "tab-aaaa-n2", "text": "rim shots read as hats", "range": None}]
    rev = store.view("a")["rev"]
    assert apply(store, {"op": "note_del", "id": "tab-aaaa-n1"}) == rev  # gone already: nothing changes
    doc = store.doc("a")
    assert doc["notes"] == [{"text": "rim shots read as hats", "range": None}]
    assert labelcheck.check_notes(doc["notes"], DUR) == []
    assert '"notes": [\n    {"text": "rim shots read as hats", "range": null}\n  ],' in store.text("a")


def test_note_text_is_cleaned(store):
    apply(store, {"op": "note_add", "id": "n-x", "text": "  two\nlines‮ and\x00 a <b>tag</b>  ", "range": None})
    assert store.view("a")["notes"][0]["text"] == "two lines and a <b>tag</b>"  # plain text, kept as text


@pytest.mark.parametrize("op", [
    {"op": "note_add", "id": "n-x", "text": "", "range": None},
    {"op": "note_add", "id": "n-x", "text": " ​\x07 ", "range": None},  # nothing left after cleaning
    {"op": "note_add", "id": "n-x", "text": "x" * (labelcheck.MAX_NOTE_CHARS + 1), "range": None},
    {"op": "note_add", "id": "n-x", "text": 5, "range": None},
    {"op": "note_add", "id": "n-x", "text": "x"},  # no range key
    {"op": "note_add", "id": "n-x", "text": "x", "range": [2.0, 1.0]},
    {"op": "note_add", "id": "n-x", "text": "x", "range": [0.0, DUR + 5]},
    {"op": "note_add", "id": "n-x", "text": "x", "range": "0-1"},
    {"op": "note_add", "id": "n x", "text": "x", "range": None},
    {"op": "note_edit", "id": "n-x"},
])
def test_bad_note_operations_are_refused(store, op):
    with pytest.raises(state.OpError):
        apply(store, op)
    assert store.view("a")["rev"] == 0


def test_note_cap(store):
    apply(store, *[{"op": "note_add", "id": f"n-{i}", "text": f"note {i}", "range": None} for i in range(labelcheck.MAX_NOTES)])
    with pytest.raises(state.OpError, match="at most"):
        apply(store, {"op": "note_add", "id": "n-extra", "text": "one too many", "range": None})
    apply(store, {"op": "note_add", "id": "n-0", "text": "re-adding an existing id edits it", "range": None})
    assert len(store.view("a")["notes"]) == labelcheck.MAX_NOTES


def test_notes_from_the_human_file_and_a_reset(tmp_path):
    notes = [{"text": "from main", "range": [1.0, 2.0]}]
    s = state.TrackStore(tmp_path / "t", lambda stem: init(notes=notes))
    assert s.view("a")["notes"] == [{"id": "n1", "text": "from main", "range": [1.0, 2.0]}]
    assert s.doc("a")["notes"] == notes
    s.reset("a", init(notes=[{"text": "reverted", "range": None}]))
    assert s.view("a")["notes"] == [{"id": "r1n1", "text": "reverted", "range": None}]


def test_a_state_from_before_notes_loads(tmp_path):
    s = state.TrackStore(tmp_path / "t", lambda stem: init())
    s.view("a")
    s._save(s._load("a"))
    raw = json.loads((tmp_path / "t" / "a.json").read_text())
    raw.pop("notes")
    (tmp_path / "t" / "a.json").write_text(json.dumps(raw))
    s2 = state.TrackStore(tmp_path / "t", lambda stem: init())
    assert s2.view("a")["notes"] == [] and "notes" not in s2.doc("a")
