"""deployer.py: the build host's pull deployer, against a scratch git repo standing in for
blinky_time and a fake of the GitHub API."""

from __future__ import annotations

import importlib.util
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("deployer", HERE / "deployer.py")
dep = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dep)

SVC = {"name": "labels", "paths": ["labels-service/", "deploy/build-host/labels/"],
       "required": ["Labels service"], "deploy": ["deploy/build-host/labels/deploy.sh"]}
NOW = 2_000_000_000.0


def g(cwd, *a):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *a], cwd=cwd,
                          check=True, capture_output=True, text=True).stdout.strip()


@pytest.fixture
def repo(tmp_path, monkeypatch):
    up = tmp_path / "up"
    up.mkdir()
    g(up, "init", "-q", "-b", "master")
    (up / "README").write_text("x")
    g(up, "add", "-A")
    g(up, "commit", "-qm", "base")
    clone = tmp_path / "clone"
    g(tmp_path, "clone", "-q", str(up), str(clone))
    monkeypatch.setattr(dep, "REPO", clone)
    monkeypatch.setattr(dep, "log", lambda event, **f: None)
    return up


def commit(up, path, text="x"):
    p = up / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    g(up, "add", "-A")
    g(up, "commit", "-qm", f"change {path}")
    return g(up, "rev-parse", "HEAD")


def fake_fetch(verdicts=None, required_ran=True, review="No issues found."):
    """GitHub, for any sha: merged PR #7 with head 'h7', checks green, review `review`."""
    def fetch(path):
        if path.endswith("/pulls"):
            sha = path.split("/")[-2]
            return [{"merged_at": "x", "base": {"ref": "master"}, "merge_commit_sha": sha, "number": 7,
                     "head": {"sha": "h7"}}]
        if "/check-runs" in path and "/h7/" in path:
            return {"check_runs": [{"id": 1, "name": "claude-review", "status": "completed", "conclusion": "success"}]}
        if "/check-runs" in path:
            runs = [{"id": 1, "name": "Documentation Check", "status": "completed", "conclusion": "success"}]
            if required_ran:
                runs.append({"id": 2, "name": "Labels service", "status": "completed", "conclusion": "success"})
            return {"check_runs": runs}
        if "/issues/7/comments" in path:
            return [{"user": {"login": "github-actions[bot]"}, "body": f"## Claude review — h7\n\n{review}\n"}]
        if "/commits/" in path:
            return {"commit": {"committer": {"date": "2033-05-18T03:00:00Z"}}}
        raise AssertionError(path)
    return fetch


def test_nothing_due_when_the_service_did_not_change(repo):
    first = g(repo, "rev-parse", "HEAD")
    state = {"labels": {"deployed": first, "refused": {}}}
    commit(repo, "docs/x.md")
    ran = []
    dep.deploy_pass([SVC], state, fake_fetch(), runner=lambda c: ran.append(c) or 0, now=NOW)
    assert ran == [] and state["labels"]["deployed"] == first


def test_a_gated_change_is_deployed(repo):
    first = g(repo, "rev-parse", "HEAD")
    state = {"labels": {"deployed": first, "refused": {}}}
    new = commit(repo, "labels-service/server.py")
    ran = []
    dep.deploy_pass([SVC], state, fake_fetch(), runner=lambda c: ran.append((c, dep.git("rev-parse", "HEAD").strip())) or 0, now=NOW)
    assert state["labels"]["deployed"] == new
    assert ran == [([str(dep.REPO / "deploy/build-host/labels/deploy.sh")], new)]  # ran from the new commit


def test_waits_for_the_required_check(repo):
    state = {"labels": {"deployed": g(repo, "rev-parse", "HEAD"), "refused": {}}}
    commit(repo, "deploy/build-host/labels/compose.yaml")
    ran = []
    dep.deploy_pass([SVC], state, fake_fetch(required_ran=False), runner=lambda c: ran.append(c) or 0, now=NOW)
    assert ran == [] and not state["labels"]["refused"]


def test_a_review_with_findings_is_refused_and_can_be_accepted(repo):
    state = {"labels": {"deployed": g(repo, "rev-parse", "HEAD"), "refused": {}}}
    new = commit(repo, "labels-service/state.py")
    ran = []
    dep.deploy_pass([SVC], state, fake_fetch(review="Found 1 issue(s)."), runner=lambda c: ran.append(c) or 0, now=NOW)
    assert ran == [] and "review" in state["labels"]["refused"][new]
    dep.deploy_pass([SVC], state, fake_fetch(review="Found 1 issue(s)."), runner=lambda c: ran.append(c) or 0, now=NOW)
    assert ran == []  # refused stays refused
    dep.deploy_pass([SVC], state, fake_fetch(), runner=lambda c: ran.append(c) or 0, now=NOW, accept=new)
    assert state["labels"]["deployed"] == new


def test_a_failed_deploy_rolls_back(repo):
    first = g(repo, "rev-parse", "HEAD")
    state = {"labels": {"deployed": first, "refused": {}}}
    new = commit(repo, "labels-service/server.py")
    ran = []

    def runner(cmd):
        at = dep.git("rev-parse", "HEAD").strip()
        ran.append(at)
        return 1 if at == new else 0

    dep.deploy_pass([SVC], state, fake_fetch(), runner=runner, now=NOW)
    assert ran == [new, first] and state["labels"]["deployed"] == first and new in state["labels"]["refused"]


def test_dry_run_changes_nothing(repo):
    state = {"labels": {"deployed": g(repo, "rev-parse", "HEAD"), "refused": {}}}
    commit(repo, "labels-service/server.py")
    ran = []
    dep.deploy_pass([SVC], state, fake_fetch(), runner=lambda c: ran.append(c) or 0, now=NOW, dry_run=True)
    assert ran == [] and state["labels"]["refused"] == {}


def test_gate_needs_a_merged_pr():
    def fetch(path):
        if path.endswith("/pulls"):
            return []
        return {"commit": {"committer": {"date": "2033-05-18T03:00:00Z"}}}
    assert dep.gate("abc", ("Labels service",), fetch, NOW)[0] == "wait"
    assert dep.gate("abc", ("Labels service",), fetch, NOW + 10 * 86400)[0] == "reject"


def test_the_state_is_saved_after_each_decision(repo):
    state = {"labels": {"deployed": g(repo, "rev-parse", "HEAD"), "refused": {}}}
    new = commit(repo, "labels-service/server.py")
    saved = []
    dep.deploy_pass([SVC], state, fake_fetch(), runner=lambda c: 0, now=NOW,
                    save=lambda s: saved.append(s["labels"]["deployed"]))
    assert saved == [new]
