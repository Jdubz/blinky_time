#!/usr/bin/env python3
"""Hardware-test the staging branch on the test box (#349, docs/RELEASE_PROCESS.md "Staging").

blinky-staging.timer runs this every 2 minutes, from the deploy worktree (master code, like
deploy.py). One pass:

1. A suite is in flight: poll it (GET /api/test/jobs/{id}). Finished: post the commit status
   HARDWARE_CONTEXT on its sha (success only if the report passed) and the report as a comment
   on the PR that merged it into staging. On a pass, if staging's head is still that sha, open
   the staging -> master PR if none is open and turn on its auto-merge, pinned to that sha (a
   merge commit: master's tree is the tested tree).
2. Otherwise, the staging head: already reported, nothing to do. No change under the
   firmware's paths (tools/firmware_paths.json at the head) since master: post a passing
   status saying the test was skipped, and promote. Its staging build
   (staging-build.yml, artifact staging-firmware-<sha>) failed: post a failure. Not built yet:
   a pending status. Built: submit the suite (POST /api/test/staging) for every connected test
   device (devices/test_devices.json, by configured device id) and post a pending status.

Only the newest staging commit is tested; a commit superseded while it waits is skipped (its
successor carries its changes). State is in ~/.local/share/blinky-server/staging.json; the log
goes to the journal (`journalctl -u blinky-staging`). `--status` prints the state. Stdlib only.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any

# The firmware and its staging flow live in blinky-firmware (blinky_time#357): its staging
# head is what gets tested, and its statuses and PRs are what this posts.
GH_REPO = "Jdubz/blinky-firmware"
BRANCH = "staging"
HARDWARE_CONTEXT = "Hardware test (staging)"  # deploy.py gates staging -> master merges on it
HOME = Path.home()
LINK = HOME / "blinky-deploy"
BASE_URL = "http://127.0.0.1:8420"
STATE = (
    Path(os.environ.get("XDG_DATA_HOME") or HOME / ".local/share")
    / "blinky-server"
    / "staging.json"
)
BUILD_WORKFLOW = "staging-build.yml"
# The paths that change what is built and flashed, owned by the firmware repo and read at the
# commit being judged; its release.yml skips a candidate by the same list.
FIRMWARE_PATHS = "tools/firmware_paths.json"
COMPARE_FILE_LIMIT = 300  # GitHub's compare lists at most this many files
CORPUS = "corpus/edm"  # the blinky-corpus submodule
REPORT_HEADER = "## Hardware test (staging) — "
TERMINAL = ("complete", "error", "cancelled")  # GET /api/test/jobs/{id} status (work/jobs.py)

Gh = Callable[..., str]


def log(msg: str) -> None:
    print(msg, flush=True)


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


def test_devices(devices: list[dict[str, Any]], listed: set[str]) -> list[str]:
    """Connected nRF52840s whose configured id is a listed test device."""
    return [
        d["id"]
        for d in devices
        if d.get("state") == "connected"
        and d.get("platform") == "nrf52840"
        and d.get("device_type") in listed
    ]


def listed_test_devices() -> set[str]:
    # The firmware repo's list (the firmware/ submodule): the same one its tools enforce.
    data = json.loads((LINK / "firmware" / "devices" / "test_devices.json").read_text())
    return set(data["test_devices"])


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


def firmware_change(gh_: Gh, sha: str) -> tuple[bool, str]:
    """(changed, why): whether sha changes anything under the firmware's paths since master.

    Anything that can't be judged counts as a change, so it gets the hardware test: no path
    list at that commit, a GitHub error, or a compare GitHub truncated.
    """
    try:
        meta = gh_json(gh_, "api", f"repos/{GH_REPO}/contents/{FIRMWARE_PATHS}?ref={sha}")
        paths = [str(p) for p in json.loads(base64.b64decode(meta["content"]))["paths"]]
        cmp = gh_json(gh_, "api", f"repos/{GH_REPO}/compare/master...{sha}")
    except (RuntimeError, KeyError, ValueError, TypeError) as e:
        return True, f"can't compare with master ({str(e)[:80]}): testing"
    if not paths:
        return True, f"{FIRMWARE_PATHS} lists no paths: testing"
    files = cmp.get("files")
    if not isinstance(files, list) or len(files) >= COMPARE_FILE_LIMIT:
        return True, "the compare with master is truncated: testing"

    def firmware(name: str) -> bool:
        return any(name.startswith(p) if p.endswith("/") else name == p for p in paths)

    hits = [
        n
        for f in files
        for n in (f.get("filename"), f.get("previous_filename"))
        if n and firmware(n)
    ]
    base = str(cmp.get("merge_base_commit", {}).get("sha", "master"))[:12]
    if hits:
        return True, f"{len(hits)} firmware file(s) changed since master {base}"
    return False, f"no firmware change since master {base}: hardware test skipped"


def post_status(gh_: Gh, sha: str, state: str, description: str, url: str | None = None) -> None:
    args = [
        "api",
        "-X",
        "POST",
        f"repos/{GH_REPO}/statuses/{sha}",
        "-f",
        f"state={state}",
        "-f",
        f"context={HARDWARE_CONTEXT}",
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
            f"Staging at `{sha[:12]}`: {skipped} (`{FIRMWARE_PATHS}`). Merging this to master "
            "cuts no candidate release (docs/RELEASE_PROCESS.md). "
            "Opened by the test box (scripts/testbox/staging.py)."
        )
    else:
        body = (
            f"Staging at `{sha[:12]}` passed the hardware test on the box's test chips"
            + (f": {report_url}" if report_url else ".")
            + "\n\nMerging this to master cuts a candidate release (docs/RELEASE_PROCESS.md). "
            "Opened by the test box (scripts/testbox/staging.py)."
        )
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


def render(report: dict[str, Any]) -> str:
    """The report as markdown: verdict, every check per device, drum means, timing, alerts."""
    lines = [
        f"{REPORT_HEADER}{report.get('sha', '?')}",
        "",
        (
            f"**{'PASSED' if report.get('passed') else 'FAILED'}** — "
            f"`{report.get('version', '?')}` on "
            f"{', '.join(d[:12] for d in report.get('devices', []))}"
        ),
        "",
    ]
    dep = report.get("deploy") or {}
    if not dep.get("pass"):
        lines += [f"Deploy failed at `{dep.get('failed_step')}`: {dep.get('message')}"]
        return "\n".join(lines) + "\n"
    rows = ["| device | check | | detail |", "|---|---|---|---|"]
    for section in ("health", "music", "final"):
        for dev, checks in (report.get(section) or {}).items():
            for name, c in checks.items():
                if not isinstance(c, dict) or "pass" not in c:
                    continue
                detail = ", ".join(
                    f"{k}={v}" for k, v in c.items() if k not in ("pass", "lines", "series_ms")
                )
                rows.append(f"| {dev[:12]} | {name} | {'✅' if c['pass'] else '❌'} | {detail} |")
    lines += [*rows, ""]
    val = report.get("validation") or {}
    if val.get("results"):
        by_dev: dict[str, dict[str, list[float | None]]] = {}
        for r in val["results"]:
            for dev, s in (r.get("scores") or {}).items():
                drums = s.get("drums") or {}
                for k in ("kickR50", "snareR50", "onDrum", "drumF", "loneHat"):
                    by_dev.setdefault(dev, {}).setdefault(k, []).append(drums.get(k))
        lines += [
            (
                f"Validation: {val.get('played')}/{val.get('tracks')} edm/ tracks played "
                "(reported, not gated). Means over tracks:"
            ),
            "",
            "| device | kick R@50 | snare R@50 | on-drum | drum F | lone hat |",
            "|---|---|---|---|---|---|",
        ]
        for dev, m in by_dev.items():
            vals = [_mean(m[k]) for k in ("kickR50", "snareR50", "onDrum", "drumF", "loneHat")]
            lines.append(f"| {dev[:12]} | " + " | ".join(str(v) for v in vals) + " |")
        lines.append("")
    elif val:
        lines += [f"Validation: ❌ {val.get('detail')}", ""]
    for dev, f in (report.get("final") or {}).items():
        lines.append(f"Onset-path timing ({dev[:12]}): {f.get('timing') or 'not reported'}")
    alerts = [
        (dev, a)
        for dev, f in (report.get("final") or {}).items()
        for a in (f.get("alerts") or {}).get("lines", [])
    ]
    if alerts:
        lines += ["", "Firmware alerts:", "```"] + [f"{d[:12]} {a}" for d, a in alerts[:30]]
        lines.append("```")
    return "\n".join(lines) + "\n"


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


def finish(gh_: Gh, state: dict[str, Any], job: dict[str, Any]) -> None:
    sha = state["running"]["sha"]
    report = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(report, dict) or "passed" not in report:
        report = {
            "sha": sha,
            "passed": False,
            "devices": state["running"]["devices"],
            "deploy": {"pass": False, "failed_step": "suite", "message": job.get("error")},
        }
    text = render(report)
    pr = staging_pr(gh_, sha)
    url = None
    if pr is not None:
        url = gh_("pr", "comment", str(pr), "--repo", GH_REPO, "--body", text).strip() or None
    passed = bool(report["passed"])
    post_status(
        gh_,
        sha,
        "success" if passed else "failure",
        f"passed on {len(report.get('devices', []))} test device(s)"
        if passed
        else "FAILED: see the report",
        url,
    )
    log(f"{sha[:12]}: {'passed' if passed else 'FAILED'} ({url or 'no PR to comment on'})")
    if passed:
        # Only the commit that was tested goes to master. Staging moved on during the suite:
        # the new head gets its own run, and its own promotion.
        head = staging_head(gh_)
        if head != sha:
            log(f"{sha[:12]}: passed, but staging is now at {head[:12]}; not promoting")
        else:
            number = promote(gh_, sha, url)
            log(f"{sha[:12]}: staging -> master PR #{number}, auto-merge on")
    state["reported"] = sha
    state.pop("running", None)


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
    listed: Callable[[], set[str]] = listed_test_devices,
) -> dict[str, Any]:
    state = load_state()
    if "running" in state:
        try:
            job = server("GET", f"/api/test/jobs/{state['running']['job']}")
        except urllib.error.HTTPError as e:
            if e.code != 404:
                log(f"server answered {e.code}; next pass")
                return state
            # The work store lost it: a failure, loudly, not a wait for good.
            job = {"status": "error", "error": f"suite job {state['running']['job']} not found"}
        except (urllib.error.URLError, OSError) as e:
            log(f"server not answering ({e}); next pass")
            return state
        if job.get("status") not in TERMINAL:
            log(f"{state['running']['sha'][:12]}: {job.get('progressMessage')}")
            return state
        finish(gh_, state, job)
        save_state(state)
        return state

    sha = staging_head(gh_)
    if state.get("reported") == sha:
        return state
    changed, why = firmware_change(gh_, sha)
    if not changed:
        # Nothing on the device changes: master gets it without a run (the owner, 2026-11-05).
        # Reported only once promoted, so a failed PR create or auto-merge is retried next pass.
        post_status(gh_, sha, "success", why)
        log(f"{sha[:12]}: {why}")
        number = promote(gh_, sha, None, skipped=why)
        log(f"{sha[:12]}: staging -> master PR #{number}, auto-merge on")
        state["reported"] = sha
        state.pop("waiting", None)
        save_state(state)
        return state
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
    try:
        devices = test_devices(server("GET", "/api/devices"), listed())
        if not devices:
            if state.get("no_device") != sha:  # once per commit, not every pass
                post_status(gh_, sha, "pending", "no test device connected to the box")
                state["no_device"] = sha
                save_state(state)
            log(f"{sha[:12]}: no test device connected")
            return state
        job = server(
            "POST",
            "/api/test/staging",
            {"sha": sha, "device_ids": devices, "track_dir": str(LINK / CORPUS)},
        )
    except urllib.error.HTTPError as e:
        if not (400 <= e.code < 500) or e.code in RETRY_CODES:
            # Busy (a deploy or suite runs), draining for a restart, or a server error: next pass.
            log(f"{sha[:12]}: server answered {e.code}; next pass")
            return state
        # Refused for good (403 not --staging, 400 not a test device, 422 bad request): a
        # failure with the server's reason, loudly, not a retry every pass forever.
        detail = _detail(e)
        post_status(gh_, sha, "failure", f"the server refused the suite ({e.code}): {detail}")
        state["reported"] = sha
        save_state(state)
        log(f"{sha[:12]}: suite refused ({e.code}): {detail}")
        return state
    except (urllib.error.URLError, OSError) as e:
        log(f"server not answering ({e}); next pass")
        return state
    state["running"] = {"sha": sha, "job": job["job_id"], "devices": devices}
    state.pop("waiting", None)
    save_state(state)
    post_status(gh_, sha, "pending", f"testing on {len(devices)} test device(s)")
    log(f"{sha[:12]}: suite {job['job_id']} on {devices}")
    return state


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--status", action="store_true", help="print the state and exit")
    args = ap.parse_args(argv)
    if args.status:
        print(json.dumps(load_state(), indent=2))
        return 0
    run_once()
    return 0


if __name__ == "__main__":
    sys.exit(main())
