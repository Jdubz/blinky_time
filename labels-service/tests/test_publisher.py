"""hosted.AutoPublisher: changed tracks are published by themselves, in batches, as one commit on a
labels/auto-<time> branch. A local bare repository stands in for GitHub (pushes) and
tests/fake_github.py serves the public pull-request lookup."""

from __future__ import annotations

import base64
import io
import json
import os
import subprocess
from pathlib import Path

import pytest
from conftest import DUR, KICKS, SNARES, git, sha, write_json
from fake_github import FakeGitHub

import hosted
import server as srv

REPO = "owner/corpus"
SERVICE = Path(__file__).resolve().parents[1]


class Env:
    def __init__(self, tmp_path, edm, fake_url, **kw):
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
        self.pub = hosted.AutoPublisher(co, self.origin.as_uri(), hosted.PublicGitHub(REPO, fake_url),
                                        tmp_path / "state" / "publish.json", poll_every=0,
                                        **{"quiet_s": 0, **kw}, log=jl)
        self.app = srv.App(srv.Corpus(co.root / "edm"), tmp_path / "state", publisher=self.pub,
                           checkout=co, hosted_mode=True, log=jl)
        self.edm = co.root / "edm"

    def edit(self, stem, *ops, tab="tab-aaaa"):
        v = self.app.store.view(stem)
        return self.app.store.apply(stem, list(ops), tab, srv.Corpus(self.edm).audio_sha256(stem), DUR, DUR) and v

    def branches(self):
        return git(self.origin, "for-each-ref", "--format=%(refname:short)", "refs/heads/labels/").split()

    def events(self, name):
        return [e for e in map(json.loads, self.log.getvalue().splitlines()) if e["event"] == name]

    def push_to_main(self, files: dict[str, str], msg="change on main"):
        git(self.work, "pull", "-q", "origin", "main")
        for rel, text in files.items():
            (self.work / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.work / rel).write_text(text)
        git(self.work, "add", "-A")
        git(self.work, "commit", "-qm", msg)
        git(self.work, "push", "-q", "origin", "HEAD:main")

    def merge_branch(self):
        """What auto-merge does: the branch's files land on main (squash)."""
        (branch,) = self.branches()
        files = git(self.origin, "diff", "--name-only", "main", branch).split()
        self.push_to_main({f: git(self.origin, "show", f"{branch}:{f}") for f in files}, "squash merge")


@pytest.fixture
def gh():
    fake = FakeGitHub(REPO)
    url = fake.start()
    yield fake, url
    fake.stop()


@pytest.fixture
def env(tmp_path, edm, gh):
    return Env(tmp_path, edm, gh[1])


A = "edm/drums_human/a.drums.json"
B = "edm/drums_human/b.drums.json"


def test_nothing_to_publish_until_someone_edits(env):
    env.app.store.view("a")  # opened, not edited
    env.app.tick()
    assert env.branches() == []


def test_a_batch_is_one_commit_of_every_changed_track(env):
    env.edit("a", {"op": "del", "id": "h1"})
    env.edit("b", {"op": "review", "a": 0, "b": 3, "on": True})
    env.app.tick()
    (branch,) = env.branches()
    assert branch.startswith("labels/auto-")
    main = git(env.origin, "rev-parse", "main").strip()
    assert git(env.origin, "rev-parse", f"{branch}^").strip() == main
    assert sorted(git(env.origin, "diff", "--name-only", main, branch).split()) == [A, B]
    msg = git(env.origin, "log", "-1", "--format=%B", branch)
    assert msg.startswith("labels: drum reviewer autosave (2 tracks)") and "- a\n- b" in msg
    doc = json.loads(git(env.origin, "show", f"{branch}:{A}"))
    assert len(doc["onsets"]) == 18 and doc["complete"] is False  # partial progress is published
    assert git(env.app.checkout.root, "status", "--porcelain") == ""  # the clone's tree is untouched
    assert env.events("published")[0]["tracks"] == 2
    assert env.app.status("a")["unpublished"] is True


def test_quiet_and_max_wait(tmp_path, edm, gh):
    e = Env(tmp_path, edm, gh[1], quiet_s=1000, max_wait_s=1000)
    e.edit("a", {"op": "del", "id": "h1"})
    e.app.tick()
    assert e.branches() == []  # someone is still editing
    e.pub.max_wait_s = 0  # ...for too long now
    e.app.tick()
    assert len(e.branches()) == 1


def test_the_open_batch_rolls_forward(env, gh):
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    (branch,) = env.branches()
    first = git(env.origin, "rev-parse", branch).strip()
    gh[0].open_pr(branch)
    env.app.tick()  # nothing new: the PR already has it
    assert git(env.origin, "rev-parse", branch).strip() == first
    env.edit("a", {"op": "del", "id": "h2"})
    env.edit("c", {"op": "del", "id": "h1"})
    env.app.tick()
    assert env.branches() == [branch]  # the same branch, force-pushed
    main = git(env.origin, "rev-parse", "main").strip()
    assert git(env.origin, "rev-parse", f"{branch}^").strip() == main
    assert len(json.loads(git(env.origin, "show", f"{branch}:{A}"))["onsets"]) == 17
    assert "edm/drums_human/c.drums.json" in git(env.origin, "diff", "--name-only", main, branch)
    assert env.events("pr_opened")[0]["pr"] == 1 and gh[0].auth_seen == []  # no token, ever


def test_landing_and_the_next_batch(env):
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    env.merge_branch()
    env.app.tick()
    assert env.branches() == [] and env.events("batch_landed")
    st = env.app.status("a")
    assert st["unpublished"] is False and st["published"]
    env.app.tick()
    assert env.branches() == []  # landed: nothing to do
    env.edit("a", {"op": "del", "id": "h2"})
    env.app.tick()
    assert len(env.branches()) == 1 and len(env.events("published")) == 2


def test_main_moving_rebuilds_the_branch(env):
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    (branch,) = env.branches()
    env.push_to_main({"README.md": "someone else's change\n"})
    env.app.tick()
    main = git(env.origin, "rev-parse", "main").strip()
    assert git(env.origin, "rev-parse", f"{branch}^").strip() == main and env.branches() == [branch]


def test_a_closed_pr_is_dropped(env, gh):
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    (branch,) = env.branches()
    pr = gh[0].open_pr(branch)
    gh[0].prs[pr["number"]]["state"] = "closed"
    env.pub.quiet_s = 10_000  # don't republish in the same tick
    env.app.tick()
    assert env.events("batch_closed") and env.branches() == []
    env.pub.quiet_s = 0
    env.app.tick()  # the labels are still changed: a new batch
    assert len(env.branches()) == 1 and env.branches() != [branch]


def human(edm, stem, kicks):
    body = {"schema": 1, "track": stem, "audio_sha256": sha(edm / f"{stem}.mp3"), "duration": DUR,
            "started_from": {"source": "drums_adtof"}, "reviewed_ranges": [], "complete": False,
            "onsets": [{"time": t, "type": "kick"} for t in kicks] + [{"time": t, "type": "snare"} for t in SNARES],
            "updated": "2026-11-26T00:00:00+00:00"}
    from labelcheck import dumps_doc
    return dumps_doc(body)


def test_an_outside_change_on_main_wins(env):
    env.edit("c", {"op": "del", "id": "h1"})
    env.pub.quiet_s = 10_000
    env.app.tick()  # c is changed but not published yet
    env.push_to_main({"edm/drums_human/c.drums.json": human(env.edm, "c", KICKS[:9])}, "the owner's fix")
    rev = env.app.store.view("c")["rev"]
    env.app.tick()
    assert env.events("adopted_main") and env.app.store.since("c", rev)["reset"]
    assert len(env.app.store.view("c")["onsets"]) == 18 and not env.app.store.edited_since_base("c")


def test_a_track_that_started_from_main_is_not_overwritten_by_it(env):
    env.push_to_main({A: human(env.edm, "a", KICKS)}, "a human file")
    env.app.tick()  # refresh the clone
    assert env.app.store.view("a")["seed"] == "human"  # its state starts from main's file
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    assert not env.events("adopted_main") and len(env.branches()) == 1


def test_reverting_a_published_batch_sticks(env):
    """A revert on main is adopted, not re-published: for a track that started from main's human
    file (the revert restores that file) and for one that started from a seed (it deletes the file)."""
    env.push_to_main({A: human(env.edm, "a", KICKS)}, "a human file")
    env.app.tick()
    env.edit("a", {"op": "del", "id": "h1"})  # started from main's file
    env.edit("b", {"op": "del", "id": "h1"})  # started from its seed
    env.app.tick()
    env.merge_branch()
    env.app.tick()
    assert env.branches() == [] and env.events("batch_landed")
    before = git(env.work, "rev-parse", "HEAD").strip()
    git(env.work, "revert", "--no-edit", before)  # the owner reverts the squash merge
    git(env.work, "push", "-q", "origin", "HEAD:main")
    env.app.tick()
    adopted = {e["path"]: e.get("deleted", False) for e in env.events("adopted_main")}
    assert adopted == {A: False, B: True}
    assert len(env.app.store.view("a")["onsets"]) == 19 and env.app.store.view("b")["seed"] == "drums_adtof"
    env.app.tick()
    assert env.branches() == [], "the reverted labels must not be re-published"


def test_vandalism_is_not_published(env):
    ids = [o["id"] for o in env.app.store.view("a")["onsets"]]
    env.edit("a", *({"op": "del", "id": i} for i in ids[:15]))  # deletes 15 of 19
    env.edit("b", {"op": "del", "id": "h1"})
    env.app.tick()
    (branch,) = env.branches()
    main = git(env.origin, "rev-parse", "main").strip()
    assert git(env.origin, "diff", "--name-only", main, branch).split() == [B]
    (refused,) = env.events("publish_refused")
    assert refused["path"] == A and any("deletes 15 of the 19" in p for p in refused["problems"])
    env.app.tick()
    assert len(env.events("publish_refused")) == 1  # logged once per version


def test_push_failure_is_logged_and_retried(env):
    env.edit("a", {"op": "del", "id": "h1"})
    env.pub.push_url = (env.origin.parent / "nowhere.git").as_uri()
    env.app.tick()
    assert env.events("publish_failed") and env.branches() == []
    env.pub.push_url = env.origin.as_uri()
    env.app.tick()
    assert len(env.branches()) == 1


def test_ssh_pushes_pin_github_host_keys(tmp_path):
    co = hosted.Checkout(tmp_path / "co", "file:///x")
    gh = hosted.PublicGitHub(REPO)
    with pytest.raises(ValueError):
        hosted.AutoPublisher(co, "ssh://git@ssh.github.com:443/o/c.git", gh, tmp_path / "p.json")
    p = hosted.AutoPublisher(co, "ssh://git@ssh.github.com:443/o/c.git", gh, tmp_path / "p.json",
                             key_file=tmp_path / "k", known_hosts=SERVICE / "github_known_hosts")
    cmd = p.env["GIT_SSH_COMMAND"]
    assert "StrictHostKeyChecking=yes" in cmd and f"UserKnownHostsFile={SERVICE / 'github_known_hosts'}" in cmd
    assert "GlobalKnownHostsFile=/dev/null" in cmd and "IdentitiesOnly=yes" in cmd


def test_known_hosts_are_githubs_published_keys():
    # https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/githubs-ssh-key-fingerprints
    published = {"SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU", "SHA256:p2QAMXNIC1TJYWeIOttrVc98/R1BUFWu3/LiyKgUfQM",
                 "SHA256:uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s"}
    out = subprocess.run(["ssh-keygen", "-lf", str(SERVICE / "github_known_hosts")], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    by_host: dict[str, set[str]] = {}
    for line in out:
        _, fp, host = line.split()[:3]
        by_host.setdefault(host, set()).add(fp)
    assert by_host == {"github.com": published, "[ssh.github.com]:443": published}


def test_deploy_key_file():
    assert srv.deploy_key_file("") is None
    with pytest.raises(SystemExit):
        srv.deploy_key_file(base64.b64encode(b"not a key").decode())
    key = b"-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----\n"
    p = srv.deploy_key_file(base64.b64encode(key).decode())
    try:
        assert p.read_bytes() == key and (p.stat().st_mode & 0o777) == 0o600 and str(p).startswith("/tmp/")
    finally:
        os.unlink(p)


def test_state_carries_over_a_restart(env, tmp_path):
    env.edit("a", {"op": "del", "id": "h1"})
    env.app.tick()
    again = hosted.AutoPublisher(env.app.checkout, env.origin.as_uri(), env.pub.gh,
                                 tmp_path / "state" / "publish.json", poll_every=0, quiet_s=0)
    assert again.st["branch"] == env.branches()[0]
    write_json(tmp_path / "unused.json", {})
