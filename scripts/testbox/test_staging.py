"""The test box's staging runner (staging.py, #349): one pass from staging head to report."""

from __future__ import annotations

import base64
import importlib.util
import json
import sys
import urllib.error
from pathlib import Path
from typing import Any

import pytest

_spec = importlib.util.spec_from_file_location("staging", Path(__file__).with_name("staging.py"))
assert _spec and _spec.loader
staging = importlib.util.module_from_spec(_spec)
sys.modules["staging"] = staging
_spec.loader.exec_module(staging)

SHA = "s" * 40
HAT = {"id": "C04C56F9", "state": "connected", "platform": "nrf52840", "device_type": "hat_v1"}
LAMP = {"id": "AAAA", "state": "connected", "platform": "nrf52840", "device_type": "lamp"}


class GitHub:
    """gh as a fake: answers for one staging commit, records what was posted."""

    def __init__(
        self,
        artifact: bool = True,
        runs: list[dict[str, Any]] | None = None,
        changed: list[str] | None = None,
        paths: list[str] | None = None,
        no_path_list: bool = False,
    ) -> None:
        self.artifact, self.runs = artifact, runs or []
        # What staging changed since master, and the firmware's path list at the head
        # (no_path_list: the file isn't there). By default a firmware change, so the suite runs.
        self.changed = ["blinky-things/audio/AudioTracker.cpp"] if changed is None else changed
        self.paths = ["blinky-things/", "scripts/build.sh"] if paths is None else paths
        self.no_path_list = no_path_list
        self.fail_merge = False
        self.statuses: list[tuple[str, str]] = []
        self.comments: list[tuple[str, str]] = []
        self.calls: list[tuple[str, ...]] = []
        self.open_promotion: list[dict[str, Any]] = []

    def __call__(self, *args: str, stdin: str | None = None) -> str:
        self.calls.append(args)
        a = " ".join(args)
        if a.startswith(f"api repos/{staging.GH_REPO}/branches/staging"):
            return json.dumps({"commit": {"sha": SHA}})
        if "actions/artifacts?name=staging-firmware-" + SHA in a:
            arts = [{"expired": False}] if self.artifact else []
            return json.dumps({"artifacts": arts})
        if f"contents/{staging.FIRMWARE_PATHS}?ref={SHA}" in a:
            if self.no_path_list:
                raise RuntimeError("gh api repos/... -> 1: HTTP 404: Not Found")
            raw = json.dumps({"paths": self.paths}).encode()
            return json.dumps({"content": base64.b64encode(raw).decode()})
        if f"compare/master...{SHA}" in a:
            return json.dumps(
                {
                    "merge_base_commit": {"sha": "m" * 40},
                    "files": [{"filename": f} for f in self.changed],
                }
            )
        if f"actions/runs?head_sha={SHA}" in a:
            return json.dumps({"workflow_runs": self.runs})
        if f"statuses/{SHA}" in a:
            st = dict(x.split("=", 1) for x in args if "=" in x and not x.startswith("repos"))
            self.statuses.append((st["state"], st["description"]))
            return "{}"
        if f"commits/{SHA}/pulls" in a:
            return json.dumps([{"number": 12, "merged_at": "x", "base": {"ref": "staging"}}])
        if args[:2] == ("pr", "comment"):
            self.comments.append((args[2], args[-1]))
            return f"https://github.com/{staging.GH_REPO}/pull/{args[2]}#issuecomment-1\n"
        if args[:2] == ("pr", "list"):
            return json.dumps(self.open_promotion)
        if args[:2] == ("pr", "create"):
            return f"https://github.com/{staging.GH_REPO}/pull/77\n"
        if args[:2] == ("pr", "merge"):
            if self.fail_merge:
                raise RuntimeError("gh pr merge -> 1: GraphQL: something went wrong")
            return ""
        raise AssertionError(args)


class Server:
    def __init__(self, devices: list[dict[str, Any]], job: dict[str, Any] | Exception) -> None:
        self.devices, self.job = devices, job
        self.submitted: list[Any] = []

    def __call__(self, method: str, path: str, body: Any = None) -> Any:
        if path == "/api/devices":
            return self.devices
        if method == "POST" and path == "/api/test/staging":
            self.submitted.append(body)
            return {"job_id": "job-1"}
        if path == "/api/test/jobs/job-1":
            if isinstance(self.job, Exception):
                raise self.job
            return self.job
        raise AssertionError(path)


@pytest.fixture(autouse=True)
def state_file(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setattr(staging, "STATE", tmp_path / "staging.json")
    return tmp_path / "staging.json"


def listed() -> set[str]:
    return {"hat_v1"}


GOOD_REPORT = {
    "sha": SHA,
    "passed": True,
    "version": "b216-dev-ssssssss",
    "devices": ["C04C56F9"],
    "deploy": {"pass": True},
    "health": {"C04C56F9": {"fps": {"pass": True, "min": 120.0}, "alerts": []}},
    "music": {"C04C56F9": {"music": {"pass": True}, "beatpace": {"pass": True}, "alerts": []}},
    "validation": {
        "pass": True,
        "tracks": 1,
        "played": 1,
        "results": [{"track": "t", "scores": {"C04C56F9": {"drums": {"kickR50": 0.8}}}}],
    },
    "final": {
        "C04C56F9": {
            "version": {"pass": True},
            "alerts": {"pass": True, "lines": []},
            "timing": {"onsetUs": 700},
        }
    },
}


def test_a_built_head_is_submitted_for_the_test_devices_only() -> None:
    g, srv = GitHub(), Server([HAT, LAMP, {**HAT, "id": "X", "state": "disconnected"}], {})
    state = staging.run_once(g, srv, listed)
    assert srv.submitted == [
        {"sha": SHA, "device_ids": ["C04C56F9"], "track_dir": str(staging.LINK / staging.CORPUS)}
    ]
    assert state["running"]["job"] == "job-1" and g.statuses[-1][0] == "pending"


def test_a_running_suite_is_left_alone() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "running", "progressMessage": "health"})
    staging.run_once(g, srv, listed)
    staging.run_once(g, srv, listed)
    assert len(srv.submitted) == 1 and [s[0] for s in g.statuses] == ["pending"]


def test_a_pass_reports_and_promotes_with_a_merge_commit() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    staging.run_once(g, srv, listed)
    state = staging.run_once(g, srv, listed)
    assert state["reported"] == SHA and "running" not in state
    assert g.statuses[-1][0] == "success"
    pr, text = g.comments[0]
    assert pr == "12" and text.startswith(staging.REPORT_HEADER + SHA) and "PASSED" in text
    assert "onsetUs" in text and "0.8" in text
    merge = (
        "pr",
        "merge",
        "77",
        "--repo",
        staging.GH_REPO,
        "--merge",
        "--auto",
        "--match-head-commit",
        SHA,
    )
    assert merge in g.calls
    # Reported once: the next pass does nothing.
    staging.run_once(g, srv, listed)
    assert len(srv.submitted) == 1 and len(g.comments) == 1


def test_a_pass_on_a_superseded_head_is_reported_but_not_promoted() -> None:
    """Staging moved on while the suite ran: the tested commit gets its status, the new head
    its own run; nothing untested reaches the PR to master (#351 review)."""
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    staging.run_once(g, srv, listed)
    newer = "n" * 40
    real = g.__call__

    def moved(*args: str, stdin: str | None = None) -> str:
        if " ".join(args).startswith(f"api repos/{staging.GH_REPO}/branches/staging"):
            return json.dumps({"commit": {"sha": newer}})
        return real(*args, stdin=stdin)

    state = staging.run_once(moved, srv, listed)
    assert g.statuses[-1][0] == "success" and state["reported"] == SHA
    assert not any(c[:2] in (("pr", "create"), ("pr", "merge")) for c in g.calls)


def test_a_run_from_before_the_split_finishes_in_its_own_repo() -> None:
    """A suite started for blinky_time before the runner moved to blinky-firmware (no "repo"
    in its state) is reported against blinky_time, and never promoted (#362 review)."""
    staging.save_state({"running": {"sha": SHA, "job": "job-1", "devices": ["C04C56F9"]}})
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    state = staging.run_once(g, srv, listed)
    assert state["reported"] == SHA and "running" not in state
    status = [
        c
        for c in g.calls
        if c[:4] == ("api", "-X", "POST", f"repos/{staging.LEGACY_REPO}/statuses/{SHA}")
    ]
    assert status and not any(c[:2] in (("pr", "create"), ("pr", "merge")) for c in g.calls)
    assert all(staging.LEGACY_REPO in c for c in g.calls if c[:2] == ("pr", "comment"))


def test_a_failed_report_fails_the_status_and_promotes_nothing() -> None:
    bad = {**GOOD_REPORT, "passed": False}
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": bad})
    staging.run_once(g, srv, listed)
    staging.run_once(g, srv, listed)
    assert g.statuses[-1][0] == "failure" and "FAILED" in g.comments[0][1]
    assert not any(c[:2] in (("pr", "create"), ("pr", "merge")) for c in g.calls)


@pytest.mark.parametrize(
    "job",
    [
        {"status": "error", "error": "Traceback: boom"},
        urllib.error.HTTPError("u", 404, "not found", None, None),  # type: ignore[arg-type]
    ],
)
def test_a_crashed_or_lost_suite_is_a_failure(job: Any) -> None:
    g, srv = GitHub(), Server([HAT], job)
    staging.run_once(g, srv, listed)
    state = staging.run_once(g, srv, listed)
    assert g.statuses[-1][0] == "failure" and state["reported"] == SHA


def test_an_unbuilt_head_waits_and_a_failed_build_fails() -> None:
    g, srv = GitHub(artifact=False), Server([HAT], {})
    staging.run_once(g, srv, listed)
    staging.run_once(g, srv, listed)
    assert g.statuses == [("pending", "waiting for the staging build")] and not srv.submitted
    g.runs = [
        {
            "path": ".github/workflows/staging-build.yml",
            "status": "completed",
            "conclusion": "failure",
            "event": "workflow_run",
        }
    ]
    state = staging.run_once(g, srv, listed)
    assert g.statuses[-1][0] == "failure" and state["reported"] == SHA and not srv.submitted


def test_no_test_device_waits_without_reporting() -> None:
    g, srv = GitHub(), Server([LAMP], {})
    state = staging.run_once(g, srv, listed)
    staging.run_once(g, srv, listed)
    assert "running" not in state and "reported" not in state and not srv.submitted
    assert g.statuses == [("pending", "no test device connected to the box")]  # posted once


class Refusing(Server):
    def __init__(self, code: int) -> None:
        super().__init__([HAT], {})
        self.code = code

    def __call__(self, method: str, path: str, body: Any = None) -> Any:
        if method == "POST":
            import io

            raise urllib.error.HTTPError(
                path,
                self.code,
                "x",
                {},
                io.BytesIO(b'{"detail": "not test devices"}'),  # type: ignore[arg-type]
            )
        return super().__call__(method, path, body)


@pytest.mark.parametrize("code", [400, 403, 422])
def test_a_refused_suite_is_a_failure_not_a_retry(code: int) -> None:
    g = GitHub()
    state = staging.run_once(g, Refusing(code), listed)
    assert state["reported"] == SHA and g.statuses[-1][0] == "failure"
    assert "not test devices" in g.statuses[-1][1]


@pytest.mark.parametrize("code", [409, 429, 500, 502, 503])
def test_a_busy_or_failing_server_is_retried(code: int) -> None:
    g = GitHub()
    state = staging.run_once(g, Refusing(code), listed)
    assert "reported" not in state and not g.statuses


def test_alert_lines_are_in_the_report() -> None:
    bad = {**GOOD_REPORT, "passed": False}
    bad["final"] = {
        "C04C56F9": {"alerts": {"pass": False, "lines": ["[FALLBACK] x"]}, "timing": None}
    }
    text = staging.render(bad)
    assert "| alerts | ❌ |" in text and "C04C56F9 [FALLBACK] x" in text


def test_a_deploy_failure_renders_its_step() -> None:
    text = staging.render(
        {
            "sha": SHA,
            "passed": False,
            "devices": ["C04C56F9"],
            "deploy": {"pass": False, "failed_step": "targets", "message": "not test devices"},
        }
    )
    assert "FAILED" in text and "`targets`" in text and "not test devices" in text


# -- no firmware change, no hardware test (the owner, 2026-11-05) -----------------------------


def test_a_head_with_no_firmware_change_is_promoted_without_a_run() -> None:
    g = GitHub(changed=[".github/workflows/pr-validation.yml", "docs/RELEASE_PROCESS.md"])
    srv = Server([HAT], {})
    state = staging.run_once(g, srv, listed)
    assert srv.submitted == []
    assert g.statuses == [
        ("success", "no firmware change since master mmmmmmmmmmmm: hardware test skipped")
    ]
    assert state["reported"] == SHA
    assert ("pr", "create") in [c[:2] for c in g.calls]
    merge = next(c for c in g.calls if c[:2] == ("pr", "merge"))
    assert merge[-2:] == ("--match-head-commit", SHA)
    # Reported: the next pass does nothing.
    g.calls.clear()
    staging.run_once(g, srv, listed)
    assert [c for c in g.calls if c[0] == "pr"] == []


@pytest.mark.parametrize(
    "changed",
    [
        ["docs/x.md", "blinky-things/generators/Fire.cpp"],  # a directory prefix
        ["scripts/build.sh"],  # an exact path
    ],
)
def test_a_firmware_change_gets_the_suite(changed: list[str]) -> None:
    g, srv = GitHub(changed=changed), Server([HAT], {})
    staging.run_once(g, srv, listed)
    assert len(srv.submitted) == 1
    assert not any(s == "success" for s, _ in g.statuses)


def test_a_rename_out_of_the_firmware_counts_as_a_change() -> None:
    g = GitHub(changed=["docs/old.cpp"])
    real = g.__call__

    def call(*args: str, stdin: str | None = None) -> str:
        out = real(*args, stdin=stdin)
        if "compare/master..." in " ".join(args):
            d = json.loads(out)
            d["files"] = [{"filename": "docs/old.cpp", "previous_filename": "blinky-things/old.cpp"}]
            return json.dumps(d)
        return out

    srv = Server([HAT], {})
    staging.run_once(call, srv, listed)
    assert len(srv.submitted) == 1


@pytest.mark.parametrize(
    "gh_kwargs",
    [
        {"changed": ["docs/x.md"], "no_path_list": True},  # no list at that commit
        {"changed": ["docs/x.md"], "paths": []},  # an empty list
        {"changed": [f"docs/{i}.md" for i in range(300)]},  # GitHub truncated the compare
    ],
)
def test_anything_that_cant_be_judged_gets_the_suite(gh_kwargs: dict[str, Any]) -> None:
    g, srv = GitHub(**gh_kwargs), Server([HAT], {})
    staging.run_once(g, srv, listed)
    assert len(srv.submitted) == 1


def test_a_skipped_head_whose_promotion_fails_is_retried() -> None:
    g, srv = GitHub(changed=["docs/x.md"]), Server([HAT], {})
    g.fail_merge = True
    with pytest.raises(RuntimeError):
        staging.run_once(g, srv, listed)
    assert staging.load_state().get("reported") != SHA
    g.fail_merge = False
    state = staging.run_once(g, srv, listed)
    assert state["reported"] == SHA
    assert srv.submitted == []
