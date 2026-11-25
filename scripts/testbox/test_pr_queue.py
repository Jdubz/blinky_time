"""The test box's PR hardware queue (pr_queue.py, docs/DEV_LOOP.md "PR hardware queue")."""

from __future__ import annotations

import base64
import importlib.util
import json
import sys
import time
import urllib.error
from pathlib import Path
from typing import Any

import pytest

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
_spec = importlib.util.spec_from_file_location("pr_queue", HERE / "pr_queue.py")
assert _spec and _spec.loader
prq = importlib.util.module_from_spec(_spec)
sys.modules["pr_queue"] = prq
_spec.loader.exec_module(prq)
dev = sys.modules["dev"]

REPO = "Jdubz/blinky-firmware"
A1, A2, B1 = "a" * 40, "b" * 40, "c" * 40
GRID = {"id": "87DE9B58", "state": "connected", "platform": "nrf52840", "device_type": "grid"}


def pr(number: int, sha: str, draft: bool = False, repo: str | None = REPO) -> dict[str, Any]:
    return {
        "number": number,
        "draft": draft,
        "title": f"PR {number}",
        "sha": sha,
        "ref": "x",
        "repo": repo,
    }


class GitHub:
    """gh as a fake: the open PRs to staging, each head's changed files, statuses and comments."""

    def __init__(self, prs: list[dict[str, Any]], changed: list[str] | None = None) -> None:
        self.prs = prs
        self.changed = ["blinky-things/audio/AudioTracker.cpp"] if changed is None else changed
        self.statuses: list[tuple[str, str, str, str]] = []  # (sha, state, description, context)
        self.comments: dict[str, list[dict[str, Any]]] = {}
        self.next_comment = 100
        self.fail_list = False

    def __call__(self, *args: str, stdin: str | None = None) -> str:
        a = " ".join(args)
        if "pulls?base=staging&state=open" in a:
            if self.fail_list:
                raise RuntimeError("gh api -> 1: HTTP 502")
            return "".join(json.dumps(p) + "\n" for p in self.prs)
        if f"contents/{prq.firmware_change.__globals__['FIRMWARE_PATHS']}?ref=" in a:
            raw = json.dumps({"paths": ["blinky-things/"], "unfingerprinted": []}).encode()
            return json.dumps({"content": base64.b64encode(raw).decode()})
        if f"repos/{REPO}/compare/staging..." in a:
            return json.dumps(
                {
                    "merge_base_commit": {"sha": "m" * 40},
                    "files": [{"filename": f} for f in self.changed],
                }
            )
        if f"repos/{REPO}/statuses/" in a:
            st = dict(x.split("=", 1) for x in args if "=" in x and not x.startswith("repos"))
            sha = a.split("/statuses/", 1)[1].split()[0]
            self.statuses.append((sha, st["state"], st["description"], st["context"]))
            return "{}"
        if args[:2] == ("api", "--paginate") and "/comments?per_page=100" in a:
            n = a.split("/issues/", 1)[1].split("/", 1)[0]
            return "".join(
                json.dumps({"id": c["id"], "url": c["url"], "first": c["body"].split("\n")[0]})
                + "\n"
                for c in self.comments.get(n, [])
            )
        if "--input" in args and stdin is not None:
            body = json.loads(stdin)["body"]
            if "-X PATCH" in a:
                cid = int(a.split("/issues/comments/", 1)[1].split()[0])
                for cs in self.comments.values():
                    for c in cs:
                        if c["id"] == cid:
                            c["body"] = body
                            return json.dumps({"html_url": c["url"]})
                raise AssertionError(cid)
            n = a.split("/issues/", 1)[1].split("/", 1)[0]
            self.next_comment += 1
            c = {
                "id": self.next_comment,
                "body": body,
                "url": f"https://x/pull/{n}#c{self.next_comment}",
            }
            self.comments.setdefault(n, []).append(c)
            return json.dumps({"html_url": c["url"]})
        raise AssertionError(args)

    def on(self, sha: str) -> list[tuple[str, str]]:
        return [(s, d) for h, s, d, c in self.statuses if h == sha and c == prq.CONTEXT]


def http_error(code: int, detail: str = "nope") -> urllib.error.HTTPError:
    import io

    return urllib.error.HTTPError(
        "u", code, "x", {}, io.BytesIO(json.dumps({"detail": detail}).encode())
    )  # type: ignore[arg-type]


class Server:
    def __init__(self) -> None:
        self.devices = [GRID]
        self.mode: dict[str, Any] = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
        self.jobs: dict[str, dict[str, Any]] = {}
        self.submitted: list[dict[str, Any]] = []
        self.cancelled: list[str] = []
        self.queue: list[list[dict[str, Any]]] = []
        self.refuse: Exception | None = None
        self.cancel_error: Exception | None = None

    def __call__(self, method: str, path: str, body: Any = None) -> Any:
        if path == "/api/devices":
            return self.devices
        if method == "GET" and path == "/api/box/mode":
            return self.mode
        if method == "PUT" and path == "/api/box/queue/pr":
            self.queue.append(body["waiting"])
            return {}
        if method == "POST" and path == "/api/test/pr-smoke":
            if self.refuse:
                raise self.refuse
            self.submitted.append(body)
            job = f"job-{len(self.submitted)}"
            self.jobs[job] = {"status": "running", "progressMessage": "health"}
            return {"job_id": job}
        if method == "DELETE" and path.startswith("/api/test/pr-smoke/"):
            if self.cancel_error:
                raise self.cancel_error
            job = path.rsplit("/", 1)[1]
            self.cancelled.append(job)
            self.jobs[job]["status"] = "cancelled"
            return {}
        if method == "GET" and path.startswith("/api/test/jobs/"):
            return self.jobs[path.rsplit("/", 1)[1]]
        raise AssertionError((method, path))


class Builds:
    def __init__(self) -> None:
        self.state: dict[str, str] = {}  # sha -> state the service reports (default done)
        self.submitted: list[str] = []
        self.untrusted = False

    def submit_sha(self, sha: str) -> dict[str, Any]:
        if self.untrusted:
            raise dev.Untrusted("the build service refused the commit (403: not on a branch)")
        self.submitted.append(sha)
        return self.get(f"build-{sha[:4]}")

    def get(self, build_id: str) -> dict[str, Any]:
        sha = next(s for s in self.submitted if build_id == f"build-{s[:4]}")
        st = self.state.get(sha, "done")
        rec = {"id": build_id, "state": st, "sha": sha}
        if st == "failed":
            rec |= {"error": "compile failed", "log_tail": "error: 'foo' was not declared\n```x"}
        return rec


@pytest.fixture(autouse=True)
def state_file(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setattr(prq, "STATE", tmp_path / "pr-queue.json")
    return tmp_path / "pr-queue.json"


def run(g: GitHub, srv: Server, b: Builds) -> dict[str, Any]:
    return prq.run_once(g, srv, lambda: b)


GOOD = {
    "sha": A1,
    "passed": True,
    "version": "b230-dev-aaaaaaaa",
    "build_id": "build-aaaa",
    "devices": [GRID["id"]],
    "limits": {"stable_minutes": 2.0},
    "corpus": "f" * 40,
    "deploy": {"pass": True},
    "health": {GRID["id"]: {"fps": {"pass": True, "min": 120.0}}},
    "info": {GRID["id"]: {"version": "b230-dev-aaaaaaaa", "timers": {"heapPeak": 9, "acfUs": 700}}},
    "validation": {
        "kind": "validate",
        "tracks": 3,
        "played": 2,
        "results": [
            {
                "track": "techno-machine-drum",
                "scores": {GRID["id"]: {"drums": {"drumF": 0.71, "kickR50": 0.9}}},
            },
            {"track": "dnb-energetic-breakbeat", "scores": {GRID["id"]: {"drums": {"drumF": 0.1}}}},
            {"track": "garage-uk-2step", "failed": True},
        ],
    },
    "final": {
        GRID["id"]: {
            "version": {"pass": True},
            "no_reboot": {"pass": True, "uptime_s": 300},
            "alerts": {"pass": True, "lines": []},
        }
    },
}


# -- by kind -----------------------------------------------------------------------------------


def test_a_draft_is_ignored() -> None:
    g, srv, b = GitHub([pr(5, A1, draft=True)]), Server(), Builds()
    state = run(g, srv, b)
    assert g.statuses == [] and b.submitted == [] and srv.submitted == [] and state["queue"] == {}
    assert srv.queue[-1] == []


def test_a_fork_pr_is_never_built_or_tested_and_never_blocked() -> None:
    for repo in ("someone/blinky-firmware", None):  # a deleted fork has no head repo
        g, srv, b = GitHub([pr(5, A1, repo=repo)]), Server(), Builds()
        run(g, srv, b)
        run(g, srv, b)
        assert g.on(A1) == [("success", prq.FORK)]  # once
        assert b.submitted == [] and srv.submitted == []
        prq.STATE.unlink()


def test_no_firmware_change_skips_the_hardware_test() -> None:
    g, srv, b = GitHub([pr(5, A1)], changed=["docs/README.md"]), Server(), Builds()
    run(g, srv, b)
    run(g, srv, b)
    assert g.on(A1) == [("success", "no firmware change: hardware test skipped")]
    assert b.submitted == [] and srv.submitted == []


def test_a_firmware_change_is_built_by_sha_and_smoke_tested() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    state = run(g, srv, b)
    assert b.submitted == [A1]
    assert srv.submitted == [
        {"sha": A1, "build_id": "build-aaaa", "device_ids": [GRID["id"]], "pr": 5}
    ]
    assert state["running"]["job"] == "job-1" and state["queue"] == {}
    assert g.on(A1) == [
        ("pending", "built; queued for the box"),
        ("pending", "testing on 1 test device(s)"),
    ]
    assert srv.queue[-1] == []  # running is not waiting
    run(g, srv, b)  # still running: nothing new
    assert len(srv.submitted) == 1 and len(g.on(A1)) == 2


def test_a_building_head_waits_with_one_pending_status() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    b.state[A1] = "building"
    run(g, srv, b)
    state = run(g, srv, b)
    assert g.on(A1) == [("pending", "building on the build service")]
    assert b.submitted == [A1] and srv.submitted == []  # polled by id, not submitted again
    assert srv.queue[-1] == [{"sha": A1, "since": state["queue"]["5"]["since"], "what": "PR #5"}]
    b.state[A1] = "done"
    assert run(g, srv, b)["running"]["sha"] == A1


def test_a_failed_build_fails_the_head_with_the_log_tail() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    b.state[A1] = "failed"
    state = run(g, srv, b)
    assert g.on(A1)[-1] == ("failure", "the build failed: see the log in the comment")
    (c,) = g.comments["5"]
    assert c["body"].startswith(f"## Hardware test (PR) — {A1}\n") and "BUILD FAILED" in c["body"]
    assert "'foo' was not declared" in c["body"] and "```x" not in c["body"]  # fence kept intact
    assert state["reported"] == {"5": A1} and srv.submitted == []
    run(g, srv, b)
    assert len(g.comments["5"]) == 1 and g.on(A1)[-1][0] == "failure" and len(b.submitted) == 1


def test_a_refused_build_is_retried_then_a_failure() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    b.untrusted = True
    for _ in range(prq.REFUSED_MAX - 1):
        assert "5" in run(g, srv, b)["queue"]
    state = run(g, srv, b)
    assert g.on(A1)[-1][0] == "failure" and "refused" in g.on(A1)[-1][1]
    assert state["reported"] == {"5": A1} and srv.submitted == []


# -- the verdict -------------------------------------------------------------------------------


def test_a_pass_posts_success_and_one_comment_per_head() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    run(g, srv, b)
    srv.jobs["job-1"] = {"status": "complete", "result": GOOD}
    state = run(g, srv, b)
    assert g.on(A1)[-1][0] == "success" and "deploy, health, asserts" in g.on(A1)[-1][1]
    (c,) = g.comments["5"]
    body = c["body"]
    assert body.startswith(f"## Hardware test (PR) — {A1}\n") and "**PASSED**" in body
    assert "reported, not gated (corpus `ffffffffffff`)" in body
    assert "| techno-machine-drum | 87DE9B58 | 0.9 | - | - | 0.71 | - |" in body
    assert "| garage-uk-2step | | not played |" in body
    assert "Info timers (87DE9B58): acfUs=700, heapPeak=9" in body
    assert "| 87DE9B58 | no_reboot | ✅ |" in body
    assert state["reported"] == {"5": A1} and "running" not in state
    run(g, srv, b)  # settled: nothing more
    assert len(g.comments["5"]) == 1 and srv.submitted == srv.submitted[:1]


def test_a_rerun_of_the_same_head_updates_its_comment() -> None:
    g = GitHub([pr(5, A1)])
    g.comments["5"] = [
        {"id": 7, "body": "## Claude review — x\n\nfine", "url": "u7"},
        {"id": 8, "body": f"## Hardware test (PR) — {A1}\n\nold", "url": "u8"},
    ]
    assert prq.upsert_comment(g, "5", A1, f"## Hardware test (PR) — {A1}\n\nnew") == "u8"
    assert [c["body"].split("\n")[-1] for c in g.comments["5"]] == ["fine", "new"]


def test_a_health_or_assert_failure_fails_the_head_and_scores_never_do() -> None:
    bad = {
        **GOOD,
        "passed": False,
        "final": {GRID["id"]: {"alerts": {"pass": False, "lines": ["[ASSERT] x"]}}},
    }
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    run(g, srv, b)
    srv.jobs["job-1"] = {"status": "complete", "result": bad}
    run(g, srv, b)
    assert g.on(A1)[-1] == ("failure", "FAILED: see the report")
    assert "[ASSERT] x" in g.comments["5"][0]["body"]


@pytest.mark.parametrize(
    "job", [{"status": "error", "error": "boom"}, {"status": "cancelled", "error": None}]
)
def test_a_crashed_or_cancelled_job_is_a_failure(job: dict[str, Any]) -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    run(g, srv, b)
    srv.jobs["job-1"] = job
    run(g, srv, b)
    assert (
        g.on(A1)[-1][0] == "failure"
        and "Deploy failed at `smoke test`" in g.comments["5"][0]["body"]
    )


def test_a_flash_failure_names_each_devices_reason() -> None:
    flash = {"per_device": {"87DE9B58C90A": {"flash": "error", "message": "verify timed out"}}}
    dep = {
        "pass": False,
        "failed_step": "flash",
        "message": "flash: 0/1",
        "steps": {"flash": {"status": "error", "detail": flash}},
    }
    text = prq.render(
        {
            "sha": A1,
            "passed": False,
            "version": "b211-dev-aaaaaaaa",
            "devices": ["X"],
            "deploy": dep,
        }
    )
    assert "- 87DE9B58C90A: verify timed out" in text and "`b211-dev-aaaaaaaa`" in text


def test_a_deploy_failure_renders_its_step() -> None:
    text = prq.render(
        {
            "sha": A1,
            "passed": False,
            "devices": ["X"],
            "deploy": {"pass": False, "failed_step": "verify", "message": "version"},
        }
    )
    assert "Deploy failed at `verify`: version" in text and "**FAILED**" in text


# -- dev mode, supersedes, closes, preempts ----------------------------------------------------


def test_dev_mode_submits_nothing_and_says_why_once() -> None:
    g, srv, b = GitHub([pr(5, A1), pr(6, B1)]), Server(), Builds()
    since = time.mktime(time.strptime("2026-11-25 14:05", "%Y-%m-%d %H:%M"))
    srv.mode = {
        "mode": "dev",
        "since": since,
        "waiting": [{"runner": "staging", "sha": A2, "since": 1.0}],
        "preempted": [],
    }
    run(g, srv, b)
    state = run(g, srv, b)
    want = "queued: box in dev mode since 14:05 (3 waiting)"
    assert g.on(A1) == [("pending", want)] and g.on(B1) == [("pending", want)]
    assert b.submitted == [] and srv.submitted == []
    assert [(w["sha"], w["what"]) for w in srv.queue[-1]] == [(A1, "PR #5"), (B1, "PR #6")]
    assert srv.queue[-1][0]["since"] == state["queue"]["5"]["since"]
    srv.mode = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
    state = run(g, srv, b)
    assert state["running"]["pr"] == "5" and [w["sha"] for w in srv.queue[-1]] == [B1]


def test_a_superseded_head_drops_out_and_its_successor_keeps_its_place() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.mode = {"mode": "dev", "since": time.time(), "waiting": [], "preempted": []}
    first = run(g, srv, b)["queue"]["5"]["since"]
    g.prs = [pr(5, A2)]
    state = run(g, srv, b)
    assert g.on(A1)[-1] == ("success", f"superseded by {A2[:8]} before its hardware test")
    assert state["queue"]["5"]["sha"] == A2 and state["queue"]["5"]["since"] == first


def test_a_closed_pr_is_dropped_and_nothing_posted() -> None:
    g, srv, b = GitHub([pr(5, A1), pr(6, B1)]), Server(), Builds()
    srv.mode = {"mode": "dev", "since": time.time(), "waiting": [], "preempted": []}
    run(g, srv, b)
    posted = len(g.on(A1))
    g.prs = [pr(6, B1)]
    state = run(g, srv, b)
    assert list(state["queue"]) == ["6"] and len(g.on(A1)) == posted  # nothing more on #5
    assert g.on(B1)[-1][1].endswith("(1 waiting)")  # the other's count follows
    srv.mode = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
    run(g, srv, b)  # B1 runs
    g.prs = []
    state = run(g, srv, b)
    assert srv.cancelled == ["job-1"] and "running" not in state
    assert g.on(B1)[-1] == ("pending", "testing on 1 test device(s)")  # nothing after
    assert state["queue"] == {} and state["reported"] == {}


def test_a_running_test_whose_cancel_fails_is_kept_for_the_next_pass() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    run(g, srv, b)
    g.prs, srv.cancel_error = [], urllib.error.URLError("down")
    assert run(g, srv, b)["running"]["job"] == "job-1"
    srv.cancel_error = None
    assert "running" not in run(g, srv, b) and srv.cancelled == ["job-1"]


def test_a_new_head_cancels_the_running_test_and_is_tested_instead() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    first = run(g, srv, b)["running"]["since"]
    g.prs = [pr(5, A2)]
    state = run(g, srv, b)
    assert srv.cancelled == ["job-1"]
    assert g.on(A1)[-1] == ("success", f"superseded by {A2[:8]} before its hardware test")
    assert state["running"]["sha"] == A2 and state["running"]["since"] == first
    assert srv.submitted[-1]["sha"] == A2


def test_a_preempted_test_is_queued_again_not_failed() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    run(g, srv, b)
    srv.jobs["job-1"] = {"status": "cancelled"}
    srv.mode = {
        "mode": "dev",
        "since": time.time(),
        "waiting": [],
        "preempted": [{"job": "job-1", "sha": A1, "at": 1_700_000_100.0}],
    }
    state = run(g, srv, b)
    q = state["queue"]["5"]
    assert q["sha"] == A1 and q["preempted"] and q["since"] == 1_700_000_100.0 and q["ready"]
    assert not any(s == "failure" for s, _ in g.on(A1)) and g.comments == {}
    assert g.on(A1)[-1][1].startswith("queued: box in dev mode since")
    assert srv.queue[-1][0]["preempted"] is True
    srv.mode = {"mode": "queue", "since": 0.0, "waiting": [], "preempted": []}
    state = run(g, srv, b)
    assert state["running"]["sha"] == A1 and b.submitted == [A1]  # not rebuilt


# -- order and sharing the box -----------------------------------------------------------------


def test_the_oldest_pr_goes_first() -> None:
    g, srv, b = GitHub([pr(9, B1), pr(5, A1)]), Server(), Builds()
    state = run(g, srv, b)
    assert state["running"]["pr"] == "5"
    assert srv.queue[-1] == [
        {"sha": B1, "since": state["queue"]["9"]["since"], "what": "PR #9", "ready": True}
    ]


def test_it_yields_to_the_staging_runners_older_ready_head() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    now = time.time()
    older = {"runner": "staging", "sha": A2, "since": now - 600, "ready": True, "reported_at": now}
    srv.mode["waiting"] = [older]
    state = run(g, srv, b)
    assert srv.submitted == [] and state["queue"]["5"]["ready"]
    # Not ready (still building, held), or a runner that stopped reporting: no turn to wait for.
    for w in (
        {**older, "ready": False},
        {**older, "reported_at": now - 3600},
        {k: v for k, v in older.items() if k != "reported_at"},
    ):
        srv.mode["waiting"] = [w]
        run(g, srv, b)
    assert len(srv.submitted) == 1


@pytest.mark.parametrize("code", [409, 429, 500, 502, 503])
def test_a_busy_box_is_retried(code: int) -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.refuse = http_error(code)
    state = run(g, srv, b)
    assert "5" in state["queue"] and not any(s == "failure" for s, _ in g.on(A1))
    srv.refuse = None
    assert run(g, srv, b)["running"]["sha"] == A1


def test_a_refused_smoke_test_is_a_failure_not_a_retry() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.refuse = http_error(400, "unknown tracks")
    state = run(g, srv, b)
    assert g.on(A1)[-1] == ("failure", "the server refused the smoke test (400): unknown tracks")
    assert state["reported"] == {"5": A1}


def test_no_test_device_waits() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.devices = []
    run(g, srv, b)
    run(g, srv, b)
    assert (
        g.on(A1)[-1] == ("pending", "no test device connected to the box") and srv.submitted == []
    )
    assert len(g.on(A1)) == 2


def test_github_down_is_a_retry_and_touches_nothing() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    run(g, srv, b)
    g.fail_list = True
    state = run(g, srv, b)
    assert state["running"]["job"] == "job-1" and srv.cancelled == []


def test_an_unconfigured_build_service_builds_nothing() -> None:
    g, srv = GitHub([pr(5, A1)]), Server()

    def none() -> Any:
        raise dev.DevError("no build service configured")

    state = prq.run_once(g, srv, none)
    assert "5" in state["queue"] and srv.submitted == []


def test_a_build_the_service_no_longer_has_is_built_again() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    b.state[A1] = "building"
    run(g, srv, b)
    real_get = b.get

    def gone(build_id: str) -> dict[str, Any]:
        raise dev.ServiceError("the build service answered 404: no such build", 404)

    b.get = gone  # type: ignore[method-assign]
    state = run(g, srv, b)
    assert "build" not in state["queue"]["5"]
    b.get, b.state[A1] = real_get, "done"  # type: ignore[method-assign]
    assert run(g, srv, b)["running"]["sha"] == A1 and b.submitted == [A1, A1]


def test_a_github_failure_mid_pass_still_reports_the_queue() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.mode = {"mode": "dev", "since": time.time(), "waiting": [], "preempted": []}
    real = g.__call__

    def flaky(*args: str, stdin: str | None = None) -> str:
        if "/statuses/" in " ".join(args):
            raise RuntimeError("gh api -> 1: HTTP 502")
        return real(*args, stdin=stdin)

    state = prq.run_once(flaky, srv, lambda: b)
    assert state["queue"]["5"]["sha"] == A1 and "posted" not in state["queue"]["5"]
    assert [w["sha"] for w in srv.queue[-1]] == [A1]
    run(g, srv, b)  # GitHub back: the status goes out
    assert g.on(A1)[-1][1].startswith("queued: box in dev mode since")


def test_turn_taking_compares_the_longest_wait_so_the_runners_never_deadlock() -> None:
    """PR #5 got a new head (newer wait) while PR #9 has waited long; the staging head's wait
    is between them. The staging runner yields to #9 (older than it), so this runner must not
    yield to staging: it submits (#5, the oldest PR) instead of both waiting for good."""
    g, srv, b = GitHub([pr(5, A1), pr(9, B1)]), Server(), Builds()
    srv.mode = {"mode": "dev", "since": time.time(), "waiting": [], "preempted": []}
    run(g, srv, b)
    state = json.loads(prq.STATE.read_text())
    now = time.time()
    state["queue"]["9"]["since"] = now - 900
    state["queue"]["5"]["since"] = now - 60
    prq.save_state(state)
    staging = {
        "runner": "staging",
        "sha": A2,
        "since": now - 300,
        "ready": True,
        "reported_at": now,
    }
    srv.mode = {"mode": "queue", "since": 0.0, "waiting": [staging], "preempted": []}
    assert sys.modules["staging"].older_elsewhere(
        {
            "waiting": [
                {"runner": "pr", "sha": B1, "since": now - 900, "ready": True, "reported_at": now}
            ]
        },
        "staging",
        now - 300,
    )  # the staging runner yields
    state = run(g, srv, b)
    assert state["running"]["pr"] == "5"  # and this one goes


def test_a_build_pruned_after_it_was_ready_is_built_again() -> None:
    """Ready in dev mode with an old build id the service has since pruned: the server answers
    410 for it, and the head is built again rather than retried with that id forever."""
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.refuse = http_error(410, "the build service has no build build-aaaa")
    state = run(g, srv, b)
    assert "build" not in state["queue"]["5"] and not state["queue"]["5"].get("ready")
    srv.refuse = None
    assert run(g, srv, b)["running"]["sha"] == A1 and b.submitted == [A1, A1]


def test_a_build_service_down_leaves_nothing_ready_so_staging_is_not_held() -> None:
    g, srv, b = GitHub([pr(5, A1)]), Server(), Builds()
    srv.devices = []  # built and ready, but not submitted yet
    assert run(g, srv, b)["queue"]["5"]["ready"]
    real_get = b.get

    def down(build_id: str) -> dict[str, Any]:
        raise dev.DevError("the build service isn't answering: refused")

    b.get = down  # type: ignore[method-assign]
    state = run(g, srv, b)
    assert not state["queue"]["5"].get("ready") and "ready" not in srv.queue[-1][0]
    b.get = real_get  # type: ignore[method-assign]
    srv.devices = [GRID]
    assert run(g, srv, b)["running"]["sha"] == A1 and b.submitted == [A1]  # same build
