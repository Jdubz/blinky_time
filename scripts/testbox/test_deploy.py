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


def run_(name: str, conclusion: str | None = "success") -> dict[str, Any]:
    status = "completed" if conclusion else "in_progress"
    return {"name": name, "status": status, "conclusion": conclusion}


def gh(
    master: list[dict[str, Any]],
    head: list[dict[str, Any]],
    merged: bool = True,
    age_s: float = 600,
) -> Any:
    date = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(NOW - age_s))

    def fetch(path: str) -> Any:
        if path.endswith(f"{SHA}/pulls"):
            return [
                {
                    "number": 7,
                    "merged_at": "x" if merged else None,
                    "base": {"ref": "master"},
                    "merge_commit_sha": SHA,
                    "head": {"sha": HEAD},
                }
            ]
        if path.endswith(f"commits/{SHA}"):
            return {"commit": {"committer": {"date": date}}}
        if f"{SHA}/check-runs" in path:
            return {"check_runs": master}
        if f"{HEAD}/check-runs" in path:
            return {"check_runs": head}
        raise AssertionError(path)

    return fetch


GOOD_MASTER = [run_("Blinky Server Tests"), run_("Compile Firmware"), run_("Docs", "skipped")]
GOOD_HEAD = [run_("claude-review"), run_("Blinky Server Tests")]


def test_green_master_and_reviewed_pr_deploys() -> None:
    assert deploy.gate(SHA, gh(GOOD_MASTER, GOOD_HEAD), NOW)[0] == "deploy"


@pytest.mark.parametrize(
    "master,head,merged,age,verdict,why",
    [
        (GOOD_MASTER, GOOD_HEAD, False, 600, "reject", "not the merge of a PR"),
        (
            [*GOOD_MASTER, run_("Code Quality", "failure")],
            GOOD_HEAD,
            True,
            600,
            "reject",
            "Code Quality=failure",
        ),
        ([*GOOD_MASTER, run_("Compile", None)], GOOD_HEAD, True, 600, "wait", "running Compile"),
        (GOOD_MASTER[1:], GOOD_HEAD, True, 600, "wait", "Blinky Server Tests hasn't started"),
        (GOOD_MASTER[1:], GOOD_HEAD, True, 7 * 3600, "reject", "never ran"),
        (GOOD_MASTER, GOOD_HEAD[1:], True, 600, "wait", "claude-review"),
        (GOOD_MASTER, [run_("claude-review", "cancelled")], True, 600, "reject", "PR #7"),
    ],
)
def test_gate_refuses_or_waits(
    master: Any, head: Any, merged: bool, age: float, verdict: str, why: str
) -> None:
    got = deploy.gate(SHA, gh(master, head, merged, age), NOW)
    assert got[0] == verdict and why in got[1]


def test_busy() -> None:
    def server(flashes: list[Any], jobs: list[Any]) -> Any:
        return lambda p: (
            {"jobs": flashes}
            if p.startswith("/api/flash-jobs")
            else jobs
            if p == "/api/fleet/jobs"
            else []
        )

    assert deploy.busy(server([], [])) is None
    assert deploy.busy(server([], [{"status": "complete"}])) is None
    assert "flash" in deploy.busy(server([{"id": 1}], []))
    assert "fleet-deploy" in deploy.busy(
        server([], [{"status": "running", "type": "fleet-deploy"}])
    )

    def down(p: str) -> Any:
        raise deploy.urllib.error.URLError("refused")

    assert deploy.busy(down) is None  # a dead server isn't busy; deploying may fix it


@pytest.fixture
def box(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> dict[str, Any]:
    """deploy_once against fake git, GitHub, server and systemd."""
    calls: dict[str, Any] = {"installed": [], "restarts": 0, "healthy": [True]}
    monkeypatch.setattr(deploy, "STATE", tmp_path / "deploy.json")

    def run(*cmd: str, **kw: Any) -> str:
        if cmd[:2] == ("git", "rev-parse"):
            return (SHA if cmd[2] == "origin/master" else OLD) + "\n"
        if cmd[:2] == ("git", "diff"):
            return "blinky-server/blinky_server/app.py\n"
        return ""

    def restart() -> str | None:
        calls["restarts"] += 1
        return None if calls["healthy"].pop(0) else "not answering"

    monkeypatch.setattr(deploy, "run", run)
    monkeypatch.setattr(deploy, "gh_api", gh(GOOD_MASTER, GOOD_HEAD))
    monkeypatch.setattr(deploy, "busy", lambda: None)
    monkeypatch.setattr(deploy, "install", lambda sha, files: calls["installed"].append(sha))
    monkeypatch.setattr(deploy, "restart_healthy", restart)
    monkeypatch.setattr(deploy.time, "time", lambda: NOW)
    return calls


def test_deploys_once(box: dict[str, Any]) -> None:
    assert deploy.deploy_once() == 0
    assert box["installed"] == [SHA] and box["restarts"] == 1
    assert deploy.load_state()["deployed"] == SHA
    assert deploy.deploy_once() == 0 and box["restarts"] == 1  # nothing new: no restart


def test_waits_while_busy(box: dict[str, Any], monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(deploy, "busy", lambda: "1 flash job(s) active")
    assert deploy.deploy_once() == 0
    assert box["installed"] == [] and box["restarts"] == 0


def test_unhealthy_rolls_back_and_refuses_the_commit(box: dict[str, Any]) -> None:
    box["healthy"] = [False, True]
    assert deploy.deploy_once() == 1
    assert box["installed"] == [SHA, OLD]  # rolled back to the running commit
    s = deploy.load_state()
    assert s["deployed"] == OLD and "not answering" in s["refused"][SHA]
    assert deploy.deploy_once() == 0 and box["installed"] == [SHA, OLD]  # never retried


def test_red_ci_is_refused_without_touching_anything(
    box: dict[str, Any], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(deploy, "gh_api", gh([*GOOD_MASTER, run_("X", "failure")], GOOD_HEAD))
    assert deploy.deploy_once() == 0
    assert box["installed"] == [] and SHA in deploy.load_state()["refused"]
