"""The test box deployer (deploy.py): the gate, the busy check, and a pass with rollback."""

from __future__ import annotations

import importlib.util
import sys
import time
from pathlib import Path
from typing import Any

import pytest

_spec = importlib.util.spec_from_file_location("deploy", Path(__file__).with_name("deploy.py"))
assert _spec and _spec.loader
deploy = importlib.util.module_from_spec(_spec)
sys.modules["deploy"] = deploy
_spec.loader.exec_module(deploy)

SHA, HEAD, OLD = "m" * 40, "h" * 40, "o" * 40
NOW = 1_800_000_000.0
BOT = "github-actions[bot]"


def run_(name: str, conclusion: str | None = "success") -> dict[str, Any]:
    status = "completed" if conclusion else "in_progress"
    return {"name": name, "status": status, "conclusion": conclusion}


def verdict(
    sha: str = HEAD, headline: str = "No issues found. Checked for bugs.", author: str = BOT
) -> dict[str, Any]:
    body = f"## Claude review — {sha}\n\n{headline}\n\nsummary"
    return {"user": {"login": author}, "body": body}


GOOD_MASTER = [run_("Blinky Server Tests"), run_("Compile Firmware"), run_("Docs", "skipped")]
GOOD_HEAD = [run_("claude-review"), run_("Blinky Server Tests")]
GOOD_COMMENTS = [verdict(OLD, "Found 2 issue(s) — see the inline comments."), verdict()]


def gh(
    master: list[dict[str, Any]] = GOOD_MASTER,
    head: list[dict[str, Any]] = GOOD_HEAD,
    comments: list[dict[str, Any]] = GOOD_COMMENTS,
    merged: bool = True,
    age_s: float = 600,
) -> Any:
    date = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(NOW - age_s))

    def fetch(path: str) -> Any:
        if path.endswith(f"{SHA}/pulls"):
            pr = {"number": 7, "merged_at": "x" if merged else None, "base": {"ref": "master"}}
            return [{**pr, "merge_commit_sha": SHA, "head": {"sha": HEAD}}]
        if path.endswith(f"commits/{SHA}"):
            return {"commit": {"committer": {"date": date}}}
        if f"{SHA}/check-runs" in path:
            return {"check_runs": master}
        if f"{HEAD}/check-runs" in path:
            return {"check_runs": head}
        if "issues/7/comments" in path:
            return comments
        raise AssertionError(path)

    return fetch


def test_green_ci_and_a_clean_review_deploy() -> None:
    assert deploy.gate(SHA, gh(), NOW)[0] == "deploy"


ISSUES = "Found 2 issue(s) — see the inline comments."
INCOMPLETE = "Incomplete review: no issues found, but not every changed file was read."


@pytest.mark.parametrize(
    "kw,want,why",
    [
        ({"merged": False}, "reject", "not the merge of a PR"),
        ({"master": [*GOOD_MASTER, run_("Code Quality", "failure")]}, "reject", "=failure"),
        ({"master": [*GOOD_MASTER, run_("Compile", None)]}, "wait", "running Compile"),
        ({"master": GOOD_MASTER[1:]}, "wait", "Blinky Server Tests hasn't started"),
        ({"master": GOOD_MASTER[1:], "age_s": 7 * 3600}, "reject", "never ran"),
        ({"head": GOOD_HEAD[1:]}, "wait", "claude-review"),
        ({"head": [run_("claude-review", None)]}, "wait", "running claude-review"),
        # The review check is green either way (continue-on-error): the verdict decides.
        ({"comments": [verdict(headline=ISSUES)]}, "reject", "Found 2 issue(s)"),
        ({"comments": [verdict(headline=INCOMPLETE)]}, "reject", "Incomplete review"),
        ({"comments": [verdict(OLD)]}, "reject", "no review verdict"),  # an older commit's
        ({"comments": [verdict(author="someone")]}, "reject", "no review verdict"),  # forged
    ],
)
def test_gate_refuses_or_waits(kw: dict[str, Any], want: str, why: str) -> None:
    got = deploy.gate(SHA, gh(**kw), NOW)
    assert got[0] == want and why in got[1], got


def test_busy() -> None:
    def server(flashes: list[Any], jobs: list[Any]) -> Any:
        def get(p: str) -> Any:
            if p.startswith("/api/flash-jobs"):
                return {"jobs": flashes}
            return jobs if p == "/api/fleet/jobs" else []

        return get

    assert deploy.busy(server([], [])) is None
    assert deploy.busy(server([], [{"status": "complete"}])) is None
    assert "flash" in deploy.busy(server([{"id": 1}], []))
    running = [{"status": "running", "type": "fleet-deploy"}]
    assert "fleet-deploy" in deploy.busy(server([], running))

    def down(p: str) -> Any:
        raise deploy.urllib.error.URLError("refused")

    assert deploy.busy(down) is None  # a dead server isn't busy; deploying may fix it


def fake_git(*cmd: str, **kw: Any) -> str:
    if cmd[:2] == ("git", "rev-parse"):
        return (SHA if cmd[2] == "origin/master" else OLD) + "\n"
    return ""


@pytest.fixture
def box(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> dict[str, Any]:
    """deploy_once against fake git, GitHub, server and systemd, with real slot symlinks."""
    a, b = tmp_path / "blinky-deploy-a", tmp_path / "blinky-deploy-b"
    a.mkdir()
    link = tmp_path / "blinky-deploy"
    link.symlink_to(a)
    calls: dict[str, Any] = {"prepared": [], "restarted_on": [], "healthy": [True], "busy": []}
    monkeypatch.setattr(deploy, "STATE", tmp_path / "deploy.json")
    monkeypatch.setattr(deploy, "LINK", link)
    monkeypatch.setattr(deploy, "SLOTS", (a, b))

    def restart() -> str | None:
        calls["restarted_on"].append(link.resolve().name)
        return None if calls["healthy"].pop(0) else "not answering"

    def prepare(slot: Path, sha: str) -> None:
        calls["prepared"].append((slot.name, sha))

    monkeypatch.setattr(deploy, "run", fake_git)
    monkeypatch.setattr(deploy, "gh_api", gh())
    monkeypatch.setattr(deploy, "busy", lambda: calls["busy"].pop(0) if calls["busy"] else None)
    monkeypatch.setattr(deploy, "prepare", prepare)
    monkeypatch.setattr(deploy, "restart_healthy", restart)
    monkeypatch.setattr(deploy.time, "time", lambda: NOW)
    calls["link"] = link
    return calls


def test_deploys_into_the_other_slot_then_switches(box: dict[str, Any]) -> None:
    assert deploy.deploy_once() == 0
    assert box["prepared"] == [("blinky-deploy-b", SHA)]
    assert box["restarted_on"] == ["blinky-deploy-b"]
    assert box["link"].resolve().name == "blinky-deploy-b"
    assert deploy.load_state()["deployed"] == SHA


def test_busy_after_preparing_leaves_the_live_slot_alone(box: dict[str, Any]) -> None:
    box["busy"] = ["1 flash job(s) active"]
    assert deploy.deploy_once() == 0
    assert box["prepared"] == [("blinky-deploy-b", SHA)]
    assert box["restarted_on"] == [] and box["link"].resolve().name == "blinky-deploy-a"
    assert deploy.deploy_once() == 0  # idle on the next pass: it goes ahead
    assert box["restarted_on"] == ["blinky-deploy-b"]


def test_unhealthy_switches_back_and_refuses_the_commit(box: dict[str, Any]) -> None:
    box["healthy"] = [False, True]
    assert deploy.deploy_once() == 1
    assert box["restarted_on"] == ["blinky-deploy-b", "blinky-deploy-a"]
    assert box["link"].resolve().name == "blinky-deploy-a"
    assert "not answering" in deploy.load_state()["refused"][SHA]
    assert deploy.deploy_once() == 0 and len(box["prepared"]) == 1  # never retried


def test_a_refused_review_is_kept_until_the_owner_accepts_it(
    box: dict[str, Any], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(deploy, "gh_api", gh(comments=[verdict(headline="Found 1 issue(s)")]))
    assert deploy.deploy_once() == 0
    assert box["prepared"] == [] and "Found 1 issue(s)" in deploy.load_state()["refused"][SHA]
    assert deploy.deploy_once() == 0 and box["prepared"] == []  # stays refused
    monkeypatch.setattr(deploy, "run", lambda *c, **k: SHA + "\n")
    assert deploy.accept(SHA[:12], "trivial finding, fixed in the next PR") == 0
    monkeypatch.setattr(deploy, "run", fake_git)
    assert deploy.deploy_once() == 0
    assert box["link"].resolve().name == "blinky-deploy-b"
    assert deploy.load_state()["accepted"][SHA].startswith("trivial")
