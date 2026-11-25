"""The box's dev loop client (dev.py, docs/DEV_LOOP.md): the git side (dirty trees, --wip,
bundles and their bases), the build service's contract, and one `try` from end to end."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import threading
from collections.abc import Iterator
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import pytest

_spec = importlib.util.spec_from_file_location("dev", Path(__file__).with_name("dev.py"))
assert _spec and _spec.loader
dev = importlib.util.module_from_spec(_spec)
sys.modules["dev"] = dev
_spec.loader.exec_module(dev)


def git(cwd: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, check=True
    ).stdout.strip()


@pytest.fixture
def repo(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[Any]:
    """A clone of a bare 'origin' with master and staging (staging one commit ahead), and a
    local branch one commit past staging that was never pushed."""
    for k, v in {
        "GIT_AUTHOR_NAME": "t",
        "GIT_AUTHOR_EMAIL": "t@t",
        "GIT_COMMITTER_NAME": "t",
        "GIT_COMMITTER_EMAIL": "t@t",
    }.items():
        monkeypatch.setenv(k, v)
    origin, work = tmp_path / "origin.git", tmp_path / "work"
    git(tmp_path, "init", "-q", "--bare", "-b", "master", str(origin))
    git(tmp_path, "clone", "-q", str(origin), str(work))
    (work / ".gitignore").write_text("build/\n")
    (work / "a.cpp").write_text("int a;\n")
    git(work, "add", ".")
    git(work, "commit", "-qm", "master")
    git(work, "push", "-q", "origin", "HEAD:master")
    (work / "b.cpp").write_text("int b;\n")
    git(work, "add", ".")
    git(work, "commit", "-qm", "staging")
    git(work, "push", "-q", "origin", "HEAD:staging")
    git(work, "checkout", "-qb", "feature")
    (work / "c.cpp").write_text("int c;\n")
    git(work, "add", ".")
    git(work, "commit", "-qm", "feature")
    yield dev.Repo(work)


class FakeService:
    """The build service: records submissions; 409 for bundles on the bases in ``lacks``."""

    def __init__(self, lacks: tuple[str, ...] = ()) -> None:
        self.lacks = lacks
        self.bundles: list[tuple[str, bytes]] = []
        self.shas: list[str] = []
        self.states = ["queued", "building", "done"]

    def submit_sha(self, sha: str) -> dict[str, Any]:
        self.shas.append(sha)
        return {"id": "b-sha", "state": "queued", "sha": sha}

    def submit_bundle(self, path: Path, sha: str) -> dict[str, Any]:
        heads = subprocess.run(
            ["git", "bundle", "list-heads", str(path)], capture_output=True, text=True, check=True
        ).stdout
        assert heads.startswith(sha)
        prereq = path.read_bytes().split(b"\n")[1].decode()  # "-<base sha> ..."
        if any(prereq.startswith(f"-{b}") for b in self.lacks):
            raise dev.BaseMissing("the build service lacks the bundle's base: prerequisites")
        self.bundles.append((prereq[1:41], path.read_bytes()))
        return {"id": "b1", "state": "queued", "sha": sha}

    def get(self, build_id: str) -> dict[str, Any]:
        state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
        return {
            "id": build_id,
            "state": state,
            "sha": "x",
            "version": "b220-dev-abcdef01",
            "error": "compile error" if state == "failed" else None,
            "log_tail": "foo.cpp:1: error" if state == "failed" else "",
        }


@pytest.fixture(autouse=True)
def no_sleep(monkeypatch: pytest.MonkeyPatch) -> list[float]:
    slept: list[float] = []
    monkeypatch.setattr(dev.time, "sleep", slept.append)
    return slept


# -- config ------------------------------------------------------------------------------------


def test_the_build_service_has_no_default_address(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("BLINKY_BUILD_URL", raising=False)
    monkeypatch.delenv("BLINKY_BUILD_TOKEN", raising=False)
    with pytest.raises(dev.DevError, match="no build service configured"):
        dev.build_config(tmp_path / "none.env")
    env = tmp_path / "build.env"
    env.write_text(
        '# private\nexport BLINKY_BUILD_URL="http://build.example.invalid/"\nBLINKY_BUILD_TOKEN=tok\n'
    )
    assert dev.build_config(env) == ("http://build.example.invalid", "tok")


def test_checks_parse() -> None:
    assert dev.parse_checks(
        ["info", "health=2", "validate=a,b", "pace", "cmd=set x 1;json info"]
    ) == [
        {"kind": "info"},
        {"kind": "health", "minutes": 2.0},
        {"kind": "validate", "tracks": ["a", "b"]},
        {"kind": "pace"},
        {"kind": "commands", "commands": ["set x 1", "json info"]},
    ]
    assert dev.parse_checks(["health"]) == [{"kind": "health", "minutes": 5.0}]
    for bad in ("validate", "camera", "cmd="):
        with pytest.raises(dev.DevError, match="unknown check"):
            dev.parse_checks([bad])


# -- git: dirty trees and --wip ----------------------------------------------------------------


def test_a_clean_head_builds_as_is_and_a_dirty_one_is_refused(repo: Any) -> None:
    head = git(repo.root, "rev-parse", "HEAD")
    assert dev.resolve(repo, None, False) == head
    (repo.root / "c.cpp").write_text("int c2;\n")
    with pytest.raises(dev.DevError, match=r"dirty \(c.cpp\).*--wip"):
        dev.resolve(repo, None, False)
    # An explicit ref is that commit, whatever the tree holds; --wip with it makes no sense.
    assert dev.resolve(repo, "origin/staging", False) == git(
        repo.root, "rev-parse", "origin/staging"
    )
    with pytest.raises(dev.DevError, match="no REF"):
        dev.resolve(repo, "HEAD", True)


def test_wip_commits_the_working_tree_without_touching_branch_index_or_stash(repo: Any) -> None:
    root = repo.root
    head = git(root, "rev-parse", "HEAD")
    (root / "c.cpp").write_text("int c2;\n")  # modified, unstaged
    (root / "a.cpp").write_text("int a2;\n")
    git(root, "add", "a.cpp")  # modified, staged
    (root / "new.h").write_text("#pragma once\n")  # untracked, not ignored
    (root / "build").mkdir()
    (root / "build" / "out.o").write_text("junk")  # ignored
    (root / "b.cpp").unlink()  # deleted
    index_before = (root / ".git" / "index").read_bytes()
    status_before = git(root, "status", "--porcelain")

    sha = dev.resolve(repo, None, True)

    assert (
        git(root, "rev-parse", "HEAD") == head
        and git(root, "branch", "--show-current") == "feature"
    )
    assert (root / ".git" / "index").read_bytes() == index_before
    assert git(root, "status", "--porcelain") == status_before
    assert git(root, "stash", "list") == ""
    assert git(root, "rev-parse", dev.WIP_REF) == sha and git(root, "rev-parse", f"{sha}^") == head
    files = set(git(root, "ls-tree", "-r", "--name-only", sha).splitlines())
    assert files == {".gitignore", "a.cpp", "c.cpp", "new.h"}  # b.cpp deleted, build/ ignored
    assert (
        git(root, "show", f"{sha}:c.cpp") == "int c2;"
        and git(root, "show", f"{sha}:a.cpp") == "int a2;"
    )


# -- bundles and their bases -------------------------------------------------------------------


def test_an_unpushed_commit_goes_as_a_bundle_on_the_staging_merge_base(repo: Any) -> None:
    svc = FakeService()
    head = git(repo.root, "rev-parse", "HEAD")
    rec = dev.submit(repo, svc, head)
    assert rec["id"] == "b1" and not svc.shas
    base, data = svc.bundles[0]
    assert base == git(repo.root, "rev-parse", "origin/staging")
    # The bundle holds base..head and applies to a clone that has the base.
    clone = repo.root.parent / "svc-clone"
    git(repo.root.parent, "clone", "-q", "--bare", str(repo.root.parent / "origin.git"), str(clone))
    (clone / "x.bundle").write_bytes(data)
    git(clone, "bundle", "verify", "x.bundle")
    git(clone, "fetch", "-q", "x.bundle", f"{dev.BUNDLE_REF}:refs/heads/built")
    assert git(clone, "rev-parse", "built") == head


def test_a_409_falls_back_to_the_master_merge_base(repo: Any) -> None:
    staging = git(repo.root, "rev-parse", "origin/staging")
    svc = FakeService(lacks=(staging,))
    dev.submit(repo, svc, git(repo.root, "rev-parse", "HEAD"))
    assert svc.bundles[0][0] == git(repo.root, "rev-parse", "origin/master")


def test_when_every_base_is_missing_it_says_what_to_push(repo: Any) -> None:
    lacks = (
        git(repo.root, "rev-parse", "origin/staging"),
        git(repo.root, "rev-parse", "origin/master"),
    )
    with pytest.raises(dev.DevError, match="git push origin feature"):
        dev.submit(repo, FakeService(lacks=lacks), git(repo.root, "rev-parse", "HEAD"))


def test_a_commit_on_a_branch_goes_by_sha(repo: Any) -> None:
    svc = FakeService()
    staging = git(repo.root, "rev-parse", "origin/staging")
    dev.submit(repo, svc, staging)
    assert svc.shas == [staging] and not svc.bundles


def test_a_failed_build_shows_its_log_tail() -> None:
    svc = FakeService()
    svc.states = ["queued", "failed"]
    with pytest.raises(dev.DevError, match=r"compile error\nfoo\.cpp:1: error"):
        dev.wait_build(svc, "b1")
    assert dev.wait_build(FakeService(), "b2")["version"] == "b220-dev-abcdef01"


# -- the build service's HTTP contract ---------------------------------------------------------


def test_the_build_service_contract_over_http(tmp_path: Path) -> None:
    seen: list[tuple[str, str, str | None, bytes]] = []
    answers = iter(
        [
            (202, {"id": "j", "state": "queued", "sha": "s"}),
            (202, {"id": "b"}),
            (409, {"detail": "prerequisites missing"}),
            (403, {"detail": "untrusted"}),
        ]
    )

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a: Any) -> None:
            pass

        def do_POST(self) -> None:
            body = self.rfile.read(int(self.headers["Content-Length"]))
            seen.append(
                (self.path, self.headers["Content-Type"], self.headers["Authorization"], body)
            )
            code, out = next(answers)
            data = json.dumps(out).encode()
            self.send_response(code)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        svc = dev.BuildService(f"http://127.0.0.1:{srv.server_address[1]}", "tok")
        assert svc.submit_sha("a" * 40)["id"] == "j"
        bundle = tmp_path / "x.bundle"
        bundle.write_bytes(b"# v2 git bundle\nBYTES")
        assert svc.submit_bundle(bundle, "a" * 40)["id"] == "b"
        with pytest.raises(dev.BaseMissing):
            svc.submit_bundle(bundle, "a" * 40)
        with pytest.raises(dev.DevError, match="refused the commit"):
            svc.submit_sha("a" * 40)
    finally:
        srv.shutdown()
    path, ctype, auth, body = seen[0]
    assert path == "/v1/builds" and ctype == "application/json" and auth == "Bearer tok"
    assert json.loads(body) == {"repo": "Jdubz/blinky-firmware", "sha": "a" * 40}
    path, ctype, auth, body = seen[1]
    assert ctype.startswith("multipart/form-data; boundary=")
    assert b'name="sha"\r\n\r\n' + b"a" * 40 in body and b"# v2 git bundle\nBYTES" in body


# -- the box -----------------------------------------------------------------------------------


class Box:
    """The box's server: mode, devices, dev deploy and run jobs."""

    def __init__(self, mode: str = "queue") -> None:
        self.mode: dict[str, Any] = {
            "mode": mode,
            "since": 0.0,
            "note": None,
            "switching": None,
            "waiting": [],
            "preempted": [],
        }
        self.calls: list[tuple[str, str, Any]] = []
        self.reset_refusals = 0

    def __call__(self, method: str, path: str, body: Any = None) -> Any:
        self.calls.append((method, path, body))
        if path == "/api/box/mode":
            if method == "PUT":
                self.mode.update(mode=body["mode"], note=body["note"], since=1_700_000_000.0)
            return dict(self.mode)
        if path == "/api/devices":
            return [
                {
                    "id": "87DE9B58C90AFE2D",
                    "state": "connected",
                    "platform": "nrf52840",
                    "device_type": "testbox_grid_v1",
                    "version": "b211",
                },
                {"id": "ESP", "state": "connected", "platform": "esp32s3", "device_type": None},
            ]
        if path == "/api/dev/deploy":
            if self.reset_refusals:
                self.reset_refusals -= 1
                raise dev.ServerError(409, "a chip reset 20 s ago", "41")
            return {"job_id": "d1"}
        if path == "/api/fleet/jobs/d1":
            return {
                "status": "complete",
                "progressMessage": "Deploy complete",
                "result": {
                    "status": "ok",
                    "version": "b220-dev-abcdef01",
                    "message": "deployed",
                    "steps": {
                        "flash": {
                            "per_device": {
                                "87DE9B58C90A": {"flash": "ok", "version": "b220-dev-abcdef01"}
                            }
                        }
                    },
                },
            }
        if path == "/api/dev/run":
            return {"job_id": "r1"}
        if path == "/api/test/jobs/r1":
            return {"status": "complete", "result": RUN_REPORT}
        if path == "/api/work":
            return {"in_flight": []}
        raise AssertionError(path)


DEV = "87DE9B58C90AFE2D"
RUN_REPORT = {
    "passed": True,
    "checks": [
        {
            "kind": "info",
            "devices": {
                DEV: {
                    "version": "b220-dev-abcdef01",
                    "timers": {"heapPeak": 9, "acfUs": 7185, "plpUs": 589},
                }
            },
        },
        {
            "kind": "health",
            "minutes": 1.0,
            "devices": {
                DEV: {
                    "fps": {"pass": True, "min": 150.0},
                    "frametime": {"pass": True, "max_ms": 15},
                    "stability": {"pass": True, "rebooted": False, "overrun_growth": 0},
                }
            },
        },
        {
            "kind": "validate",
            "tracks": 1,
            "played": 1,
            "results": [
                {
                    "track": "techno",
                    "scores": {DEV[:12]: {"drums": {"drumF": 0.71, "kickR50": 0.9}}},
                }
            ],
        },
    ],
    "final": {DEV: {"no_reboot": {"pass": True}, "alerts": {"pass": True, "lines": []}}},
}


def test_flash_waits_out_the_60_s_rule_once() -> None:
    box = Box("dev")
    box.reset_refusals = 1
    res = dev.flash(box, "b1", [DEV])
    assert res["status"] == "ok"
    assert [c[1] for c in box.calls].count("/api/dev/deploy") == 2


def test_a_switch_waits_for_the_suite_then_dev(no_sleep: list[float]) -> None:
    box = Box()
    states = iter([{"mode": "queue", "switching": {"waiting_for": ["s1"]}}, {"mode": "dev"}])
    box.mode = {"mode": "queue", "switching": {"waiting_for": ["s1"]}}
    original = box.__call__

    def server(method: str, path: str, body: Any = None) -> Any:
        if method == "GET" and path == "/api/box/mode":
            return next(states)
        return original(method, path, body)

    st = dev.wait_dev(server, {"mode": "queue", "switching": {"waiting_for": ["s1"]}})
    assert st["mode"] == "dev" and no_sleep


def test_try_enters_dev_mode_builds_flashes_tests_and_reports(
    repo: Any, capsys: pytest.CaptureFixture[str]
) -> None:
    box, svc = Box("queue"), FakeService()
    args = dev.main.__globals__["argparse"].Namespace(
        ref=None,
        wip=False,
        note=None,
        preempt=False,
        device=None,
        health=1.0,
        tracks="techno",
        pace=False,
        camera=False,
        info=False,
    )
    assert dev.cmd_try(args, box, svc, repo) == 0
    out = capsys.readouterr().out
    assert "switching it to dev mode" in out and "dev.py mode queue" in out
    put = next(c for c in box.calls if c[0] == "PUT")
    assert put[2]["mode"] == "dev" and put[2]["note"].startswith("dev.py try ")
    deploy = next(c for c in box.calls if c[1] == "/api/dev/deploy")
    assert deploy[2] == {"build_id": "b1", "device_ids": [DEV]}  # configured nRF52840s only
    run = next(c for c in box.calls if c[1] == "/api/dev/run")
    assert [ch["kind"] for ch in run[2]["checks"]] == ["info", "health", "validate"]
    report = out.split("== dev.py try ", 1)[1]
    assert "b220-dev-abcdef01" in report and "flash: ok" in report
    assert "acfUs=7185, plpUs=589, heapPeak=9" in report  # the named timers first
    assert "drumF=0.71" in report and "verdict (health + asserts): PASSED" in report
    assert "box: DEV mode since" in report


def test_try_refuses_a_dirty_tree_before_touching_the_box(repo: Any) -> None:
    (repo.root / "c.cpp").write_text("dirty")
    box = Box("queue")
    args = dev.main.__globals__["argparse"].Namespace(
        ref=None,
        wip=False,
        note=None,
        preempt=False,
        device=None,
        health=None,
        tracks=None,
        pace=False,
        camera=False,
        info=False,
    )
    with pytest.raises(dev.DevError, match="dirty"):
        dev.cmd_try(args, box, FakeService(), repo)
    assert box.calls == []


def test_status_shows_what_waits_behind_dev_mode(capsys: pytest.CaptureFixture[str]) -> None:
    box = Box("dev")
    box.mode["waiting"] = [{"runner": "staging", "sha": "s" * 40, "since": 0.0, "preempted": True}]
    assert dev.main(["status"], server=box) == 0
    out = capsys.readouterr().out
    assert "box: DEV mode since" in out and "waiting behind dev mode: 1" in out
    assert "staging: ssssssssssss" in out and "preempted" in out and "dev.py mode queue" in out
    assert "chip 87DE9B58C90A testbox_grid_v1: connected, b211" in out


def test_a_cache_hit_is_done_at_once(
    no_sleep: list[float], capsys: pytest.CaptureFixture[str]
) -> None:
    """The live service answers 202 with state done for a commit it has built already."""
    svc = FakeService()
    svc.states = ["done"]
    build_id = dev.started({"id": "b9", "state": "done", "sha": "a" * 40}, "a" * 40)
    rec = dev.wait_build(svc, build_id)
    assert rec["state"] == "done" and not no_sleep
    assert "already built (cache hit)" in capsys.readouterr().out
    with pytest.raises(dev.DevError, match="answered for"):
        dev.started({"id": "b9", "state": "done", "sha": "b" * 40}, "a" * 40)
