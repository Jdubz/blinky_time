#!/usr/bin/env python3
"""The PR hardware queue on the test box (docs/DEV_LOOP.md, "PR hardware queue"): the blocking
hardware checks (blinky-server testing/hardware_checks.py) on each firmware PR's head, on the
box's test chips, before it merges into staging. A PR staging -> master must pass them, and a
red commit must not reach the history (owner, 2026-11-25): so they run here, on the PR head, and
the staging runner only confirms them for the merged commit (same tree).

blinky-pr-queue.timer runs this every 2 minutes from the deploy worktree (master code), like
staging.py, whose helpers it shares. One pass:

1. The open PRs to blinky-firmware's ``staging``, each by kind (posting on the PR's head, context
   ``Hardware test (PR)``):
   - a draft: ignored, and out of the queue;
   - a fork PR (its head repo isn't Jdubz/blinky-firmware): never built or tested on the box
     ("Trust": no untrusted code on the owner's machines). It gets ``success``, "fork PR: not
     hardware-tested before merge; ...", so a required status can't block it forever;
   - no change under the firmware's paths (tools/firmware_paths.json at the head) since its
     merge-base with staging: ``success``, "no firmware change: hardware test skipped";
   - anything else is queued: the newest head of each PR, oldest PR first. A head superseded
     while it waits drops out (its successor carries its changes, and its place); one this
     runner had posted on is marked superseded.
2. Hardware checks in flight (GET /api/test/jobs/{id}): its PR closed or merged, the job is
   cancelled (DELETE /api/test/hardware-checks/{id}) and nothing more is posted; its PR has a
   new head, it is cancelled and its head marked superseded. Finished:
   - passed: ``success`` "hardware checks passed: ...";
   - a check failed: ``failure`` naming it;
   - a rig error (the camera, the speaker, the build service, a job that crashed): no verdict.
     The head stays ``pending`` "rig error: ..., retrying" and goes back in the queue (its build
     kept), tried again on the next pass, then backing off (staging.rig_backoff) so a broken
     rig isn't flashed every pass. Never red for a rig fault.
   One report comment per tested head, headed ``## Hardware test (PR) — <sha>`` and updated in
   place: the checks table first, then the quality section (non-blocking: scores are the
   staging quality suite's, after merge). Cancelled by dev mode's --preempt (the job among the
   mode's "preempted"): not a failure, the head goes back in the queue.
3. Dev mode (or switching to it): nothing is submitted, builds included. Each queued head gets
   the pending status "queued: box in dev mode since HH:MM (N waiting)", posted when its text
   changes.
4. Queue mode: each queued head is built on the build service by sha (its head is a branch of
   the repo, so the service's trust check passes). A failed build: ``failure`` and the log's
   tail in the head's report comment. The oldest PR's built head is submitted
   (POST /api/test/hardware-checks) unless the staging runner has a ready head that has waited
   longer (staging.older_elsewhere), or the box is busy (409: next pass).
5. The queue is reported to the server (PUT /api/box/queue/pr), so dev.py status,
   GET /api/box/mode and the metrics show what waits, and the staging runner can take turns.

``--retest PR`` queues the PR's current head again (a rig flake that ended in a verdict).
State: ~/.local/share/blinky-server/pr-queue.json; log: ``journalctl -u blinky-pr-queue``;
``--status`` prints the state. Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
from collections.abc import Callable
from pathlib import Path
from typing import Any, Protocol

from dev import BuildService, DevError, Untrusted, build_config
from staging import (
    GH_REPO,
    RETRY_CODES,
    TERMINAL,
    Gh,
    _detail,
    box_mode,
    firmware_change,
    gh,
    holds,
    http,
    hw_lines,
    hw_status,
    log,
    older_elsewhere,
    post_status,
    preempted,
    put_queue,
    queued_description,
    rig_backoff,
    test_devices,
)
from staging import upsert_comment as _upsert

BASE = "staging"  # the branch the queue's PRs merge into
CONTEXT = "Hardware test (PR)"
RUNNER = "pr"  # this runner's name in the box's hardware queue (PUT /api/box/queue/pr)
REPORT_HEADER = "## Hardware test (PR) — "
HOME = Path.home()
STATE = (
    Path(os.environ.get("XDG_DATA_HOME") or HOME / ".local/share")
    / "blinky-server"
    / "pr-queue.json"
)
FORK = "fork PR: not hardware-tested before merge; the staging suite tests it after merge"
NO_CHANGE = "no firmware change: hardware test skipped"
# A 403 from the build service for a head that is on a branch here is most likely its mirror
# fetch failing for a moment: tried this many passes before it is a failure.
REFUSED_MAX = 3
LOG_TAIL_CHARS = 6000
QUALITY_NOTE = (
    "**Quality (non-blocking):** not run on PR heads. After merge, the staging quality suite "
    "(every edm/ track, beat pace, scores with the corpus pin) reports on the staging commit; "
    "it never blocks."
)

Server = Callable[..., Any]
# Why the last pass couldn't finish (GitHub unreachable or failing), or None: main() then exits
# 1, so the unit fails and the LGTM alert BlinkyBoxRunnerFailing sees a runner whose every pass
# fails (it keys on the unit never going back to inactive), not a string of quiet exits.
PASS_ERROR: str | None = None


class Builds(Protocol):
    def submit_sha(self, sha: str) -> dict[str, Any]: ...
    def get(self, build_id: str) -> dict[str, Any]: ...


# -- GitHub ------------------------------------------------------------------------------------


def open_prs(gh_: Gh) -> dict[str, dict[str, Any]]:
    """The open PRs to staging by number (as a string, the state file's keys)."""
    raw = gh_(
        "api",
        "--paginate",
        f"repos/{GH_REPO}/pulls?base={BASE}&state=open&per_page=100",
        "--jq",
        ".[] | {number, draft, title, sha: .head.sha, ref: .head.ref,"
        " repo: (.head.repo.full_name // null)}",
    )
    prs = [json.loads(ln) for ln in raw.splitlines() if ln.strip()]
    return {str(p["number"]): p for p in prs}


def is_fork(pr: dict[str, Any]) -> bool:
    """Its head isn't a branch of the repo itself (a deleted fork has no repo at all)."""
    return pr.get("repo") != GH_REPO


def status(gh_: Gh, sha: str, state: str, description: str, url: str | None = None) -> None:
    post_status(gh_, sha, state, description, url, context=CONTEXT)


def upsert_comment(gh_: Gh, number: str, sha: str, body: str) -> str | None:
    """The head's report comment: the one starting with its header is updated, else one is
    added. Returns its URL."""
    return _upsert(gh_, number, f"{REPORT_HEADER}{sha}", body)


# -- the report --------------------------------------------------------------------------------


def render(report: dict[str, Any]) -> str:
    """The hardware checks' report as markdown: the checks table first, then the quality note
    (non-blocking)."""
    lines = [f"{REPORT_HEADER}{report.get('sha', '?')}", ""]
    if report.get("build_id"):
        lines += [f"Build `{report['build_id']}` from the build service.", ""]
    lines += hw_lines(report)
    lines += ["", QUALITY_NOTE]
    return "\n".join(lines) + "\n"


def render_build_failure(sha: str, build: dict[str, Any]) -> str:
    tail = str(build.get("log_tail") or build.get("error") or "no log").replace("```", "'''")
    return (
        f"{REPORT_HEADER}{sha}\n\n**BUILD FAILED** on the build service (build "
        f"`{build.get('id')}`): {build.get('error') or 'see the log'}\n\nThe log's tail:\n\n"
        f"```\n{tail[-LOG_TAIL_CHARS:]}\n```\n"
    )


# -- state -------------------------------------------------------------------------------------


def load_state() -> dict[str, Any]:
    if STATE.exists():
        state: dict[str, Any] = json.loads(STATE.read_text())
        return state
    return {}


def save_state(state: dict[str, Any]) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n")
    tmp.replace(STATE)


def post_once(gh_: Gh, item: dict[str, Any], state: str, desc: str) -> None:
    """A status on a queued head, only when its text changes."""
    if item.get("posted") != desc:
        status(gh_, item["sha"], state, desc)
        item["posted"] = desc
        log(f"{item['sha'][:12]}: {desc}")


def superseded(gh_: Gh, old: str, new: str) -> None:
    """A head this runner posted on, replaced before its verdict: said so, not left pending.
    A success like the fork and skip statuses: it blocks nothing and says what happened."""
    status(gh_, old, "success", f"superseded by {new[:8]} before its hardware test")
    log(f"{old[:12]}: superseded by {new[:12]}")


def settled(state: dict[str, Any], number: str, sha: str) -> None:
    state.setdefault("reported", {})[number] = sha
    state.setdefault("queue", {}).pop(number, None)


# -- one pass ----------------------------------------------------------------------------------


def cancel(server: Server, job: str) -> bool:
    """Cancel our hardware checks: True once they are cancelled or gone."""
    try:
        server("DELETE", f"/api/test/hardware-checks/{job}")
    except urllib.error.HTTPError as e:
        if e.code != 404:
            log(f"cancelling {job}: server answered {e.code}; next pass")
            return False
    except (urllib.error.URLError, OSError) as e:
        log(f"cancelling {job}: server not answering ({e}); next pass")
        return False
    return True


def finish(gh_: Gh, state: dict[str, Any], job: dict[str, Any]) -> None:
    run = state["running"]
    number, sha = run["pr"], run["sha"]
    report = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(report, dict) or "passed" not in report:
        # The job itself crashed (or was cancelled by hand): no verdict on the firmware.
        report = {
            "sha": sha,
            "passed": False,
            "devices": run["devices"],
            "rig_error": f"the hardware-checks job ended {job.get('status')}: "
            f"{job.get('error') or 'no result'}",
        }
    report.setdefault("build_id", run.get("build"))
    url = upsert_comment(gh_, number, sha, render(report))
    st, desc = hw_status(report)
    state.pop("running", None)
    if report.get("rig_error"):
        n = int(run.get("rig_errors") or 0) + 1
        retry = time.time() + rig_backoff(n) - 60  # the next pass first (passes are 2 min)
        state.setdefault("queue", {})[number] = {
            "sha": sha,
            "since": run["since"],
            "build": run.get("build"),
            "ready": bool(run.get("build")),
            "rig_errors": n,
            "retry_at": retry,
            "posted": desc,
        }
        status(gh_, sha, st, desc, url)
        # "rig error N in a row" is matched by the LGTM alert BlinkyHardwareCheckRigError (N >= 6).
        log(f"{sha[:12]} (#{number}): {desc} (rig error {n} in a row)")
        return
    status(gh_, sha, st, desc, url)
    log(f"{sha[:12]} (#{number}): {desc} ({url})")
    settled(state, number, sha)


def poll_running(
    gh_: Gh, server: Server, state: dict[str, Any], prs: dict[str, dict[str, Any]]
) -> None:
    run = state["running"]
    number, sha = run["pr"], run["sha"]
    pr = prs.get(number)
    if pr is None or pr["sha"] != sha:
        # Closed or merged: dropped, nothing more posted. A new head: this one is moot.
        if not cancel(server, run["job"]):
            return
        if pr is None:
            log(
                f"{sha[:12]}: PR #{number} closed or merged; hardware checks {run['job']} cancelled"
            )
        else:
            superseded(gh_, sha, pr["sha"])
            # The new head is judged (fork? firmware change?) like any other, this pass, and
            # keeps the place this one had.
            state.setdefault("inherit", {})[number] = run["since"]
        state.pop("running")
        return
    try:
        job = server("GET", f"/api/test/jobs/{run['job']}")
    except urllib.error.HTTPError as e:
        if e.code != 404:
            log(f"server answered {e.code}; next pass")
            return
        job = {"status": "error", "error": f"hardware-checks job {run['job']} not found"}
    except (urllib.error.URLError, OSError) as e:
        log(f"server not answering ({e}); next pass")
        return
    if job.get("status") not in TERMINAL:
        log(f"{sha[:12]} (#{number}): {job.get('progressMessage')}")
        return
    if job["status"] == "cancelled":
        try:
            pre = preempted(server, run["job"])
        except (urllib.error.URLError, OSError) as e:  # HTTPError included
            log(f"{sha[:12]}: hardware checks cancelled, box mode unreadable ({e}); next pass")
            return
        if pre is not None:
            state.pop("running")
            state.setdefault("queue", {})[number] = {
                "sha": sha,
                "since": float(pre.get("at") or time.time()),
                "build": run.get("build"),
                "ready": bool(run.get("build")),
                "preempted": True,
            }
            log(f"{sha[:12]} (#{number}): hardware checks preempted by dev mode; queued again")
            return
    finish(gh_, state, job)


def judge(gh_: Gh, state: dict[str, Any], number: str, pr: dict[str, Any]) -> None:
    """Settle or queue one open PR's head."""
    queue, reported = state.setdefault("queue", {}), state.setdefault("reported", {})
    sha = pr["sha"]
    since = (state.get("inherit") or {}).pop(number, None)
    item = queue.get(number)
    if item and item["sha"] != sha:
        if item.get("posted"):
            superseded(gh_, item["sha"], sha)
        since = item["since"]  # the successor keeps its place
        del queue[number]
    run = state.get("running") or {}
    if pr.get("draft") or reported.get(number) == sha or number in queue:
        return
    if run.get("pr") == number and run.get("sha") == sha:
        return
    if is_fork(pr):
        status(gh_, sha, "success", FORK)
        log(f"{sha[:12]} (#{number}): {FORK}")
        settled(state, number, sha)
        return
    changed, why, _ = firmware_change(gh_, sha, base=BASE)
    if not changed:
        status(gh_, sha, "success", NO_CHANGE)
        log(f"{sha[:12]} (#{number}): {why}")
        settled(state, number, sha)
        return
    queue[number] = {"sha": sha, "since": float(since) if since else time.time()}
    log(f"{sha[:12]} (#{number}): {why}: queued")


def hold(gh_: Gh, state: dict[str, Any], mode: dict[str, Any]) -> None:
    """Dev mode: every queued head pending with the reason, nothing submitted."""
    queue = state.get("queue") or {}
    others = [w for w in mode.get("waiting") or [] if w.get("runner") != RUNNER]
    desc = queued_description(mode, len(others) + len(queue))
    for item in queue.values():
        post_once(gh_, item, "pending", desc)


def build(gh_: Gh, state: dict[str, Any], builds: Builds) -> None:
    """Queue mode: each queued head built (by sha) or its build polled, a built one too, so
    ``ready`` always means a build the service has now; a failed build settles the head with a
    failure and the log's tail. A service that can't be reached leaves nothing ready: a head
    that can't be tested must not hold the staging runner back (it yields to ready items)."""
    queue = state.get("queue") or {}
    for number in sorted(queue, key=int):
        item = queue[number]
        try:
            rec = builds.get(item["build"]) if item.get("build") else builds.submit_sha(item["sha"])
        except Untrusted as e:
            item["refused"] = int(item.get("refused") or 0) + 1
            log(f"{item['sha'][:12]} (#{number}): {e} ({item['refused']}/{REFUSED_MAX})")
            if item["refused"] >= REFUSED_MAX:
                status(gh_, item["sha"], "failure", f"the build service refused the commit: {e}")
                settled(state, number, item["sha"])
            continue
        except DevError as e:
            if getattr(e, "code", None) == 404 and item.get("build"):
                # The service no longer has the build (pruned, or its store was reset): build
                # the head again.
                log(f"{item['sha'][:12]} (#{number}): build {item.pop('build')} gone; rebuilding")
                item.pop("ready", None)
                continue
            log(f"build service: {e}; next pass")
            for held in queue.values():
                held.pop("ready", None)
            return
        item["build"] = rec.get("id") or item.get("build")
        st = rec.get("state")
        if st == "done":
            if not item.get("ready"):  # said once, not over a later "no test device"
                post_once(gh_, item, "pending", "built; queued for the box")
            item["ready"] = True
        elif st == "failed":
            url = upsert_comment(gh_, number, item["sha"], render_build_failure(item["sha"], rec))
            status(gh_, item["sha"], "failure", "the build failed: see the log in the comment", url)
            log(f"{item['sha'][:12]} (#{number}): build {rec.get('id')} failed")
            settled(state, number, item["sha"])
        else:
            post_once(gh_, item, "pending", "building on the build service")


def submit(gh_: Gh, server: Server, state: dict[str, Any], mode: dict[str, Any]) -> None:
    """The oldest PR's built head to the box, unless another runner's older item goes first."""
    queue = state.get("queue") or {}
    now = time.time()
    ready = [
        n
        for n in sorted(queue, key=int)
        if queue[n].get("ready") and float(queue[n].get("retry_at") or 0) <= now
    ]
    if not ready or "running" in state:
        return
    number = ready[0]
    item = queue[number]
    # Turn-taking compares this runner's longest wait, not the PR it picks: the staging runner
    # yields to any older ready PR head, so judging by one PR's (possibly newer) wait could
    # have both yield to each other for good.
    oldest = min(float(queue[n]["since"]) for n in ready)
    ahead = older_elsewhere(mode, RUNNER, oldest)
    if ahead:
        other = f"{ahead.get('runner')} {str(ahead.get('sha'))[:12]}"
        log(f"{item['sha'][:12]} (#{number}): {other} has waited longer")
        return
    try:
        devices = test_devices(server("GET", "/api/devices"))
        if not devices:
            post_once(gh_, item, "pending", "no test device connected to the box")
            return
        job = server(
            "POST",
            "/api/test/hardware-checks",
            {
                "sha": item["sha"],
                "build_id": item["build"],
                "device_ids": devices,
                "pr": int(number),
                "source": "dev_build",
            },
        )
    except urllib.error.HTTPError as e:
        if e.code == 410:  # the build service no longer has the build: build the head again
            log(f"{item['sha'][:12]} (#{number}): build {item.pop('build')} gone; rebuilding")
            item.pop("ready", None)
            return
        if not (400 <= e.code < 500) or e.code in RETRY_CODES:
            log(f"{item['sha'][:12]} (#{number}): server answered {e.code}; next pass")
            return
        detail = _detail(e)
        status(
            gh_,
            item["sha"],
            "failure",
            f"the server refused the hardware checks ({e.code}): {detail}",
        )
        settled(state, number, item["sha"])
        log(f"{item['sha'][:12]} (#{number}): hardware checks refused ({e.code}): {detail}")
        return
    except (urllib.error.URLError, OSError) as e:
        log(f"server not answering ({e}); next pass")
        return
    state["running"] = {
        "pr": number,
        "sha": item["sha"],
        "job": job["job_id"],
        "build": item["build"],
        "devices": devices,
        "since": item["since"],
        "rig_errors": item.get("rig_errors") or 0,
    }
    del queue[number]
    # Saved before anything is posted: a status that fails to post (GitHub flaking) must not
    # lose the job, or the next pass would submit the head again (staging.py saves first too).
    save_state(state)
    status(gh_, item["sha"], "pending", f"hardware checks on {len(devices)} test device(s)")
    log(f"{item['sha'][:12]} (#{number}): hardware checks {job['job_id']} on {devices}")


def report_queue(server: Server, state: dict[str, Any]) -> None:
    waiting = []
    for number in sorted(state.get("queue") or {}, key=int):
        item = state["queue"][number]
        w = {"sha": item["sha"], "since": item["since"], "what": f"PR #{number}"}
        if item.get("ready") and float(item.get("retry_at") or 0) <= time.time():
            w["ready"] = True  # backing off after a rig error: the staging runner needn't wait
        if item.get("preempted"):
            w["preempted"] = True
        waiting.append(w)
    put_queue(server, RUNNER, waiting)


def _pass(gh_: Gh, server: Server, builds: Callable[[], Builds]) -> dict[str, Any]:
    global PASS_ERROR
    state = load_state()
    try:
        prs = open_prs(gh_)
    except (RuntimeError, ValueError) as e:
        PASS_ERROR = f"can't list the open PRs ({str(e)[:200]})"
        log(f"{PASS_ERROR}; next pass")
        return state
    state.setdefault("queue", {})
    # Settled heads of PRs no longer open: forgotten.
    state["reported"] = {n: s for n, s in (state.get("reported") or {}).items() if n in prs}
    if "running" in state:
        poll_running(gh_, server, state, prs)
        save_state(state)
    for number in sorted(prs, key=int):  # oldest PR first
        judge(gh_, state, number, prs[number])
        save_state(state)
    state.pop("inherit", None)
    # Closed, merged or back to draft while queued: dropped, nothing posted.
    for number in [n for n in state["queue"] if n not in prs or prs[n].get("draft")]:
        log(f"{state['queue'][number]['sha'][:12]}: PR #{number} closed, merged or draft: dropped")
        del state["queue"][number]
    save_state(state)
    if not state["queue"]:
        return state
    try:
        mode = box_mode(server)
    except (urllib.error.URLError, OSError) as e:  # HTTPError included: never a verdict
        log(f"box mode unreadable ({e}); next pass")
        return state
    if holds(mode):
        hold(gh_, state, mode)
        save_state(state)
        return state
    try:
        svc = builds()
    except DevError as e:
        log(f"{e}; nothing can be built")
        return state
    build(gh_, state, svc)
    save_state(state)
    submit(gh_, server, state, mode)
    save_state(state)
    return state


def build_service() -> Builds:
    return BuildService(*build_config())


def run_once(
    gh_: Gh = gh, server: Server = http, builds: Callable[[], Builds] = build_service
) -> dict[str, Any]:
    global PASS_ERROR
    PASS_ERROR = None
    try:
        state = _pass(gh_, server, builds)
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        # GitHub failing mid-pass (a status or comment that didn't post): what was saved
        # stands, and what wasn't is done again next pass. The queue is still reported.
        PASS_ERROR = f"GitHub: {str(e)[:300]}"
        log(f"{PASS_ERROR}; next pass")
        state = load_state()
    report_queue(server, state)
    return state


def retest(number: str) -> str:
    """Queue PR ``number``'s head again on the next pass (a rig flake that ended in a verdict):
    its settled verdict is forgotten and a queued head's backoff dropped. A running test is
    left alone."""
    state = load_state()
    if (state.get("running") or {}).get("pr") == number:
        return f"PR #{number}: its hardware checks are running now; nothing to do"
    had = (state.get("reported") or {}).pop(number, None)
    item = (state.get("queue") or {}).get(number)
    if item:
        item.pop("retry_at", None)
        item["rig_errors"] = 0
    save_state(state)
    if had:
        return f"PR #{number}: {had[:12]} forgotten; the next pass queues its head again"
    if item:
        return f"PR #{number}: {item['sha'][:12]} queued, its backoff dropped"
    return f"PR #{number}: nothing settled or queued; the next pass judges its head"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--status", action="store_true", help="print the state and exit")
    ap.add_argument("--retest", metavar="PR", help="queue the PR's head again (a rig flake)")
    args = ap.parse_args(argv)
    if args.status:
        print(json.dumps(load_state(), indent=2))
        return 0
    if args.retest:
        log(retest(str(int(args.retest))))
        return 0
    run_once()
    return 1 if PASS_ERROR else 0


if __name__ == "__main__":
    sys.exit(main())
