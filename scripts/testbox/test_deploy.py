"""The test box deployer (deploy.py): the gate, the draining restart, and a pass with rollback."""

from __future__ import annotations

import importlib.util
import subprocess
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


RUN_IDS = iter(range(1, 10**6))


def run_(name: str, conclusion: str | None = "success") -> dict[str, Any]:
    """A check run; ids increase in creation order, as GitHub's do."""
    status = "completed" if conclusion else "in_progress"
    return {"name": name, "status": status, "conclusion": conclusion, "id": next(RUN_IDS)}


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


def staging_gh(statuses: list[dict[str, Any]], repo: str = deploy.GH_REPO) -> Any:
    """A staging -> master merge: no review comments at all, a hardware status instead."""
    base = gh(comments=[])

    def fetch(path: str) -> Any:
        if path.endswith(f"{SHA}/pulls"):
            (pr,) = base(path)
            return [{**pr, "head": {"sha": HEAD, "ref": "staging", "repo": {"full_name": repo}}}]
        if f"{HEAD}/statuses" in path:
            return statuses
        return base(path)

    return fetch


def hw(state: str, context: str = deploy.HARDWARE_CONTEXT) -> dict[str, Any]:
    return {"context": context, "state": state}


@pytest.mark.parametrize(
    "statuses,repo,want,why",
    [
        ([hw("success")], deploy.GH_REPO, "deploy", "hardware report passed"),
        # newest first: a later failure wins over an earlier pass
        ([hw("failure"), hw("success")], deploy.GH_REPO, "reject", "failure"),
        ([hw("success", "other")], deploy.GH_REPO, "reject", "no hardware report"),
        ([], deploy.GH_REPO, "reject", "no hardware report"),
        # a fork's branch named staging is an ordinary PR: it needs a review
        ([hw("success")], "someone/fork", "reject", "no review verdict"),
    ],
)
def test_a_staging_merge_is_gated_on_its_hardware_report(
    statuses: list[dict[str, Any]], repo: str, want: str, why: str
) -> None:
    got = deploy.gate(SHA, staging_gh(statuses, repo), NOW)
    assert got[0] == want and why in got[1], got


ISSUES = "Found 2 issue(s) — see the inline comments."
INCOMPLETE = "Incomplete review: no issues found, but not every changed file was read."


def test_a_check_is_judged_by_its_latest_run() -> None:
    """A re-run supersedes the run before it. Seen on PR #328: its review was retriggered after
    a failed run, and the stale failure refused the merge though the re-run passed."""
    retried = [run_("claude-review", "failure"), run_("claude-review"), run_("Blinky Server Tests")]
    assert deploy.gate(SHA, gh(head=retried), NOW)[0] == "deploy"
    regressed = [run_("claude-review"), run_("claude-review", "failure")]
    assert deploy.gate(SHA, gh(head=regressed), NOW)[0] == "reject"


@pytest.mark.parametrize(
    "kw,want,why",
    [
        ({"merged": False}, "wait", "not linked to a merged PR"),  # GitHub links it late
        ({"merged": False, "age_s": 7 * 3600}, "reject", "not the merge of a PR"),
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


class FakeStop:
    """``systemctl stop`` as Popen: still draining for ``waits`` status intervals."""

    def __init__(self, waits: int, rc: int = 0, stderr: str = "") -> None:
        self.waits, self.returncode, self.stderr, self.killed = waits, rc, stderr, False

    def __call__(self, cmd: list[str], **kw: Any) -> FakeStop:
        assert cmd == ["sudo", "-n", "systemctl", "stop", deploy.SERVICE]
        return self

    def communicate(self, timeout: float) -> tuple[str, str]:
        if self.waits:
            self.waits -= 1
            raise deploy.subprocess.TimeoutExpired("systemctl", timeout)
        return "", self.stderr

    def kill(self) -> None:
        self.killed = True


def test_stop_waits_for_the_drain_and_logs_what_it_waits_for(
    capsys: pytest.CaptureFixture[str],
) -> None:
    statuses = iter(["draining: waiting for a1 validation_run (40 s)"] * 2 + ["drained"])
    assert deploy.stop_drained(600, FakeStop(waits=3), lambda: next(statuses)) is None
    out = capsys.readouterr().out
    # Each new status once, not every interval.
    assert out.count("waiting for a1 validation_run") == 1 and "drained" in out


def test_stop_failures_are_reported() -> None:
    failed = deploy.stop_drained(600, FakeStop(0, rc=1, stderr="Job failed"), lambda: "")
    assert failed is not None and "Job failed" in failed
    # A stop that never returns is killed and reported once its time is up.
    stuck = FakeStop(waits=10**6)
    assert "still waiting" in (deploy.stop_drained(-1, stuck, lambda: "draining") or "")
    assert stuck.killed


def test_switch_installs_units_stops_then_moves_the_link_then_starts(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The link moves only once the old server has exited: it runs its venv through the
    link, and a draining server must never load the new slot's code (#325 review)."""
    slot, live = tmp_path / "blinky-deploy-b", tmp_path / "blinky-deploy-a"
    for d in (slot, live):
        (d / "scripts" / "testbox").mkdir(parents=True)
        for u in deploy.UNITS:
            (d / "scripts" / "testbox" / f"{u}.in").write_text(f"[Service]\n# {u} @DEPLOY_DIR@\n")
    link = tmp_path / "blinky-deploy"
    link.symlink_to(live)
    monkeypatch.setattr(deploy, "LINK", link)
    monkeypatch.setattr(deploy, "UNIT_DIR", tmp_path / "units")
    monkeypatch.setattr(deploy, "ALLOY_CONFIG", tmp_path / "no-alloy" / "config.alloy")
    (tmp_path / "units").mkdir()
    events: list[str] = []

    def fake_run(*cmd: str, **kw: Any) -> str:
        if cmd[:3] == ("sudo", "-n", "tee"):
            Path(cmd[3]).write_text(kw["stdin"])
            events.append(f"unit {Path(cmd[3]).name}")
        elif cmd[-2:] == ("start", deploy.SERVICE):
            events.append(f"start on {link.resolve().name}")
        else:
            events.append(" ".join(cmd[2:]))
        return ""

    def fake_stop(timeout_s: float) -> None:
        events.append(f"stop on {link.resolve().name}")

    monkeypatch.setattr(deploy, "run", fake_run)
    monkeypatch.setattr(deploy, "stop_drained", fake_stop)
    monkeypatch.setattr(deploy, "http_get", lambda p: [])
    assert deploy.switch_to(slot, 600) is None
    assert events == [
        *(f"unit {u}" for u in deploy.UNITS),
        "systemctl daemon-reload",
        # A timer installed for the first time is enabled too (blinky-staging.timer, #349).
        *(f"systemctl enable --now {u}" for u in deploy.UNITS if u.endswith(".timer")),
        "stop on blinky-deploy-a",
        "start on blinky-deploy-b",
    ]
    rendered = (tmp_path / "units" / "blinky-server.service").read_text()
    assert str(link) in rendered and "@" not in rendered
    events.clear()
    assert deploy.switch_to(slot, 600) is None  # unchanged units: no install, no reload
    assert events == ["stop on blinky-deploy-b", "start on blinky-deploy-b"]


def sudo_read(cmd: tuple[str, ...]) -> str:
    """read_live_alloy_config's sudo sh: "1" + the file, or "0" if there is none."""
    p = Path(cmd[-1])
    return "1" + p.read_text() if p.exists() else "0"


def test_the_live_alloy_config_is_read_through_sudo(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """/etc/alloy is root:alloy 0770, so a direct read by the deploy user fails with EACCES:
    the read goes through sudo like the writes (before, every pass failed and blinky-server's
    /metrics never reached Prometheus)."""
    live = tmp_path / "etc-alloy" / "config.alloy"
    monkeypatch.setattr(deploy, "ALLOY_CONFIG", live)
    calls: list[tuple[str, ...]] = []

    def fake_run(*cmd: str, **kw: Any) -> str:
        calls.append(cmd)
        return sudo_read(cmd)

    monkeypatch.setattr(deploy, "run", fake_run)
    assert deploy.read_live_alloy_config() is None  # none installed
    live.parent.mkdir()
    live.write_text("// live\n")
    assert deploy.read_live_alloy_config() == "// live\n"
    assert len(calls) == 2 and all(c[:3] == ("sudo", "-n", "sh") for c in calls)


def test_the_alloy_config_installs_when_it_changes_and_reloads_alloy(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    slot = tmp_path / "slot"
    src = slot / deploy.ALLOY_CONFIG_SRC
    src.parent.mkdir(parents=True)
    src.write_text("// v1\n")
    etc = tmp_path / "etc-alloy"
    monkeypatch.setattr(deploy, "ALLOY_CONFIG", etc / "config.alloy")
    events: list[str] = []

    def fake_run(*cmd: str, **kw: Any) -> str:
        if cmd[:4] == ("sudo", "-n", "sh", "-c"):
            return sudo_read(cmd)
        if cmd[:3] == ("sudo", "-n", "tee"):
            Path(cmd[3]).write_text(kw["stdin"])
            events.append(f"tee {Path(cmd[3]).name}")
        else:
            events.append(" ".join(cmd[2:]))
        return ""

    monkeypatch.setattr(deploy, "run", fake_run)
    # Alloy not installed: nothing is written, and the deploy carries on.
    assert deploy.install_alloy_config(slot) is False and events == []
    etc.mkdir()
    assert deploy.install_alloy_config(slot) is True
    assert events == ["tee config.alloy", "systemctl reload-or-restart alloy"]
    assert (etc / "config.alloy").read_text() == "// v1\n"
    events.clear()
    assert deploy.install_alloy_config(slot) is False and events == []  # unchanged
    src.write_text("// v2\n")
    assert deploy.install_alloy_config(slot) is True and len(events) == 2


def test_a_failed_alloy_reload_restores_the_old_config_so_the_next_pass_retries(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    slot = tmp_path / "slot"
    src = slot / deploy.ALLOY_CONFIG_SRC
    src.parent.mkdir(parents=True)
    src.write_text("// new\n")
    etc = tmp_path / "etc-alloy"
    etc.mkdir()
    (etc / "config.alloy").write_text("// old\n")
    monkeypatch.setattr(deploy, "ALLOY_CONFIG", etc / "config.alloy")

    def fake_run(*cmd: str, **kw: Any) -> str:
        if cmd[:4] == ("sudo", "-n", "sh", "-c"):
            return sudo_read(cmd)
        if cmd[:3] == ("sudo", "-n", "tee"):
            Path(cmd[3]).write_text(kw["stdin"])
        elif "reload-or-restart" in cmd:
            raise RuntimeError("alloy: reload failed")
        return ""

    monkeypatch.setattr(deploy, "run", fake_run)
    deploy.reconcile_alloy_config(slot)  # logged, not raised
    assert (etc / "config.alloy").read_text() == "// old\n"  # differs again: retried next pass


def test_a_unit_added_by_a_commit_is_installed_on_the_next_pass(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The first deploy of a commit that adds a unit runs the previous deploy.py, which doesn't
    know it. Every pass installs what the live slot ships but systemd doesn't have, and enables
    a new timer; installed units are left alone (#351 review)."""
    slot = tmp_path / "live"
    (slot / "scripts" / "testbox").mkdir(parents=True)
    for u in deploy.UNITS:
        (slot / "scripts" / "testbox" / f"{u}.in").write_text(f"[Unit]\n# {u} new\n")
    units = tmp_path / "units"
    units.mkdir()
    for u in (
        "blinky-server.service",
        "blinky-deploy.service",
        "blinky-deploy.timer",
        "blinky-staging.service",
        "blinky-staging.timer",
    ):
        (units / u).write_text("old\n")  # installed, different: not touched here
    monkeypatch.setattr(deploy, "UNIT_DIR", units)
    events: list[str] = []

    def fake_run(*cmd: str, **kw: Any) -> str:
        if cmd[:3] == ("sudo", "-n", "tee"):
            Path(cmd[3]).write_text(kw["stdin"])
            events.append(f"unit {Path(cmd[3]).name}")
        else:
            events.append(" ".join(cmd[2:]))
        return ""

    monkeypatch.setattr(deploy, "run", fake_run)
    assert deploy.install_missing_units(slot) == [
        "blinky-pr-queue.service",
        "blinky-pr-queue.timer",
    ]
    assert events == [
        "unit blinky-pr-queue.service",
        "unit blinky-pr-queue.timer",
        "systemctl daemon-reload",
        "systemctl enable --now blinky-pr-queue.timer",
    ]
    assert (units / "blinky-server.service").read_text() == "old\n"
    events.clear()
    assert deploy.install_missing_units(slot) == [] and events == []


def test_every_pass_reconciles_units_from_the_live_slot(box: dict[str, Any]) -> None:
    deploy.save_state({"refused": {}, "history": [], "deployed": SHA})
    GIT["calls"].clear()
    assert deploy.deploy_once() == 0
    assert box["reconciled"] == ["blinky-deploy-a"]
    assert box["alloy"] == ["blinky-deploy-a"]  # the Alloy config is reconciled every pass too


def test_a_slot_checks_out_the_server_submodules_only(monkeypatch: pytest.MonkeyPatch) -> None:
    """The corpus at the commit's pin; never the bootloader, never recursive, and not the
    firmware (decoupling step 3: nothing the box runs reads it)."""
    calls: list[tuple[str, ...]] = []

    def fake_run(*cmd: str, **kw: Any) -> str:
        calls.append(cmd)
        if cmd[:3] == ("git", "config", "--blob"):
            return (
                "submodule.bootloader/src.path bootloader/src\n"
                "submodule.firmware.path firmware\nsubmodule.corpus.path corpus\n"
            )
        return ""

    monkeypatch.setattr(deploy, "run", fake_run)
    assert deploy.init_submodules(Path("/slot"), SHA) == ["corpus"]
    submodule = [c for c in calls if c[:2] == ("git", "submodule")]
    assert submodule == [
        ("git", "submodule", "sync", "--quiet", "--", "corpus"),
        ("git", "submodule", "update", "--init", "--quiet", "--", "corpus"),
    ]


def test_a_commit_without_submodules_checks_out_none(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_run(*cmd: str, **kw: Any) -> str:
        if cmd[:3] == ("git", "config", "--blob"):
            raise RuntimeError("no .gitmodules")
        raise AssertionError(cmd)

    monkeypatch.setattr(deploy, "run", fake_run)
    assert deploy.init_submodules(Path("/slot"), SHA) == []


def _git(cwd: Path, *args: str) -> str:
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=True)
    return r.stdout.strip()


def _repo(path: Path, files: dict[str, str]) -> str:
    path.mkdir(parents=True)
    _git(path, "init", "-q")
    for name, text in files.items():
        (path / name).parent.mkdir(parents=True, exist_ok=True)
        (path / name).write_text(text)
    _git(path, "add", "-A")
    _git(path, "commit", "-qm", "c")
    return _git(path, "rev-parse", "HEAD")


def test_a_slot_from_an_older_deployer_keeps_its_firmware_checkout_and_links(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The transition, against real git. Slots that an older deploy.py prepared have firmware/
    checked out with firmware/.arduino and .arduino linked to the dev clone's toolchain. This
    deploy.py prepares them all the same and leaves all three alone; a fresh slot gets
    neither. A later commit that drops the firmware gitlink (step 4) still prepares, and a
    rollback past it too."""
    for k, v in {
        "GIT_CONFIG_GLOBAL": str(tmp_path / "gitconfig"),  # no user config (signing, hooks)
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_COUNT": "2",
        "GIT_CONFIG_KEY_0": "protocol.file.allow",  # local submodule URLs
        "GIT_CONFIG_VALUE_0": "always",
        "GIT_CONFIG_KEY_1": "init.defaultBranch",
        "GIT_CONFIG_VALUE_1": "master",
        "GIT_AUTHOR_NAME": "t",
        "GIT_AUTHOR_EMAIL": "t@t",
        "GIT_COMMITTER_NAME": "t",
        "GIT_COMMITTER_EMAIL": "t@t",
    }.items():
        monkeypatch.setenv(k, v)
    fw = tmp_path / "blinky-firmware"
    _repo(fw, {"tools/x.py": "1\n"})
    _repo(tmp_path / "blinky-corpus", {"edm/x.txt": "1\n"})
    dev = tmp_path / "blinky_time"
    _repo(dev, {"blinky-server/pyproject.toml": "[project]\n", "blinky-console/package.json": "{}"})
    for name, url in (("firmware", fw), ("corpus", tmp_path / "blinky-corpus")):
        _git(dev, "submodule", "add", "-q", str(url), name)
    _git(dev, "commit", "-qm", "submodules")
    old = _git(dev, "rev-parse", "HEAD")
    (dev / ".arduino").mkdir()

    # What the older deploy.py left in a slot: the firmware checkout and both links.
    slot = tmp_path / "blinky-deploy-a"
    _git(dev, "worktree", "add", "-q", "--detach", str(slot), old)
    _git(slot, "submodule", "update", "--init", "-q", "firmware", "corpus")
    links = (slot / "firmware" / ".arduino", slot / ".arduino")
    for link in links:
        link.symlink_to(dev / ".arduino")
    # The commit this deploy.py installs moves the firmware pin.
    (fw / "tools" / "x.py").write_text("2\n")
    _git(fw, "commit", "-qam", "fw 2")
    _git(dev / "firmware", "pull", "-q", "origin", "master")
    _git(dev, "commit", "-qam", "firmware pin")
    new = _git(dev, "rev-parse", "HEAD")

    others: list[tuple[str, ...]] = []
    real_run = deploy.run

    def run(*cmd: str, **kw: Any) -> str:
        if cmd[0] == "git":
            return str(real_run(*cmd, **kw))
        others.append(cmd)  # venv, pip, npm
        return ""

    monkeypatch.setattr(deploy, "run", run)
    monkeypatch.setattr(deploy, "DEV_CLONE", dev)
    deploy.prepare(slot, new)
    assert _git(slot, "rev-parse", "HEAD") == new
    assert (slot / "corpus" / "edm" / "x.txt").is_file()
    assert (slot / "firmware" / "tools" / "x.py").read_text() == "1\n"  # left at the old pin
    for link in links:
        assert link.is_symlink() and link.resolve() == (dev / ".arduino").resolve()

    fresh = tmp_path / "blinky-deploy-b"
    deploy.prepare(fresh, new)
    assert (fresh / "corpus" / "edm" / "x.txt").is_file()
    assert list((fresh / "firmware").iterdir()) == []  # never checked out
    assert not (fresh / ".arduino").exists() and not (fresh / ".arduino").is_symlink()

    # Step 4 drops the gitlink: git keeps the populated directory (a warning), the prepare
    # succeeds, and so does a rollback to the commit before.
    _git(dev, "rm", "-q", "firmware")
    _git(dev, "commit", "-qm", "no firmware submodule")
    for sha in (_git(dev, "rev-parse", "HEAD"), new):
        deploy.prepare(slot, sha)
        assert _git(slot, "rev-parse", "HEAD") == sha
        assert (slot / "corpus" / "edm" / "x.txt").is_file()
    assert not any("arduino" in " ".join(c) for c in others)


def test_the_server_unit_signals_only_the_server_on_stop() -> None:
    """KillMode=mixed: the default control-group SIGTERMs ffplay, the UF2 copy and a release
    download at the same moment as the server, before its drain could wait for them."""
    unit = (Path(deploy.__file__).parent / "blinky-server.service.in").read_text()
    assert "\nKillMode=mixed\n" in unit


GIT: dict[str, Any] = {"diff": ["blinky-server/blinky_server/app.py"], "calls": []}


def fake_git(*cmd: str, **kw: Any) -> str:
    GIT["calls"].append(cmd)
    if cmd[:2] == ("git", "fetch"):
        # Never a submodule (#414 review): an old slot's firmware/ must not be fetched.
        assert "--no-recurse-submodules" in cmd
    if cmd[:2] == ("git", "rev-parse"):
        return (SHA if cmd[2] == "origin/master" else OLD) + "\n"
    if cmd[:2] == ("git", "diff"):
        assert cmd[-2:] == (OLD, SHA)  # against what the server runs, not the last commit
        assert "-z" in cmd
        return "\0".join(GIT["diff"]) + "\0"
    return ""


def ref_updates() -> list[tuple[str, ...]]:
    return [c for c in GIT["calls"] if c[:2] == ("git", "update-ref")]


@pytest.fixture
def box(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> dict[str, Any]:
    """deploy_once against fake git, GitHub, server and systemd, with real slot symlinks."""
    a, b = tmp_path / "blinky-deploy-a", tmp_path / "blinky-deploy-b"
    a.mkdir()
    link = tmp_path / "blinky-deploy"
    link.symlink_to(a)
    calls: dict[str, Any] = {"prepared": [], "restarted_on": [], "healthy": [True]}
    GIT.update(diff=["blinky-server/blinky_server/app.py"], calls=[])
    monkeypatch.setattr(deploy, "STATE", tmp_path / "deploy.json")
    monkeypatch.setattr(deploy, "LINK", link)
    monkeypatch.setattr(deploy, "SLOTS", (a, b))

    def switch(slot: Path, timeout_s: float) -> str | None:
        assert timeout_s > 0
        deploy.point_at(slot)
        calls["restarted_on"].append(link.resolve().name)
        return None if calls["healthy"].pop(0) else "not answering"

    def prepare(slot: Path, sha: str) -> None:
        calls["prepared"].append((slot.name, sha))
        clock[0] += calls.get("prepare_s", 0)  # how long this prepare took

    monkeypatch.setattr(deploy, "run", fake_git)
    monkeypatch.setattr(deploy, "gh_api", gh())
    monkeypatch.setattr(deploy, "prepare", prepare)
    calls["reconciled"] = []
    monkeypatch.setattr(
        deploy, "reconcile_alloy_config", lambda slot: calls["alloy"].append(slot.name)
    )
    calls["alloy"] = []
    monkeypatch.setattr(
        deploy, "install_missing_units", lambda slot: calls["reconciled"].append(slot.name)
    )
    monkeypatch.setattr(deploy, "switch_to", switch)
    monkeypatch.setattr(deploy.time, "time", lambda: NOW)
    clock = [0.0]
    monkeypatch.setattr(deploy.time, "monotonic", lambda: clock[0])
    calls["link"] = link
    return calls


def test_deploys_into_the_other_slot_then_switches(box: dict[str, Any]) -> None:
    assert deploy.deploy_once() == 0
    assert box["prepared"] == [("blinky-deploy-b", SHA)]
    assert box["restarted_on"] == ["blinky-deploy-b"]
    assert box["link"].resolve().name == "blinky-deploy-b"
    assert deploy.load_state()["deployed"] == SHA
    assert ref_updates() == [("git", "update-ref", "refs/blinky/deployable", SHA)]


@pytest.mark.parametrize(
    "files",
    [
        ["ml-training/train.py"],
        ["tools/device_eval.py", "docs/x.md", "ml-training/configs/x.yaml"],
        [".github/workflows/pr-validation.yml", "docs/DEVICES.md", "CLAUDE.md"],
        # A firmware pin bump: the server reads nothing from the firmware checkout.
        ["firmware"],
        ["firmware", "docs/TEST_BOX.md"],
    ],
)
def test_a_commit_outside_the_server_moves_the_ref_and_restarts_nothing(
    box: dict[str, Any], files: list[str]
) -> None:
    GIT["diff"] = files
    assert deploy.deploy_once() == 0
    assert box["prepared"] == [] and box["restarted_on"] == []
    assert box["link"].resolve().name == "blinky-deploy-a"
    assert ref_updates() == [("git", "update-ref", "refs/blinky/deployable", SHA)]
    s = deploy.load_state()
    assert s["deployable"] == SHA and s["deployed"] == OLD
    assert s["history"][-1]["result"] == "no server change"
    GIT["calls"] = []
    assert deploy.deploy_once() == 0 and ref_updates() == []  # seen: not gated again


@pytest.mark.parametrize(
    "path",
    [
        "blinky-server/pyproject.toml",
        "blinky-console/src/App.tsx",
        "blinky-console/src/café del mar.ts",  # non-ASCII and spaces
        "scripts/testbox/deploy.py",
        "corpus",
    ],
)
def test_every_server_path_deploys(box: dict[str, Any], path: str) -> None:
    GIT["diff"] = ["ml-training/train.py", path]
    assert deploy.deploy_once() == 0
    assert box["restarted_on"] == ["blinky-deploy-b"]


def test_a_slow_prepare_leaves_the_restart_to_the_next_pass(box: dict[str, Any]) -> None:
    """A pass must finish inside its unit's TimeoutStartSec, drain included (#329 review):
    after a long prepare (a console rebuild) it stops, and the next pass, whose prepare is a
    no-op, restarts with a fresh budget."""
    box["prepare_s"] = 10 * 60
    assert deploy.deploy_once() == 0
    assert box["restarted_on"] == [] and box["link"].resolve().name == "blinky-deploy-a"
    box["prepare_s"] = 0
    assert deploy.deploy_once() == 0
    assert box["restarted_on"] == ["blinky-deploy-b"]


def test_the_pass_budget_is_the_units_and_fits_a_full_drain() -> None:
    unit = (Path(deploy.__file__).parent / "blinky-deploy.service.in").read_text()
    assert f"TimeoutStartSec={deploy.PASS_BUDGET_S // 60}min" in unit
    # A fresh pass has room for the longest drain plus the health check and a rollback.
    fresh = deploy.PASS_BUDGET_S - deploy.HEALTH_TIMEOUT_S - deploy.ROLLBACK_RESERVE_S
    assert fresh >= deploy.MIN_RESTART_S
    from blinky_server.work.durable import DRAIN_MAX_S

    assert deploy.MIN_RESTART_S > DRAIN_MAX_S


def test_unhealthy_switches_back_and_refuses_the_commit(box: dict[str, Any]) -> None:
    box["healthy"] = [False, True]
    assert deploy.deploy_once() == 1
    assert box["restarted_on"] == ["blinky-deploy-b", "blinky-deploy-a"]
    assert box["link"].resolve().name == "blinky-deploy-a"
    assert "not answering" in deploy.load_state()["refused"][SHA]
    assert deploy.deploy_once() == 0 and len(box["prepared"]) == 1  # never retried


def test_a_refused_head_is_logged_every_pass(
    box: dict[str, Any], capsys: pytest.CaptureFixture[str]
) -> None:
    """The alert BlinkyDeployHeadRefused matches this line while it holds, so it repeats."""
    box["healthy"] = [False, True]
    assert deploy.deploy_once() == 1
    capsys.readouterr()
    for _ in range(2):
        assert deploy.deploy_once() == 0
        out = capsys.readouterr().out
        assert f"{SHA[:12]}: master head refused (deploy failed: " in out
        assert f"deploy.py --accept {SHA[:12]} REASON" in out


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
