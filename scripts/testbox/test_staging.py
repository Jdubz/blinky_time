"""The test box's staging runner (staging.py, #349): one pass from staging head to report."""

from __future__ import annotations

import base64
import importlib.util
import json
import subprocess
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
TESTED = "t" * 40  # the staging commit master was last merged from (master's second parent)
FP_A, FP_B = "sha256:" + "a" * 64, "sha256:" + "b" * 64
PR_HEAD = "h" * 40  # the last commit of PR #12, the one that merged SHA into staging


def verdict(head: str, headline: str) -> dict[str, Any]:
    """The review workflow's sha-stamped verdict comment (scripts/post-review-verdict.sh)."""
    body = f"## Claude review — {head}\n\n{headline}\n\ndetails"
    return {"user": {"login": "github-actions[bot]"}, "body": body}


def fp_status(description: str, state: str = "success") -> dict[str, Any]:
    return {"context": staging.FINGERPRINT_CONTEXT, "state": state, "description": description}


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
        unfingerprinted: list[str] | None = None,
        fingerprints: dict[str, list[dict[str, Any]]] | None = None,
        master_parents: list[str] | None = None,
    ) -> None:
        self.artifact, self.runs = artifact, runs or []
        # What staging changed since master, and the firmware's path list at the head
        # (no_path_list: the file isn't there). By default a firmware change, so the suite runs.
        self.changed = ["blinky-things/audio/AudioTracker.cpp"] if changed is None else changed
        self.paths = ["blinky-things/", "scripts/build.sh"] if paths is None else paths
        self.no_path_list = no_path_list
        self.unfingerprinted = ["bootloader/"] if unfingerprinted is None else unfingerprinted
        # Each commit's statuses, newest first. By default the head's image differs from
        # master's, so the suite runs.
        self.fingerprints = (
            {SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_B)]}
            if fingerprints is None
            else fingerprints
        )
        self.master_parents = ["p" * 40, TESTED] if master_parents is None else master_parents
        self.fail_fingerprints = False
        self.fail_merge = False
        self.statuses: list[tuple[str, str]] = []
        self.comments: list[tuple[str, str]] = []
        self.calls: list[tuple[str, ...]] = []
        self.open_promotion: list[dict[str, Any]] = []
        # Comments on PR #12: by default a clean review of its last commit.
        self.pr_comments: list[dict[str, Any]] = [verdict(PR_HEAD, "No issues found. Checked.")]
        self.fail_reviews = False

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
            spec = {"paths": self.paths, "unfingerprinted": self.unfingerprinted}
            raw = json.dumps(spec).encode()
            return json.dumps({"content": base64.b64encode(raw).decode()})
        if args[:2] == ("api", "--paginate") and f"compare/master...{SHA}" in a:
            return SHA + "\n"
        if args[:2] == ("api", "--paginate") and "issues/12/comments" in a:
            if self.fail_reviews:
                raise subprocess.TimeoutExpired(["gh", *args], 120)
            return "".join(json.dumps(c) + "\n" for c in self.pr_comments)
        if f"compare/master...{SHA}" in a:
            return json.dumps(
                {
                    "merge_base_commit": {"sha": "m" * 40},
                    "files": [{"filename": f} for f in self.changed],
                }
            )
        if "/statuses?per_page=" in a:
            if self.fail_fingerprints:
                raise RuntimeError("gh api repos/... -> 1: HTTP 502: Bad Gateway")
            sha = a.split("/commits/", 1)[1].split("/", 1)[0]
            return json.dumps(self.fingerprints.get(sha, []))
        if a.endswith(f"repos/{staging.GH_REPO}/commits/master"):
            return json.dumps({"parents": [{"sha": p} for p in self.master_parents]})
        if f"actions/runs?head_sha={SHA}" in a:
            return json.dumps({"workflow_runs": self.runs})
        if f"statuses/{SHA}" in a:
            st = dict(x.split("=", 1) for x in args if "=" in x and not x.startswith("repos"))
            self.statuses.append((st["state"], st["description"]))
            return "{}"
        if f"commits/{SHA}/pulls" in a:
            return json.dumps(
                [
                    {
                        "number": 12,
                        "merged_at": "x",
                        "base": {"ref": "staging"},
                        "head": {"sha": PR_HEAD},
                        "title": "Fix the tracker",
                    }
                ]
            )
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
        # The box's mode (GET /api/box/mode) and what the runner reports as waiting.
        self.mode: dict[str, Any] = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
        self.queue: list[list[dict[str, Any]]] = []

    def __call__(self, method: str, path: str, body: Any = None) -> Any:
        if path == "/api/devices":
            return self.devices
        if method == "GET" and path == "/api/box/mode":
            if isinstance(self.mode, Exception):
                raise self.mode
            return self.mode
        if method == "PUT" and path == "/api/box/queue/staging":
            self.queue.append(body["waiting"])
            return {}
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


TESTBOX = "http://testbox.example.invalid:8420"  # reserved name: tests never name a real host


@pytest.fixture(autouse=True)
def hosts_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Never the machine's real ~/.config/blinky/hosts.env: a file per test, the env var unset."""
    path = tmp_path / "hosts.env"
    path.write_text(f'BLINKY_TESTBOX_URL="{TESTBOX}/"\n')
    monkeypatch.setattr(staging, "HOSTS_ENV", path)
    monkeypatch.delenv("BLINKY_TESTBOX_URL", raising=False)
    return path


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


def test_a_built_head_is_submitted_for_every_connected_configured_chip() -> None:
    unconfigured = {**HAT, "id": "Y", "device_type": None}
    disconnected = {**HAT, "id": "X", "state": "disconnected"}
    g, srv = GitHub(), Server([HAT, LAMP, disconnected, unconfigured], {})
    state = staging.run_once(g, srv)
    assert srv.submitted == [
        {
            "sha": SHA,
            "device_ids": ["C04C56F9", "AAAA"],
            "track_dir": str(staging.LINK / staging.CORPUS),
        }
    ]
    assert state["running"]["job"] == "job-1" and g.statuses[-1][0] == "pending"


def test_a_running_suite_is_left_alone() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "running", "progressMessage": "health"})
    staging.run_once(g, srv)
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1 and [s[0] for s in g.statuses] == ["pending"]


def test_a_pass_reports_and_promotes_with_a_merge_commit() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    staging.run_once(g, srv)
    state = staging.run_once(g, srv)
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
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1 and len(g.comments) == 1


def test_a_pass_on_a_superseded_head_is_reported_but_not_promoted() -> None:
    """Staging moved on while the suite ran: the tested commit gets its status, the new head
    its own run; nothing untested reaches the PR to master (#351 review)."""
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    staging.run_once(g, srv)
    newer = "n" * 40
    real = g.__call__

    def moved(*args: str, stdin: str | None = None) -> str:
        if " ".join(args).startswith(f"api repos/{staging.GH_REPO}/branches/staging"):
            return json.dumps({"commit": {"sha": newer}})
        return real(*args, stdin=stdin)

    state = staging.run_once(moved, srv)
    assert g.statuses[-1][0] == "success" and state["reported"] == SHA
    assert not any(c[:2] in (("pr", "create"), ("pr", "merge")) for c in g.calls)


def test_a_failed_report_fails_the_status_and_promotes_nothing() -> None:
    bad = {**GOOD_REPORT, "passed": False}
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": bad})
    staging.run_once(g, srv)
    staging.run_once(g, srv)
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
    staging.run_once(g, srv)
    state = staging.run_once(g, srv)
    assert g.statuses[-1][0] == "failure" and state["reported"] == SHA


def test_an_unbuilt_head_waits_and_a_failed_build_fails() -> None:
    g, srv = GitHub(artifact=False), Server([HAT], {})
    staging.run_once(g, srv)
    staging.run_once(g, srv)
    assert g.statuses == [("pending", "waiting for the staging build")] and not srv.submitted
    g.runs = [
        {
            "path": ".github/workflows/staging-build.yml",
            "status": "completed",
            "conclusion": "failure",
            "event": "workflow_run",
        }
    ]
    state = staging.run_once(g, srv)
    assert g.statuses[-1][0] == "failure" and state["reported"] == SHA and not srv.submitted


def test_no_test_device_waits_without_reporting() -> None:
    # Only chips that can't run the suite: one disconnected, one with no device config.
    g, srv = GitHub(), Server([{**LAMP, "state": "disconnected"}, {**HAT, "device_type": None}], {})
    state = staging.run_once(g, srv)
    staging.run_once(g, srv)
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
    state = staging.run_once(g, Refusing(code))
    assert state["reported"] == SHA and g.statuses[-1][0] == "failure"
    assert "not test devices" in g.statuses[-1][1]


@pytest.mark.parametrize("code", [409, 429, 500, 502, 503])
def test_a_busy_or_failing_server_is_retried(code: int) -> None:
    g = GitHub()
    state = staging.run_once(g, Refusing(code))
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
    state = staging.run_once(g, srv)
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
    staging.run_once(g, srv)
    assert [c for c in g.calls if c[0] == "pr"] == []


# -- same firmware image, no hardware test (Firmware fingerprint) ------------------------------


def test_a_head_with_masters_image_is_promoted_without_a_run() -> None:
    """A firmware path changed (a comment in blinky-things/), but the image didn't."""
    g = GitHub(fingerprints={SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_A)]})
    srv = Server([HAT], {})
    state = staging.run_once(g, srv)
    assert srv.submitted == []
    assert g.statuses == [
        ("success", "same firmware image as master's (sha256:aaaaaaaaaaaa): hardware test skipped")
    ]
    assert state["reported"] == SHA
    assert ("pr", "merge") in [c[:2] for c in g.calls]


def test_the_image_is_only_compared_once_the_head_is_built() -> None:
    g = GitHub(artifact=False, fingerprints={SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_A)]})
    staging.run_once(g, Server([HAT], {}))
    assert g.statuses == [("pending", "waiting for the staging build")]
    assert not any("/statuses?per_page=" in " ".join(c) for c in g.calls)


@pytest.mark.parametrize(
    "fingerprints, master_parents",
    [
        ({SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_B)]}, None),  # a different image
        ({TESTED: [fp_status(FP_A)]}, None),  # the head has none (built before fingerprints)
        ({SHA: [fp_status(FP_A)]}, None),  # master's tested commit has none
        ({SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_A)]}, [TESTED]),  # master not a merge
        # Only the newest status counts, and only a successful one: an older match behind a
        # newer different, failed or pending fingerprint is not a match.
        ({SHA: [fp_status(FP_B), fp_status(FP_A)], TESTED: [fp_status(FP_A)]}, None),
        ({SHA: [fp_status(FP_A, "error"), fp_status(FP_A)], TESTED: [fp_status(FP_A)]}, None),
        ({SHA: [fp_status(FP_A, "pending"), fp_status(FP_A)], TESTED: [fp_status(FP_A)]}, None),
        # A malformed fingerprint matches nothing, not even itself.
        ({SHA: [fp_status("sha256:")], TESTED: [fp_status("sha256:")]}, None),
        ({SHA: [fp_status("sha256:garbage")], TESTED: [fp_status("sha256:garbage")]}, None),
        ({SHA: [fp_status("sha256:" + "A" * 64)], TESTED: [fp_status("sha256:" + "A" * 64)]}, None),
    ],
)
def test_anything_short_of_the_same_image_gets_the_suite(
    fingerprints: dict[str, list[dict[str, Any]]], master_parents: list[str] | None
) -> None:
    g = GitHub(fingerprints=fingerprints, master_parents=master_parents)
    srv = Server([HAT], {})
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1
    assert not any(c[:2] == ("pr", "create") for c in g.calls)


def test_a_change_the_fingerprint_cant_see_gets_the_suite() -> None:
    """A bootloader change leaves the app image alone; equal fingerprints don't vouch for it."""
    g = GitHub(
        changed=["bootloader/README.md", "blinky-things/x.h"],
        paths=["blinky-things/", "bootloader/"],
        fingerprints={SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_A)]},
    )
    srv = Server([HAT], {})
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1
    assert not any("/statuses?per_page=" in " ".join(c) for c in g.calls)


def test_unreadable_fingerprints_get_the_suite() -> None:
    g = GitHub(fingerprints={SHA: [fp_status(FP_A)], TESTED: [fp_status(FP_A)]})
    g.fail_fingerprints = True
    srv = Server([HAT], {})
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1


@pytest.mark.parametrize(
    "changed",
    [
        ["docs/x.md", "blinky-things/generators/Fire.cpp"],  # a directory prefix
        ["scripts/build.sh"],  # an exact path
    ],
)
def test_a_firmware_change_gets_the_suite(changed: list[str]) -> None:
    g, srv = GitHub(changed=changed), Server([HAT], {})
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1
    assert not any(s == "success" for s, _ in g.statuses)


def test_a_rename_out_of_the_firmware_counts_as_a_change() -> None:
    g = GitHub(changed=["docs/old.cpp"])
    real = g.__call__

    def call(*args: str, stdin: str | None = None) -> str:
        out = real(*args, stdin=stdin)
        if "compare/master..." in " ".join(args):
            d = json.loads(out)
            d["files"] = [
                {"filename": "docs/old.cpp", "previous_filename": "blinky-things/old.cpp"}
            ]
            return json.dumps(d)
        return out

    srv = Server([HAT], {})
    staging.run_once(call, srv)
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
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1


def test_a_skipped_head_whose_promotion_fails_is_retried() -> None:
    g, srv = GitHub(changed=["docs/x.md"]), Server([HAT], {})
    g.fail_merge = True
    with pytest.raises(RuntimeError):
        staging.run_once(g, srv)
    assert staging.load_state().get("reported") != SHA
    g.fail_merge = False
    state = staging.run_once(g, srv)
    assert state["reported"] == SHA
    assert srv.submitted == []


# -- review verdicts (informational) ----------------------------------------------------------


def test_the_report_and_the_promotion_list_each_prs_review_verdict() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    staging.run_once(g, srv)
    staging.run_once(g, srv)
    report = g.comments[0][1]
    assert staging.REVIEWS_HEADER in report
    assert "- ✅ #12: review clean (Fix the tracker)" in report
    create = next(c for c in g.calls if c[:2] == ("pr", "create"))
    assert "- ✅ #12: review clean" in create[create.index("--body") + 1]


def test_a_flagged_review_is_shown_but_does_not_block_the_promotion() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    g.pr_comments = [
        verdict("o" * 40, "No issues found."),  # an older commit's clean review doesn't count
        verdict(PR_HEAD, "Found 1 issue: off-by-one in the picker."),
    ]
    staging.run_once(g, srv)
    state = staging.run_once(g, srv)
    assert "- ⚠️ #12: review of hhhhhhhhhhhh: Found 1 issue" in g.comments[0][1]
    assert g.statuses[-1][0] == "success" and state["reported"] == SHA
    assert any(c[:2] == ("pr", "merge") for c in g.calls)  # promoted anyway


def test_a_missing_verdict_is_shown_as_such() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    g.pr_comments = []
    staging.run_once(g, srv)
    staging.run_once(g, srv)
    assert "- ⚠️ #12: no review verdict for hhhhhhhhhhhh" in g.comments[0][1]


def test_a_stalled_review_lookup_is_a_note_not_a_failure() -> None:
    # gh's 120 s timeout raises TimeoutExpired, not RuntimeError: still only a note.
    g, srv = GitHub(), Server([HAT], {"status": "complete", "result": GOOD_REPORT})
    g.fail_reviews = True
    staging.run_once(g, srv)
    state = staging.run_once(g, srv)
    assert f"{staging.REVIEWS_HEADER} couldn't read them" in g.comments[0][1]
    assert g.statuses[-1][0] == "success" and state["reported"] == SHA


def test_the_report_links_its_archived_copy_through_the_box() -> None:
    text = staging.render({**GOOD_REPORT, "artifact": "inputs/abc/s.json"})
    assert "`inputs/abc/s.json`" in text
    assert f"{TESTBOX}/api/spark/artifacts/inputs/abc/s.json" in text
    assert "artifact store" not in staging.render(GOOD_REPORT)


# -- the box's dev mode (docs/DEV_LOOP.md) -----------------------------------------------------


def dev_mode(srv: Server, since: float = 1_700_000_000.0, others: int = 0) -> None:
    waiting = [{"runner": "pr", "sha": "x" * 40, "since": since} for _ in range(others)]
    srv.mode = {"mode": "dev", "since": since, "waiting": waiting, "preempted": []}


def test_dev_mode_holds_a_head_with_a_pending_status_once() -> None:
    import time

    g, srv = GitHub(), Server([HAT], {})
    dev_mode(srv, others=2)
    state = staging.run_once(g, srv)
    staging.run_once(g, srv)
    when = time.strftime("%H:%M", time.localtime(1_700_000_000.0))
    assert not srv.submitted and "running" not in state and "reported" not in state
    assert g.statuses == [("pending", f"queued: box in dev mode since {when} (3 waiting)")]
    # Reported to the server as waiting, with the time it started waiting, every pass.
    assert [w[0]["sha"] for w in srv.queue] == [SHA, SHA]
    assert srv.queue[0][0]["since"] == srv.queue[1][0]["since"] == state["queued"]["since"]


def test_a_switch_still_waiting_for_its_suite_holds_too() -> None:
    g, srv = GitHub(), Server([HAT], {})
    srv.mode = {"mode": "queue", "switching": {"to": "dev"}, "waiting": [], "preempted": []}
    staging.run_once(g, srv)
    assert not srv.submitted
    assert g.statuses == [("pending", "queued: box switching to dev mode (1 waiting)")]


def test_back_in_queue_mode_the_held_head_is_submitted_and_the_queue_cleared() -> None:
    g, srv = GitHub(), Server([HAT], {})
    dev_mode(srv)
    staging.run_once(g, srv)
    srv.mode = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
    state = staging.run_once(g, srv)
    assert len(srv.submitted) == 1 and state["running"]["sha"] == SHA and "queued" not in state
    assert srv.queue[-1] == [] and g.statuses[-1] == ("pending", "testing on 1 test device(s)")


def test_it_takes_turns_with_the_pr_queue_oldest_ready_item_first() -> None:
    """Queue mode: a ready head is reported as waiting; a PR head that is ready and has waited
    longer goes first (while the PR runner still reports), then this one."""
    import time

    g, srv = GitHub(), Server([HAT], {})
    now = time.time()
    pr = {"runner": "pr", "sha": "p" * 40, "since": now - 600, "ready": True, "reported_at": now}
    srv.mode["waiting"] = [pr]
    state = staging.run_once(g, srv)
    assert not srv.submitted and state["queued"]["ready"] and "reported" not in state
    assert srv.queue[-1] == [
        {"sha": SHA, "since": state["queued"]["since"], "what": "staging head", "ready": True}
    ]
    assert g.statuses == []  # waiting for a turn says nothing on GitHub
    srv.mode["waiting"] = [{**pr, "since": now + 600}]  # a newer one: this head goes first
    state = staging.run_once(g, srv)
    assert len(srv.submitted) == 1 and state["running"]["sha"] == SHA and srv.queue[-1] == []


def test_a_stopped_pr_runner_never_blocks_staging() -> None:
    import time

    g, srv = GitHub(), Server([HAT], {})
    old = {"runner": "pr", "sha": "p" * 40, "since": 1.0, "ready": True}
    srv.mode["waiting"] = [{**old, "reported_at": time.time() - 3600}]
    assert staging.run_once(g, srv)["running"]["sha"] == SHA


def test_a_busy_box_keeps_the_head_waiting_with_its_first_time() -> None:
    g, srv = GitHub(), Refusing(409)
    first = staging.run_once(g, srv)["queued"]["since"]
    assert staging.run_once(g, srv)["queued"]["since"] == first
    assert srv.queue[-1][0]["since"] == first


def test_a_head_settled_while_held_leaves_the_queue() -> None:
    """Held in dev mode, then superseded by a head with no firmware change: skipped and
    promoted (no hardware), and nothing of this runner waits any more."""
    g, srv = GitHub(), Server([HAT], {})
    dev_mode(srv)
    staging.run_once(g, srv)
    g.changed = ["docs/x.md"]
    state = staging.run_once(g, srv)
    assert state["reported"] == SHA and srv.queue[-1] == [] and not srv.submitted


def test_a_suite_preempted_by_dev_mode_is_queued_again_not_failed() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "cancelled", "error": None})
    staging.run_once(g, srv)  # submitted
    dev_mode(srv)
    srv.mode["preempted"] = [{"job": "job-1", "sha": SHA, "at": 1_700_000_100.0}]
    state = staging.run_once(g, srv)
    assert "running" not in state and "reported" not in state
    assert state["queued"]["since"] == 1_700_000_100.0
    assert srv.queue[-1][0]["preempted"] is True
    assert all(st != "failure" for st, _ in g.statuses) and not g.comments
    assert g.statuses[-1][1].startswith("queued: box in dev mode since")
    # Back in queue mode, the same head is tested again.
    srv.mode = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
    srv.job = {"status": "running", "progressMessage": "health"}
    state = staging.run_once(g, srv)
    assert len(srv.submitted) == 2 and state["running"]["sha"] == SHA


def test_a_cancel_that_was_not_a_preempt_is_still_a_failure() -> None:
    g, srv = GitHub(), Server([HAT], {"status": "cancelled", "error": None})
    staging.run_once(g, srv)
    state = staging.run_once(g, srv)
    assert state["reported"] == SHA and g.statuses[-1][0] == "failure"


def test_a_server_without_modes_is_queue_mode() -> None:
    g, srv = GitHub(), Server([HAT], {})
    srv.mode = urllib.error.HTTPError("u", 404, "not found", None, None)  # type: ignore[assignment,arg-type]
    staging.run_once(g, srv)
    assert len(srv.submitted) == 1


def test_an_unreadable_mode_is_a_retry_never_a_verdict() -> None:
    g, srv = GitHub(), Server([HAT], {})
    srv.mode = urllib.error.HTTPError("u", 500, "boom", None, None)  # type: ignore[assignment,arg-type]
    state = staging.run_once(g, srv)
    assert not srv.submitted and "reported" not in state and not g.statuses


def test_a_cancelled_suite_with_an_unreadable_mode_is_a_retry_not_a_failure() -> None:
    """Review: preempted or cancelled by hand can't be told apart without the mode."""
    g, srv = GitHub(), Server([HAT], {"status": "cancelled", "error": None})
    staging.run_once(g, srv)  # submitted
    srv.mode = urllib.error.HTTPError("u", 503, "draining", None, None)  # type: ignore[assignment,arg-type]
    state = staging.run_once(g, srv)
    assert state["running"]["job"] == "job-1" and "reported" not in state
    assert all(st != "failure" for st, _ in g.statuses) and not g.comments


def test_the_box_url_comes_from_the_environment_first(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("BLINKY_TESTBOX_URL", "http://other.example.invalid:1")
    assert staging.host_setting("BLINKY_TESTBOX_URL") == "http://other.example.invalid:1"


@pytest.mark.parametrize("content", [None, "", "BLINKY_TESTBOX_URL=http://<test-box>:8420\n"])
def test_a_missing_box_url_stops_the_runner_naming_file_and_key(
    hosts_env: Path, content: str | None, capsys: pytest.CaptureFixture[str]
) -> None:
    # No file, no key, or the template's placeholder: no default host, and no suite started.
    if content is None:
        hosts_env.unlink()
    else:
        hosts_env.write_text(content)
    with pytest.raises(RuntimeError, match=r"BLINKY_TESTBOX_URL.*hosts\.env"):
        staging.host_setting("BLINKY_TESTBOX_URL")
    assert staging.main([]) == 2
    out = capsys.readouterr().out
    assert "BLINKY_TESTBOX_URL" in out and str(hosts_env) in out
