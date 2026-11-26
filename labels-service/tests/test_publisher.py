"""hosted.Publication: the service offers the edited tracks (GET /api/export) to blinky-corpus's
scheduled workflow, which publishes them; the service notices them landing on main and adopts
outside changes. A local bare repository stands in for GitHub's blinky-corpus."""

from __future__ import annotations

import io
import json
import threading
import urllib.request

import pytest
from conftest import DUR, KICKS, SNARES, git, sha

import hosted
import server as srv
from labelcheck import dumps_doc

A = "edm/drums_human/a.drums.json"
B = "edm/drums_human/b.drums.json"


class Env:
    def __init__(self, tmp_path, edm, **kw):
        self.log = io.StringIO()
        jl = hosted.JsonLog(self.log)
        src = edm.parent
        git(src, "init", "-q", "-b", "main")
        git(src, "add", "-A")
        git(src, "commit", "-qm", "corpus")
        self.origin = tmp_path / "origin.git"
        git(tmp_path, "clone", "-q", "--bare", str(src), str(self.origin))
        self.work = tmp_path / "work"
        git(tmp_path, "clone", "-q", str(self.origin), str(self.work))
        co = hosted.Checkout(tmp_path / "co", self.origin.as_uri(), log=jl)
        co.ensure()
        self.pub = hosted.Publication(co, tmp_path / "state" / "publication.json",
                                      **{"quiet_s": 0, **kw}, log=jl)
        self.app = srv.App(srv.Corpus(co.root / "edm"), tmp_path / "state", publisher=self.pub,
                           checkout=co, hosted_mode=True, log=jl)
        self.edm = co.root / "edm"

    def edit(self, stem, *ops):
        return self.app.store.apply(stem, list(ops), "tab-aaaa", srv.Corpus(self.edm).audio_sha256(stem), DUR, DUR)

    def events(self, name):
        return [e for e in map(json.loads, self.log.getvalue().splitlines()) if e["event"] == name]

    def push_to_main(self, files, msg="change on main"):
        git(self.work, "pull", "-q", "origin", "main")
        for rel, text in files.items():
            (self.work / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.work / rel).write_text(text)
        git(self.work, "add", "-A")
        git(self.work, "commit", "-qm", msg)
        git(self.work, "push", "-q", "origin", "HEAD:main")

    def publish(self):
        """What blinky-corpus's labels-sync workflow does: the export, merged into main."""
        out = self.app.export()
        assert out["ready"] and out["tracks"]
        self.push_to_main(out["tracks"], "labels: drum reviewer autosave")
        return out


@pytest.fixture
def env(tmp_path, edm):
    return Env(tmp_path, edm)


def human(edm, stem, kicks):
    return dumps_doc({"schema": 1, "track": stem, "audio_sha256": sha(edm / f"{stem}.mp3"), "duration": DUR,
                      "started_from": {"source": "drums_adtof"}, "reviewed_ranges": [], "complete": False,
                      "onsets": [{"time": t, "type": "kick"} for t in kicks] + [{"time": t, "type": "snare"} for t in SNARES],
                      "updated": "2026-11-26T00:00:00+00:00"})


def test_nothing_to_export_until_someone_edits(env):
    env.app.store.view("a")
    assert env.app.export() == {"ready": False, "tracks": {}, "held": {}}


def test_the_export_is_every_edited_track(env):
    env.edit("a", {"op": "del", "id": "h1"})
    env.edit("b", {"op": "review", "a": 0, "b": 3, "on": True})
    out = env.app.export()
    assert out["ready"] and sorted(out["tracks"]) == [A, B] and out["held"] == {}
    doc = json.loads(out["tracks"][A])
    assert len(doc["onsets"]) == 18 and doc["complete"] is False  # partial progress is offered
    assert env.app.status("a")["unpublished"] is True


def test_ready_once_quiet_or_after_max_wait(tmp_path, edm):
    e = Env(tmp_path, edm, quiet_s=1000, max_wait_s=1000)
    e.edit("a", {"op": "del", "id": "h1"})
    out = e.app.export()
    assert not out["ready"] and out["tracks"]  # someone is still editing
    e.pub.max_wait_s = 0
    assert e.app.export()["ready"]


def test_published_once_it_lands(env):
    env.edit("a", {"op": "del", "id": "h1"})
    env.publish()
    env.app.tick()
    st = env.app.status("a")
    assert st["unpublished"] is False and st["published"] and env.events("published")
    assert env.app.export()["tracks"] == {}  # nothing left to offer
    env.edit("a", {"op": "del", "id": "h2"})
    assert list(env.app.export()["tracks"]) == [A]


def test_edits_after_a_publish_survive(env):
    """Main still holding what we last published is ours: new edits on top of it stay and follow."""
    env.edit("a", {"op": "del", "id": "h1"})
    env.publish()
    env.app.tick()
    env.edit("a", {"op": "del", "id": "h2"})
    env.app.tick()
    env.app.tick()
    assert not env.events("adopted_main") and len(env.app.store.view("a")["onsets"]) == 17
    assert list(env.app.export()["tracks"]) == [A]
    env.publish()
    env.app.tick()
    env.edit("a", {"op": "del", "id": "h3"})  # and again after a second publish
    env.app.tick()
    assert not env.events("adopted_main") and len(env.app.store.view("a")["onsets"]) == 16


def test_an_older_version_landing_is_not_an_outside_change(env):
    env.edit("a", {"op": "del", "id": "h1"})
    out = env.app.export()
    env.edit("a", {"op": "del", "id": "h2"})  # more edits while the batch is merging
    env.push_to_main(out["tracks"])  # the older version lands
    env.app.tick()
    assert not env.events("adopted_main") and len(env.app.store.view("a")["onsets"]) == 17


def test_an_outside_change_on_main_wins(env):
    env.edit("c", {"op": "del", "id": "h1"})
    env.push_to_main({"edm/drums_human/c.drums.json": human(env.edm, "c", KICKS[:9])}, "the owner's fix")
    rev = env.app.store.view("c")["rev"]
    env.app.tick()
    assert env.events("adopted_main") and env.app.store.since("c", rev)["reset"]
    assert len(env.app.store.view("c")["onsets"]) == 18 and not env.app.store.edited_since_base("c")


def test_edits_after_an_adopt_survive(env):
    """After a track takes main's version, editing it again must not make it adopt again."""
    env.edit("c", {"op": "del", "id": "h1"})
    env.push_to_main({"edm/drums_human/c.drums.json": human(env.edm, "c", KICKS[:9])}, "the owner's fix")
    env.app.tick()
    env.edit("c", {"op": "del", "id": "h2"})
    env.app.tick()
    env.app.tick()
    assert len(env.events("adopted_main")) == 1 and len(env.app.store.view("c")["onsets"]) == 17
    assert list(env.app.export()["tracks"]) == ["edm/drums_human/c.drums.json"]
    # and a later outside change still wins
    env.push_to_main({"edm/drums_human/c.drums.json": human(env.edm, "c", KICKS[:8])}, "another fix")
    env.app.tick()
    assert len(env.events("adopted_main")) == 2 and len(env.app.store.view("c")["onsets"]) == 17


def test_a_track_that_started_from_main_is_not_overwritten_by_it(env):
    env.push_to_main({A: human(env.edm, "a", KICKS)}, "a human file")
    env.app.tick()
    assert env.app.store.view("a")["seed"] == "human"
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    assert not env.events("adopted_main") and list(env.app.export()["tracks"]) == [A]


def test_reverting_a_published_batch_sticks(env):
    """A revert on main is adopted, not offered again: for a track that started from main's human
    file (the revert restores it) and for one that started from a seed (it deletes the file)."""
    env.push_to_main({A: human(env.edm, "a", KICKS)}, "a human file")
    env.app.tick()
    env.edit("a", {"op": "del", "id": "h1"})
    env.edit("b", {"op": "del", "id": "h1"})
    env.publish()
    env.app.tick()
    head = git(env.work, "rev-parse", "HEAD").strip()
    git(env.work, "revert", "--no-edit", head)
    git(env.work, "push", "-q", "origin", "HEAD:main")
    env.app.tick()
    adopted = {e["path"]: e.get("deleted", False) for e in env.events("adopted_main")}
    assert adopted == {A: False, B: True}
    assert len(env.app.store.view("a")["onsets"]) == 19 and env.app.store.view("b")["seed"] == "drums_adtof"
    assert env.app.export()["tracks"] == {}, "the reverted labels must not be offered again"
    env.edit("a", {"op": "del", "id": "h2"})  # editing after the revert works
    env.app.tick()
    assert len(env.events("adopted_main")) == 2 and len(env.app.store.view("a")["onsets"]) == 18


def test_reverting_a_later_batch_sticks(env):
    """A revert of the second batch goes back to the first batch's version, which we exported too:
    that's a revert (adopted), not our older version landing late."""
    env.edit("a", {"op": "del", "id": "h1"})
    env.publish()
    env.app.tick()
    env.edit("a", {"op": "del", "id": "h2"})
    env.publish()
    env.app.tick()
    head = git(env.work, "rev-parse", "HEAD").strip()
    git(env.work, "revert", "--no-edit", head)
    git(env.work, "push", "-q", "origin", "HEAD:main")
    env.app.tick()
    assert len(env.events("adopted_main")) == 1 and len(env.app.store.view("a")["onsets"]) == 18
    assert env.app.export()["tracks"] == {}, "the reverted batch must not be offered again"


def test_an_opened_track_follows_main_until_someone_edits_it(env):
    env.app.store.view("a")  # opened: its state starts from the seed
    env.push_to_main({A: human(env.edm, "a", KICKS[:9])}, "a human file lands from elsewhere")
    env.app.tick()
    v = env.app.store.view("a")
    assert v["seed"] == "human" and len(v["onsets"]) == 18 and env.events("refreshed_from_main")
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    assert not env.events("adopted_main") and len(env.app.store.view("a")["onsets"]) == 17


def test_vandalism_is_held_back(env):
    ids = [o["id"] for o in env.app.store.view("a")["onsets"]]
    env.edit("a", *({"op": "del", "id": i} for i in ids[:15]))  # deletes 15 of 19
    env.edit("b", {"op": "del", "id": "h1"})
    out = env.app.export()
    assert list(out["tracks"]) == [B] and any("deletes 15 of the 19" in p for p in out["held"][A])
    env.app.export()
    assert len(env.events("publish_refused")) == 1  # logged once per version


def test_export_over_http(env):
    env.edit("a", {"op": "del", "id": "h1"})
    s = srv.make_server(env.app, "127.0.0.1", 0)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{s.server_address[1]}/api/export", timeout=10) as r:
            out = json.loads(r.read())
        assert out["ready"] and list(out["tracks"]) == [A]
    finally:
        s.shutdown()
        s.server_close()


def test_state_survives_a_restart(env, tmp_path):
    env.edit("a", {"op": "del", "id": "h1"})
    env.publish()
    env.app.tick()
    again = hosted.Publication(env.app.checkout, tmp_path / "state" / "publication.json")
    assert A in again.st["published"]


def test_no_credential_anywhere():
    src = (srv.STATIC_DIR / "server.py").read_text() + (srv.STATIC_DIR / "hosted.py").read_text()
    for word in ("DEPLOY_KEY", "GITHUB_TOKEN", "ssh://", "Authorization"):
        assert word not in src, word
