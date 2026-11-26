#!/usr/bin/env python3
"""Hardware-test the staging branch on the test box (#349, docs/RELEASE_PROCESS.md "Staging";
docs/TEST_BOX.md, "Staging runner").

blinky-staging.timer runs this every 2 minutes, from the deploy worktree (master code, like
deploy.py). Two tiers (owner, 2026-11-25):

- **The hardware checks, blocking** (blinky-server testing/hardware_checks.py): the status
  HARDWARE_CONTEXT, which blinky-firmware's master ruleset requires for staging -> master. A red
  commit must not reach the history, so the checks run on each PR head before its merge
  (pr_queue.py), and this runner only confirms them: a staging head whose tree is a PR head
  that passed them (strict up-to-date and squash merges make the two identical) gets
  ``success`` "hardware checks passed before merge as PR #N head <sha8> (same tree)" without a
  run. Any other head is checked here (a fork PR's merge, a head nobody tested).
- **The quality suite, non-blocking** (testing/staging.py: every edm/ track, music and beat
  pace, scores with the corpus pin and the scorer's lag evidence): run on the staging head after
  its hardware checks pass. It reports in the head's comment and the score record, and posts
  QUALITY_CONTEXT, always ``success`` with a short score summary (never a failing status: it
  doesn't judge, it scores).

One pass:

1. A job in flight (GET /api/test/jobs/{id}): finished hardware checks post HARDWARE_CONTEXT
   (success, failure, or pending "rig error: ..., retrying": a rig fault is no verdict and is
   tried again, the next pass first, then backing off) and the report comment on the PR that
   merged the head (one per head, updated in place: the checks table, then the quality
   section). On a pass, if staging's head is still that sha, open the staging -> master PR if
   none is open and turn on its auto-merge, pinned to that sha (a merge commit: master's tree
   is the tested tree). A finished quality suite updates the comment and posts QUALITY_CONTEXT.
   The report and the promotion PR list the Claude review verdict of every PR merged into
   staging since master: informational, never a gate.
2. Otherwise, the staging head: already settled, nothing to do but its quality suite if it is
   due. No change under the firmware's paths (tools/firmware_paths.json at the head) since
   master: a passing status saying the test was skipped, and promote. Its staging build
   (staging-build.yml, artifact staging-firmware-<sha>) failed: a failure. Not built yet: a
   pending status. Built, with the same Firmware fingerprint (the build's hash of the image with
   per-commit fields fixed) as the staging commit master was last merged from, and no change
   under the list's "unfingerprinted" paths: the same image, so skip and promote as above. Its
   tree a PR head's that passed the hardware checks: confirmed, promoted. Otherwise the hardware
   checks (POST /api/test/hardware-checks, source staging) on every connected, configured chip.

The box's mode (GET /api/box/mode, docs/DEV_LOOP.md): in dev mode (or switching to it) a head
that would be submitted is held instead, with the pending status "queued: box in dev mode since
HH:MM (N waiting)", and reported to the server as waiting (PUT /api/box/queue/staging) so it
shows in GET /api/box/mode, dev.py status and /metrics. Back in queue mode it is submitted as
before. A job that dev mode preempted (its job among the mode's "preempted") is not a
failure: its head goes back in the queue and is tested again.

The box is shared with the PR hardware queue (pr_queue.py), and the server serialises them (one
hardware test at a time; a busy box answers 409 and the head is tried again next pass). A head
ready for its hardware checks is reported as waiting in queue mode too, and the runner yields to
another runner's ready item that has waited longer, so neither starves the other
(older_elsewhere). The quality suite, being non-blocking, yields to any ready PR head.

Only the newest staging commit is tested; a commit superseded while it waits is skipped (its
successor carries its changes). ``--retest`` forgets the head's verdict (a rig flake), and
``--dry-run`` shows what a pass would post without posting or submitting anything. State is in
~/.local/share/blinky-server/staging.json; the log goes to the journal
(`journalctl -u blinky-staging`). `--status` prints the state. Stdlib only.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any

from deploy import review_verdict  # the deployer's parser: one reading of the verdict comment

# The firmware and its staging flow live in blinky-firmware (blinky_time#357): its staging
# head is what gets tested, and its statuses and PRs are what this posts.
GH_REPO = "Jdubz/blinky-firmware"
BRANCH = "staging"
# The hardware checks' status: blinky-firmware's master ruleset requires it for staging -> master.
HARDWARE_CONTEXT = "Hardware test (staging)"
PR_CONTEXT = "Hardware test (PR)"  # pr_queue.py's, on PR heads: what a staging head confirms
QUALITY_CONTEXT = "Quality suite (staging)"  # always success: scores, never a verdict
HOME = Path.home()
LINK = HOME / "blinky-deploy"
BASE_URL = "http://127.0.0.1:8420"
# Host endpoints are private (this repo is public): the box's own URL, as the home network reaches
# it, is BLINKY_TESTBOX_URL in the environment or in this file (template: scripts/hosts.env.example).
HOSTS_ENV = Path(os.environ.get("BLINKY_HOSTS_ENV") or HOME / ".config/blinky/hosts.env")
STATE = (
    Path(os.environ.get("XDG_DATA_HOME") or HOME / ".local/share")
    / "blinky-server"
    / "staging.json"
)
BUILD_WORKFLOW = "staging-build.yml"
# The status staging-build.yml posts on each staging head: "sha256:<hex>" over the hex and UF2
# built with every per-commit field fixed. Equal fingerprints, equal image on the device.
FINGERPRINT_CONTEXT = "Firmware fingerprint"
FINGERPRINT_RE = re.compile(r"sha256:[0-9a-f]{64}")
# The paths that change what is built and flashed, owned by the firmware repo and read at the
# commit being judged; its release.yml skips a candidate by the same list.
FIRMWARE_PATHS = "tools/firmware_paths.json"
COMPARE_FILE_LIMIT = 300  # GitHub's compare lists at most this many files
CORPUS = "corpus/edm"  # the blinky-corpus submodule
REPORT_HEADER = "## Hardware test (staging) — "
REVIEWS_HEADER = "Claude reviews since master (informational, not gated):"
TERMINAL = ("complete", "error", "cancelled")  # GET /api/test/jobs/{id} status (work/jobs.py)
RUNNER = "staging"  # this runner's name in the box's hardware queue (PUT /api/box/queue/staging)
# Another runner's waiting item counts (we yield to it) only while that runner still reports:
# both run every 2 minutes, so a report older than this is a runner that stopped.
REPORT_STALE_S = 10 * 60

Gh = Callable[..., str]


def log(msg: str) -> None:
    print(msg, flush=True)


def host_setting(key: str) -> str:
    """A host endpoint: the environment's ``key``, else the private hosts file's (KEY=value or
    KEY="value", the last one wins; parsed, never sourced). No default: a real host never goes in
    this repo. Missing, or still the template's <placeholder>: RuntimeError naming file and key."""
    value = os.environ.get(key, "").strip()
    if not value and HOSTS_ENV.is_file():
        for line in HOSTS_ENV.read_text().splitlines():
            m = re.match(rf"\s*{re.escape(key)}\s*=\s*(.*?)\s*$", line)
            if m:
                value = m.group(1)
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
    if not re.fullmatch(r"https?://[^\s<>]+", value):
        raise RuntimeError(
            f"{key} missing or not a URL: set it in {HOSTS_ENV} (mode 600, template "
            f"scripts/hosts.env.example), e.g. {key}=http://<test-box>:8420"
        )
    return value.rstrip("/")


def gh(*args: str, stdin: str | None = None) -> str:
    r = subprocess.run(
        ["gh", *args], capture_output=True, text=True, timeout=120, check=False, input=stdin
    )
    if r.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args[:3])} -> {r.returncode}: {r.stderr[-1000:]}")
    return r.stdout


def gh_json(gh_: Gh, *args: str) -> Any:
    return json.loads(gh_(*args) or "null")


# -- the server --------------------------------------------------------------------------------


def api_key() -> str:
    key = os.environ.get("BLINKY_API_KEY")
    if not key:
        key = (HOME / ".blinky-api-key").read_text().strip()
    return key


def http(method: str, path: str, body: Any = None, timeout: float = 30) -> Any:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        BASE_URL + path,
        data=data,
        method=method,
        headers={"X-API-Key": api_key(), "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def box_mode(server: Callable[..., Any]) -> dict[str, Any]:
    """The box's mode (GET /api/box/mode). A server without the route (an older one, mid-deploy)
    is in queue mode: the box ran that way before it had modes."""
    try:
        mode: dict[str, Any] = server("GET", "/api/box/mode")
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
        return {"mode": "queue", "waiting": [], "preempted": []}
    return mode


def holds(mode: dict[str, Any]) -> bool:
    """The runners hold back: dev mode, or switching to it."""
    return mode.get("mode") == "dev" or bool(mode.get("switching"))


def queued_description(mode: dict[str, Any], waiting: int) -> str:
    if mode.get("mode") == "dev":
        when = time.strftime("%H:%M", time.localtime(float(mode.get("since") or time.time())))
        return f"queued: box in dev mode since {when} ({waiting} waiting)"
    return f"queued: box switching to dev mode ({waiting} waiting)"


def put_queue(server: Callable[..., Any], runner: str, waiting: list[dict[str, Any]]) -> None:
    """PUT /api/box/queue/{runner}. Display (and the other runner's turn-taking) only: a server
    that can't take it is logged, never a reason to stop."""
    try:
        server("PUT", f"/api/box/queue/{runner}", {"waiting": waiting})
    except urllib.error.HTTPError as e:
        if e.code != 404 or waiting:  # an older server has no queue: fine while nothing waits
            log(f"box queue not reported: server answered {e.code}")
    except (urllib.error.URLError, OSError) as e:
        log(f"box queue not reported: {e}")


def report_queue(server: Callable[..., Any], state: dict[str, Any]) -> None:
    """Tell the server what this runner holds back: behind dev mode, or ready and waiting for
    the box (``ready``: built, so the other runner yields to it if it is older)."""
    q = state.get("queued")
    if q and q["sha"] == state.get("reported"):
        q = None  # settled this pass (skipped, or its build failed): nothing waits
    waiting = [{"sha": q["sha"], "since": q["since"], "what": "staging head"}] if q else []
    if q and q.get("preempted"):
        waiting[0]["preempted"] = True
    if q and q.get("ready"):
        waiting[0]["ready"] = True
    due = quality_due(state)
    if due and not waiting and "running" not in state:
        # Never "ready": the PR runner's blocking checks don't wait for a non-blocking suite.
        since = float(state.get("quality_since") or time.time())
        waiting = [{"sha": due, "since": since, "what": "staging quality suite (non-blocking)"}]
    put_queue(server, RUNNER, waiting)


def quality_due(state: dict[str, Any]) -> str | None:
    """The settled head whose quality suite is still to run: it passed the hardware checks."""
    sha = state.get("hw_passed")
    if sha and sha == state.get("reported") and state.get("quality_done") != sha:
        return str(sha)
    return None


def older_elsewhere(
    mode: dict[str, Any], runner: str, since: float, now: float | None = None
) -> dict[str, Any] | None:
    """Another runner's item, ready to submit, that has waited longer than ``since``: this
    runner yields to it. Only while that runner still reports (REPORT_STALE_S), so a stopped
    runner never blocks this one."""
    now = time.time() if now is None else now
    for w in mode.get("waiting") or []:  # oldest first
        if w.get("runner") == runner or not w.get("ready"):
            continue
        at = w.get("reported_at")
        if at is None or now - float(at) > REPORT_STALE_S:
            continue
        if float(w.get("since") or 0.0) < since:
            return dict(w)
    return None


def test_devices(devices: list[dict[str, Any]]) -> list[str]:
    """Connected, configured nRF52840s: on the box every chip is a test chip, so no list."""
    return [
        d["id"]
        for d in devices
        if d.get("state") == "connected"
        and d.get("platform") == "nrf52840"
        and d.get("device_type")
    ]


# -- GitHub ------------------------------------------------------------------------------------


def staging_head(gh_: Gh) -> str:
    return str(gh_json(gh_, "api", f"repos/{GH_REPO}/branches/{BRANCH}")["commit"]["sha"])


def build_state(gh_: Gh, sha: str) -> str:
    """'built' | 'failed' | 'pending' for sha's staging build."""
    name = f"staging-firmware-{sha}"
    arts = gh_json(gh_, "api", f"repos/{GH_REPO}/actions/artifacts?name={name}&per_page=5")
    if any(not a.get("expired") for a in arts.get("artifacts", [])):
        return "built"
    # The build runs after PR Validation on the push; either failing means no build is coming.
    runs = gh_json(gh_, "api", f"repos/{GH_REPO}/actions/runs?head_sha={sha}&per_page=50")
    for r in runs.get("workflow_runs", []):
        name_ = r.get("path", "").rsplit("/", 1)[-1]
        if r.get("status") != "completed":
            continue
        if name_ == BUILD_WORKFLOW and r.get("conclusion") not in ("success", "skipped"):
            return "failed"
        if (
            name_ == "pr-validation.yml"
            and r.get("event") == "push"
            and r.get("conclusion") != "success"
        ):
            return "failed"
    return "pending"


def firmware_change(gh_: Gh, sha: str, base: str = "master") -> tuple[bool, str, bool]:
    """(changed, why, blind): whether sha changes anything under the firmware's paths since
    ``base`` (its merge-base with that branch), and whether one of those changes is where the
    Firmware fingerprint can't see it (the list's "unfingerprinted" paths: the bootloader, the
    OTA signing tools).

    Anything that can't be judged counts as a change the fingerprint can't vouch for, so it
    gets the hardware test: no path list at that commit, a GitHub error, or a compare GitHub
    truncated.
    """
    try:
        meta = gh_json(gh_, "api", f"repos/{GH_REPO}/contents/{FIRMWARE_PATHS}?ref={sha}")
        spec = json.loads(base64.b64decode(meta["content"]))
        paths = [str(p) for p in spec["paths"]]
        unseen = [str(p) for p in spec.get("unfingerprinted", [])]
        cmp = gh_json(gh_, "api", f"repos/{GH_REPO}/compare/{base}...{sha}")
    except (RuntimeError, KeyError, ValueError, TypeError) as e:
        return True, f"can't compare with {base} ({str(e)[:80]}): testing", True
    if not paths:
        return True, f"{FIRMWARE_PATHS} lists no paths: testing", True
    files = cmp.get("files")
    if not isinstance(files, list) or len(files) >= COMPARE_FILE_LIMIT:
        return True, f"the compare with {base} is truncated: testing", True

    def under(name: str, prefixes: list[str]) -> bool:
        return any(name.startswith(p) if p.endswith("/") else name == p for p in prefixes)

    hits = [
        n
        for f in files
        for n in (f.get("filename"), f.get("previous_filename"))
        if n and under(n, paths)
    ]
    mb = str(cmp.get("merge_base_commit", {}).get("sha", base))[:12]
    if hits:
        blind = any(under(n, unseen) for n in hits)
        return True, f"{len(hits)} firmware file(s) changed since {base} {mb}", blind
    return False, f"no firmware change since {base} {mb}: hardware test skipped", False


def fingerprint(gh_: Gh, sha: str) -> str | None:
    """sha's Firmware fingerprint ("sha256:<64 hex>"), or None.

    Only the newest status with that context counts: if it isn't a success with a well-formed
    hash, there is no fingerprint, whatever older statuses say.
    """
    statuses = gh_json(gh_, "api", f"repos/{GH_REPO}/commits/{sha}/statuses?per_page=100")
    for s in statuses or []:  # newest first
        if s.get("context") != FINGERPRINT_CONTEXT:
            continue
        d = str(s.get("description") or "")
        ok = s.get("state") == "success" and FINGERPRINT_RE.fullmatch(d)
        return d if ok else None
    return None


def same_image(gh_: Gh, sha: str) -> tuple[bool, str]:
    """(same, why): whether sha builds the image master was last merged from.

    Master takes only merges of a tested staging head (its second parent). A fingerprint
    missing on either side, or one GitHub won't return, means a different image: test.
    """
    try:
        head = fingerprint(gh_, sha)
        if head is None:
            return False, "no firmware fingerprint on the head: testing"
        parents = gh_json(gh_, "api", f"repos/{GH_REPO}/commits/master")["parents"]
        if len(parents) != 2:
            return False, "master's head is not a staging merge: testing"
        tested = str(parents[1]["sha"])
        base = fingerprint(gh_, tested)
    except (RuntimeError, KeyError, ValueError, TypeError) as e:
        return False, f"can't read the firmware fingerprints ({str(e)[:80]}): testing"
    if base is None:
        return False, f"master's staging commit {tested[:12]} has no firmware fingerprint: testing"
    if base != head:
        return False, "the firmware image differs from master's: testing"
    return True, f"same firmware image as master's ({head[:19]}): hardware test skipped"


def skip(gh_: Gh, state: dict[str, Any], sha: str, why: str) -> dict[str, Any]:
    """Nothing on the device changes: master gets sha without a run (the owner, 2026-11-05).

    Reported only once promoted, so a failed PR create or auto-merge is retried next pass.
    """
    post_status(gh_, sha, "success", why)
    log(f"{sha[:12]}: {why}")
    number = promote(gh_, sha, None, skipped=why)
    log(f"{sha[:12]}: staging -> master PR #{number}, auto-merge on")
    state["reported"] = sha
    state.pop("waiting", None)
    save_state(state)
    return state


def post_status(
    gh_: Gh,
    sha: str,
    state: str,
    description: str,
    url: str | None = None,
    context: str = HARDWARE_CONTEXT,
) -> None:
    args = [
        "api",
        "-X",
        "POST",
        f"repos/{GH_REPO}/statuses/{sha}",
        "-f",
        f"state={state}",
        "-f",
        f"context={context}",
        "-f",
        f"description={description[:140]}",
    ]
    if url:
        args += ["-f", f"target_url={url}"]
    gh_(*args)


def staging_pr(gh_: Gh, sha: str) -> int | None:
    """The PR that merged sha into staging, if any."""
    for p in gh_json(gh_, "api", f"repos/{GH_REPO}/commits/{sha}/pulls") or []:
        if p.get("merged_at") and p["base"]["ref"] == BRANCH:
            return int(p["number"])
    return None


def review_verdicts(gh_: Gh, sha: str) -> str:
    """Markdown: the Claude review verdict of every PR merged into staging since master, up to
    sha. Informational only (owner, 2026-11-23): nothing here blocks a promotion, so a
    GitHub error becomes a note in the text, never an exception. A PR's verdict is the review
    workflow's comment on its last commit, read as the box deployer reads it."""
    try:
        # Every page: a compare lists at most 250 commits per response, and a busy PR can have
        # more than 100 comments (its verdict is among the newest). One JSON value per line.
        shas = gh_(
            "api",
            "--paginate",
            f"repos/{GH_REPO}/compare/master...{sha}",
            "--jq",
            ".commits[].sha",
        ).split()
        prs: dict[int, dict[str, Any]] = {}
        for c in shas:
            for p in gh_json(gh_, "api", f"repos/{GH_REPO}/commits/{c}/pulls") or []:
                if p.get("merged_at") and p["base"]["ref"] == BRANCH:
                    prs[int(p["number"])] = p
        lines = []
        for n, p in sorted(prs.items()):
            raw = gh_(
                "api",
                "--paginate",
                f"repos/{GH_REPO}/issues/{n}/comments?per_page=100",
                "--jq",
                ".[] | {user: {login: .user.login}, body: .body}",
            )
            comments = [json.loads(ln) for ln in raw.splitlines() if ln.strip()]
            verdict, why = review_verdict(comments, p["head"]["sha"], f"#{n}")
            mark = "✅" if verdict == "deploy" else "⚠️"
            lines.append(f"- {mark} {why} ({p.get('title', '')})")
    except (
        RuntimeError,
        OSError,
        subprocess.TimeoutExpired,
        ValueError,
        KeyError,
        TypeError,
    ) as e:
        return f"{REVIEWS_HEADER} couldn't read them ({str(e)[:120]})\n"
    if not lines:
        return f"{REVIEWS_HEADER} no PRs found since master\n"
    return "\n".join([REVIEWS_HEADER, "", *lines]) + "\n"


def promote(gh_: Gh, sha: str, report_url: str | None, skipped: str | None = None) -> str:
    """Open the staging -> master PR if none is open; turn on its auto-merge (merge commit)."""
    open_prs = gh_json(
        gh_,
        "pr",
        "list",
        "--repo",
        GH_REPO,
        "--base",
        "master",
        "--head",
        BRANCH,
        "--state",
        "open",
        "--json",
        "number,url",
    )
    if skipped:
        body = (
            f"Staging at `{sha[:12]}`: {skipped}. Nothing on the device changes, so merging "
            "this to master cuts no candidate when the newest bN has the same files or image "
            "(docs/RELEASE_PROCESS.md). "
            "Opened by the test box (scripts/testbox/staging.py)."
        )
    else:
        body = (
            f"Staging at `{sha[:12]}` passed the hardware test on the box's test chips"
            + (f": {report_url}" if report_url else ".")
            + "\n\nMerging this to master cuts a candidate release (docs/RELEASE_PROCESS.md). "
            "Opened by the test box (scripts/testbox/staging.py)."
        )
    body += "\n\n" + review_verdicts(gh_, sha)
    if open_prs:
        number = str(open_prs[0]["number"])
        gh_("pr", "comment", number, "--repo", GH_REPO, "--body", body)
    else:
        url = gh_(
            "pr",
            "create",
            "--repo",
            GH_REPO,
            "--base",
            "master",
            "--head",
            BRANCH,
            "--title",
            f"Promote staging {sha[:12]} to master",
            "--body",
            body,
        ).strip()
        number = url.rsplit("/", 1)[-1]
    # Pinned to the tested commit: GitHub refuses the merge if the head has moved since.
    gh_("pr", "merge", number, "--repo", GH_REPO, "--merge", "--auto", "--match-head-commit", sha)
    return number


# -- the report --------------------------------------------------------------------------------


def _mean(xs: list[float | None]) -> float | None:
    got = [x for x in xs if x is not None]
    return round(sum(got) / len(got), 3) if got else None


DRUMS = ("kickR50", "snareR50", "onDrum", "drumF", "loneHat")
DRUM_HEAD = "kick R@50 | snare R@50 | on-drum | drum F | lone hat"


def verdict_line(report: dict[str, Any]) -> str:
    """The quality suite's headline: what ran where, and whether its health and end-of-run
    checks held. Reported, never a verdict on the commit (the hardware checks are that)."""
    held = "all hold" if report.get("passed") else "some don't (see below)"
    return (
        f"`{report.get('version', '?')}` on "
        f"{', '.join(d[:12] for d in report.get('devices', []))}: health and end-of-run checks "
        f"{held}. Reported, not gated."
    )


def check_rows(report: dict[str, Any], sections: tuple[str, ...]) -> list[str]:
    """A table of every pass/fail check in ``sections``, per device."""
    rows = ["| device | check | | detail |", "|---|---|---|---|"]
    for section in sections:
        for dev, checks in (report.get(section) or {}).items():
            for name, c in checks.items():
                if not isinstance(c, dict) or "pass" not in c:
                    continue
                detail = ", ".join(
                    f"{k}={v}" for k, v in c.items() if k not in ("pass", "lines", "series_ms")
                )
                rows.append(f"| {dev[:12]} | {name} | {'✅' if c['pass'] else '❌'} | {detail} |")
    return [*rows, ""]


def alert_lines(report: dict[str, Any]) -> list[str]:
    """The firmware's [ASSERT]/[FALLBACK] lines since the flash, if any."""
    alerts = [
        (dev, a)
        for dev, f in (report.get("final") or {}).items()
        for a in (f.get("alerts") or {}).get("lines", [])
    ]
    if not alerts:
        return []
    return ["", "Firmware alerts:", "```", *[f"{d[:12]} {a}" for d, a in alerts[:30]], "```"]


def quality_lines(report: dict[str, Any]) -> list[str]:
    """The quality suite's report as markdown: what ran, every check per device, drum means
    with the corpus pin, timing, alerts. Non-blocking: it scores, the hardware checks judge."""
    lines = [verdict_line(report), ""]
    dep = report.get("deploy") or {}
    if not dep.get("pass"):
        lines += [f"Deploy failed at `{dep.get('failed_step')}`: {dep.get('message')}"]
        return lines
    lines += check_rows(report, ("health", "music", "final"))
    val = report.get("validation") or {}
    if val.get("results"):
        by_dev: dict[str, dict[str, list[float | None]]] = {}
        for r in val["results"]:
            for dev, s in (r.get("scores") or {}).items():
                drums = s.get("drums") or {}
                for k in DRUMS:
                    by_dev.setdefault(dev, {}).setdefault(k, []).append(drums.get(k))
        pin = str(report.get("corpus") or "unknown")[:12]
        lines += [
            (
                f"Validation: {val.get('played')}/{val.get('tracks')} edm/ tracks played "
                f"(reported, not gated; corpus `{pin}`). Means over tracks:"
            ),
            "",
            f"| device | {DRUM_HEAD} |",
            "|---|---|---|---|---|---|",
        ]
        for dev, m in by_dev.items():
            vals = [_mean(m[k]) for k in DRUMS]
            lines.append(f"| {dev[:12]} | " + " | ".join(str(v) for v in vals) + " |")
        lines.append("")
        lags = lag_counts(report)
        if lags:
            lines += [
                f"Scorer lag: {lags} (blinky-corpus lag.py: an ambiguous or fallback lag is "
                "flagged, not hidden).",
                "",
            ]
    elif val:
        lines += [f"Validation: not played ({val.get('detail')})", ""]
    for dev, f in (report.get("final") or {}).items():
        lines.append(f"Onset-path timing ({dev[:12]}): {f.get('timing') or 'not reported'}")
    lines += alert_lines(report)
    return lines


def lag_counts(report: dict[str, Any]) -> str:
    """Where each track's scoring lag came from, counted: "16 ESTIMATED, 2 FALLBACK; 3
    ambiguous"."""
    src: dict[str, int] = {}
    ambiguous = 0
    for r in (report.get("validation") or {}).get("results") or []:
        for s_ in (r.get("scores") or {}).values():
            d = s_.get("drums") or {}
            if d.get("lagSource"):
                src[str(d["lagSource"])] = src.get(str(d["lagSource"]), 0) + 1
            ambiguous += bool(d.get("lagAmbiguous"))
    if not src:
        return ""
    out = ", ".join(f"{n} {k}" for k, n in sorted(src.items()))
    return out + (f"; {ambiguous} ambiguous" if ambiguous else "")


def quality_summary(report: dict[str, Any] | None) -> str:
    """QUALITY_CONTEXT's description: a few means, always a success."""
    if not report or not (report.get("validation") or {}).get("results"):
        why = "no scores" if report else "the suite didn't complete"
        return f"non-blocking: {why} (see the report)"
    means: dict[str, list[float | None]] = {}
    for r in report["validation"]["results"]:
        for s_ in (r.get("scores") or {}).values():
            for k in ("drumF", "kickR50", "snareR50"):
                means.setdefault(k, []).append((s_.get("drums") or {}).get(k))
    val = report["validation"]
    parts = [f"{k} {_mean(v)}" for k, v in means.items()]
    health = "health ok" if report.get("passed") else "health/asserts: see report"
    return f"non-blocking: {', '.join(parts)} over {val.get('played')}/{val.get('tracks')} tracks; {health}"


# -- the hardware checks' report (shared with pr_queue.py and dev.py) -----------------------------

HW_PASSED = "hardware checks passed"  # every passing status starts so (staging matches on it)
HW_MARK = {"yes": "✅ yes", "no": "❌ no", "not applicable": "n/a"}
PHASE_MARK = {"yes": "✅", "no": "❌", "rig error": "⚠️ rig error"}


def hw_failed_at(report: dict[str, Any]) -> str:
    """The first failing check, as "phase: check (evidence)"."""
    for c in report.get("checks") or []:
        if c.get("result") == "no":
            return f"{c['phase']}: {c['name']} ({str(c.get('evidence'))[:60]})"
    return "no checks ran"


def hw_status(report: dict[str, Any]) -> tuple[str, str]:
    """The commit status for a hardware-checks report: (state, description). A rig error is
    pending (no verdict on the firmware), never a failure."""
    if report.get("rig_error"):
        return "pending", f"rig error: {report['rig_error']}"[:110] + ", retrying"
    if report.get("passed"):
        phases = ", ".join(p["name"] for p in report.get("phases") or [])
        return "success", f"{HW_PASSED}: {phases}"
    return "failure", f"hardware checks FAILED at {hw_failed_at(report)}"


def hw_lines(report: dict[str, Any]) -> list[str]:
    """The hardware checks as markdown: the verdict, each phase's time, the table of checks
    (check, yes/no, evidence)."""
    devices = ", ".join(d[:12] for d in report.get("devices") or [])
    version = report.get("version") or "?"
    if report.get("rig_error"):
        head = f"**RIG ERROR, no verdict** (retried): {report['rig_error']}"
    elif report.get("passed"):
        head = f"**PASSED** — `{version}` on {devices}"
    else:
        head = f"**FAILED** at {hw_failed_at(report)} — `{version}` on {devices}"
    lines = [head, ""]
    phases = report.get("phases") or []
    if phases:
        lines += [
            "Phases (in order, stopping at the first failure): "
            + " · ".join(
                f"{p['name']} {PHASE_MARK.get(p['result'], p['result'])}"
                + (f" {p['seconds']:.0f} s" if isinstance(p.get("seconds"), (int, float)) else "")
                for p in phases
            ),
            "",
        ]
    checks = report.get("checks") or []
    if checks:
        multi = len({c["device"] for c in checks}) > 1
        lines += ["| phase | check | | evidence |", "|---|---|---|---|"]
        for c in checks:
            name = f"{c['name']} ({c['device'][:8]})" if multi else c["name"]
            ev = str(c.get("evidence")).replace("|", "\\|").replace("\n", " ")
            mark = HW_MARK.get(c["result"], c["result"])
            lines.append(f"| {c['phase']} | {name} | {mark} | {ev} |")
        lines.append("")
    for dev, meta in ((report.get("response") or {}).get("meta") or {}).items():
        lines.append(
            f"Response ({dev[:12]}): rig lag {meta.get('lag_ms'):.0f} ms from the "
            f"{meta.get('lag_source')} ({meta.get('lag_matched')} hits), "
            f"{meta.get('rows')} stream lines, {meta.get('detections')} detections."
        )
    return lines


def upsert_comment(gh_: Gh, number: str, header: str, body: str) -> str | None:
    """The comment on issue/PR ``number`` whose first line is ``header``: updated in place,
    else added. Returns its URL."""
    raw = gh_(
        "api",
        "--paginate",
        f"repos/{GH_REPO}/issues/{number}/comments?per_page=100",
        "--jq",
        '.[] | {id, url: .html_url, first: (.body | split("\\n")[0])}',
    )
    found = next(
        (
            c
            for ln in raw.splitlines()
            if ln.strip()
            for c in [json.loads(ln)]
            if c["first"].strip() == header
        ),
        None,
    )
    payload = json.dumps({"body": body})
    if found:
        got = _gh_stdin(
            gh_, payload, "-X", "PATCH", f"repos/{GH_REPO}/issues/comments/{found['id']}"
        )
    else:
        got = _gh_stdin(gh_, payload, "-X", "POST", f"repos/{GH_REPO}/issues/{number}/comments")
    return str(got.get("html_url")) if isinstance(got, dict) and got.get("html_url") else None


def _gh_stdin(gh_: Gh, payload: str, *args: str) -> Any:
    return json.loads(gh_("api", *args, "--input", "-", stdin=payload) or "null")


def rig_backoff(n: int) -> float:
    """Seconds before the next try after ``n`` rig errors in a row: the next pass first, then
    doubling to RIG_BACKOFF_MAX_S, so a broken rig isn't flashed every pass for good."""
    return float(min(RIG_BACKOFF_MAX_S, 120.0 * 2 ** max(0, n - 1)))


RIG_BACKOFF_MAX_S = 30 * 60


# -- one pass ----------------------------------------------------------------------------------


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


QUALITY_PENDING = "Runs on this commit after its hardware checks, on the box; non-blocking."
QUALITY_NOT_RUN = "Not run: the hardware checks didn't pass."


def report_comment(gh_: Gh, sha: str, hardware: str, quality: str) -> str | None:
    """The head's one comment on the PR that merged it (updated in place): the hardware checks
    first, then the quality section, then the review verdicts. Its URL, or None (no PR)."""
    pr = staging_pr(gh_, sha)
    if pr is None:
        return None
    body = "\n".join(
        [
            f"{REPORT_HEADER}{sha}",
            "",
            "### Hardware checks (blocking)",
            "",
            hardware,
            "",
            "### Quality suite (non-blocking)",
            "",
            quality,
            "",
            review_verdicts(gh_, sha),
        ]
    )
    return upsert_comment(gh_, str(pr), f"{REPORT_HEADER}{sha}", body)


def _archive_line(report: dict[str, Any]) -> str:
    if not report.get("artifact"):
        return ""
    # Presigned URLs expire, so link the box's redirect, which signs a fresh one. The store
    # signs its LAN address, so the link opens from the home network.
    key = report["artifact"]
    link = f"{host_setting('BLINKY_TESTBOX_URL')}/api/spark/artifacts/{key}"
    return f"Full report in the Spark artifact store: `{key}` ([open]({link}), home network)\n\n"


def finish_hw(gh_: Gh, state: dict[str, Any], job: dict[str, Any]) -> None:
    run = state.pop("running")
    sha = run["sha"]
    report = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(report, dict) or "passed" not in report:
        # The job itself crashed or was lost: no verdict on the firmware.
        report = {
            "sha": sha,
            "passed": False,
            "devices": run["devices"],
            "rig_error": f"the hardware-checks job ended {job.get('status')}: "
            f"{job.get('error') or 'no result'}",
        }
    hardware = "\n".join(hw_lines(report))
    state["hw_md"] = {"sha": sha, "text": hardware}
    quality = QUALITY_PENDING if report.get("passed") else QUALITY_NOT_RUN
    url = report_comment(gh_, sha, hardware, quality)
    st, desc = hw_status(report)
    post_status(gh_, sha, st, desc, url)
    if report.get("rig_error"):
        rig = state.get("rig") if (state.get("rig") or {}).get("sha") == sha else None
        n = int((rig or {}).get("n") or 0) + 1
        state["rig"] = {"sha": sha, "n": n, "retry_at": time.time() + rig_backoff(n) - 60}
        log(f"{sha[:12]}: {desc} (rig error {n} in a row)")
        return
    state.pop("rig", None)
    state["reported"] = sha
    log(f"{sha[:12]}: {desc} ({url or 'no PR to comment on'})")
    if report.get("passed"):
        passed(gh_, state, sha, url)


def passed(gh_: Gh, state: dict[str, Any], sha: str, url: str | None) -> None:
    """The head passed its hardware checks: its quality suite is due, and it goes to master."""
    state["hw_passed"] = sha
    state["quality_since"] = time.time()
    # Only the commit that was checked goes to master. Staging moved on meanwhile: the new
    # head gets its own checks, and its own promotion.
    head = staging_head(gh_)
    if head != sha:
        log(f"{sha[:12]}: passed, but staging is now at {head[:12]}; not promoting")
        return
    number = promote(gh_, sha, url)
    log(f"{sha[:12]}: staging -> master PR #{number}, auto-merge on")


def tree_match(gh_: Gh, sha: str) -> tuple[tuple[int, str, str | None] | None, str]:
    """((PR number, its head, its report URL) or None, why): whether ``sha`` is the squash of a
    PR whose head has the same tree and passed the hardware checks (PR_CONTEXT, newest status
    a success saying so). Anything unreadable is no match: the checks run here."""
    try:
        pr = next(
            (
                p
                for p in gh_json(gh_, "api", f"repos/{GH_REPO}/commits/{sha}/pulls") or []
                if p.get("merged_at") and p["base"]["ref"] == BRANCH
            ),
            None,
        )
        if pr is None:
            return None, "no PR merged it: checking it here"
        head = str(pr["head"]["sha"])
        tree = gh_json(gh_, "api", f"repos/{GH_REPO}/git/commits/{sha}")["tree"]["sha"]
        head_tree = gh_json(gh_, "api", f"repos/{GH_REPO}/git/commits/{head}")["tree"]["sha"]
        if tree != head_tree:
            return None, f"its tree isn't PR #{pr['number']} head {head[:8]}'s: checking it here"
        statuses = gh_json(gh_, "api", f"repos/{GH_REPO}/commits/{head}/statuses?per_page=100")
        newest = next((st for st in statuses or [] if st.get("context") == PR_CONTEXT), None)
    except (RuntimeError, KeyError, ValueError, TypeError) as e:
        return None, f"can't compare with its PR head ({str(e)[:80]}): checking it here"
    if (
        newest is None
        or newest.get("state") != "success"
        or not str(newest.get("description") or "").startswith(HW_PASSED)
    ):
        got = f"{newest.get('state')} {newest.get('description')!r}" if newest else "no status"
        return None, f"PR #{pr['number']} head {head[:8]} has {got}: checking it here"
    return (int(pr["number"]), head, newest.get("target_url")), "same tree"


def confirm(
    gh_: Gh, state: dict[str, Any], sha: str, number: int, head: str, url: str | None
) -> None:
    """A head whose tree a PR head that passed the hardware checks has: success, no rerun."""
    desc = f"{HW_PASSED} before merge as PR #{number} head {head[:8]} (same tree)"
    hardware = (
        f"**PASSED before merge** as PR #{number} head `{head[:12]}`, whose tree this commit "
        "has (a squash of an up-to-date PR): not run again."
        + (f" [Its report]({url})." if url else "")
    )
    state["hw_md"] = {"sha": sha, "text": hardware}
    link = report_comment(gh_, sha, hardware, QUALITY_PENDING)
    post_status(gh_, sha, "success", desc, link or url)
    state.pop("rig", None)
    state["reported"] = sha
    log(f"{sha[:12]}: {desc}")
    passed(gh_, state, sha, link or url)


def finish_quality(gh_: Gh, state: dict[str, Any], job: dict[str, Any]) -> None:
    run = state.pop("running")
    sha = run["sha"]
    report = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(report, dict) or "passed" not in report:
        report = None
    state["quality_done"] = sha
    if report is None:
        quality = f"Didn't complete ({job.get('status')}: {job.get('error') or 'no result'})."
    else:
        quality = _archive_line(report) + "\n".join(quality_lines(report))
    md = state.get("hw_md") or {}
    hardware = md["text"] if md.get("sha") == sha else "Passed (see the status)."
    url = report_comment(gh_, sha, hardware, quality)
    desc = quality_summary(report)
    post_status(gh_, sha, "success", desc, url, context=QUALITY_CONTEXT)
    log(f"{sha[:12]}: quality suite done: {desc}")


RETRY_CODES = (409, 429)  # 4xx that mean "not now"; every 5xx is retried too


def _detail(e: urllib.error.HTTPError) -> str:
    try:
        body = json.loads(e.read() or b"{}")
        return str(body.get("detail", body))[:100]
    except (ValueError, OSError):
        return str(e.reason)[:100]


def run_once(
    gh_: Gh = gh,
    server: Callable[..., Any] = http,
) -> dict[str, Any]:
    state = _pass(gh_, server)
    report_queue(server, state)
    return state


def preempted(server: Callable[..., Any], job_id: str) -> dict[str, Any] | None:
    """The box mode's record of ``job_id`` if dev mode preempted it, else None. A mode that
    can't be read raises (URLError, OSError): the caller retries, it never guesses."""
    mode = box_mode(server)
    return next((p for p in mode.get("preempted") or [] if p.get("job") == job_id), None)


def _poll(gh_: Gh, server: Callable[..., Any], state: dict[str, Any]) -> dict[str, Any]:
    run = state["running"]
    kind = run.get("kind", "quality")  # a suite from before the hardware checks: not a verdict
    try:
        job = server("GET", f"/api/test/jobs/{run['job']}")
    except urllib.error.HTTPError as e:
        if e.code != 404:
            log(f"server answered {e.code}; next pass")
            return state
        # The work store lost it: settled loudly (a rig error for the checks), not a wait.
        job = {"status": "error", "error": f"job {run['job']} not found"}
    except (urllib.error.URLError, OSError) as e:
        log(f"server not answering ({e}); next pass")
        return state
    if job.get("status") not in TERMINAL:
        log(f"{run['sha'][:12]}: {kind}: {job.get('progressMessage')}")
        return state
    try:
        pre = preempted(server, run["job"]) if job["status"] == "cancelled" else None
    except (urllib.error.URLError, OSError) as e:  # HTTPError included
        # Preempted or cancelled by hand can't be told apart now: a retry, never a verdict.
        log(f"{run['sha'][:12]}: {kind} cancelled, box mode unreadable ({e}); next pass")
        return state
    if pre is not None:
        # Dev mode took the box: not a failure. Hardware checks go back in the queue (this
        # pass holds them while the box stays in dev mode); a quality suite is still due.
        sha = state.pop("running")["sha"]
        if kind == "hw":
            state["queued"] = {"sha": sha, "since": float(pre.get("at") or time.time())}
            state["queued"]["preempted"] = True
        save_state(state)
        log(f"{sha[:12]}: {kind} preempted by dev mode; queued again")
        return _pass(gh_, server)
    if kind == "hw":
        finish_hw(gh_, state, job)
    else:
        finish_quality(gh_, state, job)
    save_state(state)
    return state


def quality(gh_: Gh, server: Callable[..., Any], state: dict[str, Any], sha: str) -> None:
    """The non-blocking quality suite on ``sha`` (its hardware checks passed): queue mode only,
    after any ready PR head (blocking checks first), when a chip is free."""
    try:
        mode = box_mode(server)
    except (urllib.error.URLError, OSError) as e:
        log(f"{sha[:12]}: box mode unreadable ({e}); quality suite next pass")
        return
    if holds(mode):
        log(f"{sha[:12]}: quality suite waits for queue mode")
        return
    now = time.time()
    for w in mode.get("waiting") or []:
        at = w.get("reported_at")
        if w.get("runner") != RUNNER and w.get("ready") and at and now - float(at) < REPORT_STALE_S:
            log(f"{sha[:12]}: quality suite yields to {w.get('runner')} {str(w.get('sha'))[:12]}")
            return
    try:
        devices = test_devices(server("GET", "/api/devices"))
        if not devices:
            log(f"{sha[:12]}: no test device for the quality suite")
            return
        job = server(
            "POST",
            "/api/test/staging",
            {"sha": sha, "device_ids": devices, "track_dir": str(LINK / CORPUS)},
        )
    except urllib.error.HTTPError as e:
        if not (400 <= e.code < 500) or e.code in RETRY_CODES:
            log(f"{sha[:12]}: quality suite: server answered {e.code}; next pass")
            return
        detail = _detail(e)
        state["quality_done"] = sha
        post_status(
            gh_,
            sha,
            "success",
            f"non-blocking: the server refused the quality suite ({e.code}): {detail}",
            context=QUALITY_CONTEXT,
        )
        log(f"{sha[:12]}: quality suite refused ({e.code}): {detail}")
        return
    except (urllib.error.URLError, OSError) as e:
        log(f"server not answering ({e}); next pass")
        return
    state["running"] = {"sha": sha, "job": job["job_id"], "devices": devices, "kind": "quality"}
    log(f"{sha[:12]}: quality suite {job['job_id']} on {devices} (non-blocking)")


def _pass(gh_: Gh, server: Callable[..., Any]) -> dict[str, Any]:
    state = load_state()
    if "running" in state:
        return _poll(gh_, server, state)

    sha = staging_head(gh_)
    if state.get("queued", {}).get("sha") not in (None, sha) or state.get("reported") == sha:
        state.pop("queued", None)  # superseded or settled: nothing of this runner waits
        save_state(state)
    if state.get("reported") == sha:
        if quality_due(state) == sha:
            quality(gh_, server, state, sha)
            save_state(state)
        return state
    changed, why, blind = firmware_change(gh_, sha)
    if not changed:
        return skip(gh_, state, sha, why)
    built = build_state(gh_, sha)
    if built == "failed":
        post_status(gh_, sha, "failure", "the staging build or its CI failed: nothing to test")
        state["reported"] = sha
        save_state(state)
        log(f"{sha[:12]}: build failed")
        return state
    if built == "pending":
        if state.get("waiting") != sha:
            post_status(gh_, sha, "pending", "waiting for the staging build")
            state["waiting"] = sha
            save_state(state)
        return state
    if blind:
        log(f"{sha[:12]}: {why}, some where the fingerprint can't see: testing")
    else:
        same, why = same_image(gh_, sha)
        if same:
            return skip(gh_, state, sha, why)
        log(f"{sha[:12]}: {why}")
    match, why = tree_match(gh_, sha)
    if match:
        confirm(gh_, state, sha, *match)
        save_state(state)
        return state
    log(f"{sha[:12]}: {why}")
    rig = state.get("rig") or {}
    if rig.get("sha") == sha and time.time() < float(rig.get("retry_at") or 0):
        log(
            f"{sha[:12]}: rig error {rig.get('n')} in a row; retrying after {time.ctime(rig['retry_at'])}"
        )
        return state
    try:
        mode = box_mode(server)
    except (urllib.error.URLError, OSError) as e:  # HTTPError included: never a verdict
        log(f"{sha[:12]}: box mode unreadable ({e}); next pass")
        return state
    if holds(mode):
        return hold(gh_, state, sha, mode)
    # Ready for the box: it waits (reported, so the PR runner can yield to it) until submitted.
    q = state.get("queued") if (state.get("queued") or {}).get("sha") == sha else None
    q = q or {"sha": sha, "since": time.time()}
    q["ready"] = True
    state["queued"] = q
    save_state(state)
    ahead = older_elsewhere(mode, RUNNER, float(q["since"]))
    if ahead:
        log(f"{sha[:12]}: {ahead.get('runner')} {str(ahead.get('sha'))[:12]} has waited longer")
        return state
    try:
        devices = test_devices(server("GET", "/api/devices"))
        if not devices:
            if state.get("no_device") != sha:  # once per commit, not every pass
                post_status(gh_, sha, "pending", "no test device connected to the box")
                state["no_device"] = sha
                save_state(state)
            log(f"{sha[:12]}: no test device connected")
            return state
        job = server(
            "POST",
            "/api/test/hardware-checks",
            {"sha": sha, "device_ids": devices, "source": "staging", "runner": RUNNER},
        )
    except urllib.error.HTTPError as e:
        if not (400 <= e.code < 500) or e.code in RETRY_CODES:
            # Busy (a deploy or test runs), draining for a restart, or a server error: next pass.
            log(f"{sha[:12]}: server answered {e.code}; next pass")
            return state
        # Refused for good (403 not --staging, 400 not a test device, 422 bad request): a
        # failure with the server's reason, loudly, not a retry every pass forever.
        detail = _detail(e)
        post_status(
            gh_, sha, "failure", f"the server refused the hardware checks ({e.code}): {detail}"
        )
        state["reported"] = sha
        save_state(state)
        log(f"{sha[:12]}: hardware checks refused ({e.code}): {detail}")
        return state
    except (urllib.error.URLError, OSError) as e:
        log(f"server not answering ({e}); next pass")
        return state
    state["running"] = {"sha": sha, "job": job["job_id"], "devices": devices, "kind": "hw"}
    state.pop("waiting", None)
    state.pop("queued", None)
    save_state(state)
    post_status(gh_, sha, "pending", f"hardware checks on {len(devices)} test device(s)")
    log(f"{sha[:12]}: hardware checks {job['job_id']} on {devices}")
    return state


def hold(gh_: Gh, state: dict[str, Any], sha: str, mode: dict[str, Any]) -> dict[str, Any]:
    """Dev mode: submit nothing; hold sha in the box's queue with a pending status saying why.
    The status is posted when its text changes, not every pass."""
    q = state.get("queued")
    if not q or q.get("sha") != sha:
        q = {"sha": sha, "since": time.time()}
    others = [w for w in mode.get("waiting") or [] if w.get("runner") != RUNNER]
    desc = queued_description(mode, len(others) + 1)
    if q.get("posted") != desc:
        post_status(gh_, sha, "pending", desc)
        q["posted"] = desc
        log(f"{sha[:12]}: {desc}")
    state["queued"] = q
    save_state(state)
    return state


def retest(gh_: Gh = gh) -> str:
    """Forget the staging head's verdict (a rig flake that ended in one): the next pass checks
    it again (or confirms it from its PR head) and runs its quality suite again."""
    sha = staging_head(gh_)
    state = load_state()
    if (state.get("running") or {}).get("sha") == sha:
        return f"{sha[:12]}: a job on it is running now; nothing to do"
    for key in ("reported", "hw_passed", "quality_done", "waiting", "no_device"):
        if state.get(key) == sha:
            state.pop(key)
    if (state.get("rig") or {}).get("sha") == sha:
        state.pop("rig")
    save_state(state)
    return f"{sha[:12]}: forgotten; the next pass checks it again"


WRITES = {"POST", "PATCH", "PUT", "DELETE"}


def dry_gh(gh_: Gh) -> Gh:
    """gh that reads, and logs every write instead of making it (--dry-run)."""

    def call(*args: str, stdin: str | None = None) -> str:
        method = args[args.index("-X") + 1] if "-X" in args else None
        if method in WRITES or args[:2] in (("pr", "create"), ("pr", "merge"), ("pr", "comment")):
            fields = [a for a in args if "=" in a and not a.startswith("repos/")]
            log(f"DRY RUN would: gh {' '.join(args[:5])} {' '.join(fields)}".rstrip())
            if args[:2] == ("pr", "create"):
                return f"https://github.com/{GH_REPO}/pull/0\n"
            return "{}" if args[0] == "api" else ""
        return gh_(*args, stdin=stdin)

    return call


def dry_server(server: Callable[..., Any]) -> Callable[..., Any]:
    """The server for reads only; a write is logged (--dry-run)."""

    def call(method: str, path: str, body: Any = None) -> Any:
        if method != "GET":
            log(f"DRY RUN would: {method} {path} {json.dumps(body) if body else ''}".rstrip())
            return {"job_id": "dry-run"}
        return server(method, path, body)

    return call


def main(argv: list[str] | None = None) -> int:
    global STATE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--status", action="store_true", help="print the state and exit")
    ap.add_argument("--retest", action="store_true", help="check the staging head again")
    ap.add_argument(
        "--dry-run", action="store_true", help="show what a pass would post; post nothing"
    )
    args = ap.parse_args(argv)
    if args.status:
        print(json.dumps(load_state(), indent=2))
        return 0
    if args.retest:
        log(retest())
        return 0
    try:
        host_setting("BLINKY_TESTBOX_URL")  # before any work, not after a 20-minute suite
    except RuntimeError as e:
        log(f"ERROR: {e}")
        return 2
    if args.dry_run:
        import tempfile

        copy = Path(tempfile.mkdtemp()) / "staging.json"
        copy.write_text(json.dumps(load_state()))
        STATE = copy  # the real state is never touched
        state = _pass(dry_gh(gh), dry_server(http))
        print(json.dumps(state, indent=2))
        return 0
    run_once()
    return 0


if __name__ == "__main__":
    sys.exit(main())
