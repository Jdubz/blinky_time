#!/usr/bin/env python3
"""Deploy blinky-server on the test box from master, once CI and review have passed.

blinky-server.service runs from ~/blinky-deploy, a symlink to one of two worktrees of the dev
clone, ~/blinky-deploy-a and ~/blinky-deploy-b, each detached at a master commit. The dev clone
(~/blinky_time) is never what the server runs, so branch work there can't reach it.
blinky-deploy.timer runs this every 2 minutes, from the live slot. One pass:

1. Fetch master. Nothing new: done.
2. Gate the new commit (`gate()`):
   - it must be the merge of a PR into master;
   - every check run on the master commit has completed green, including `Blinky Server Tests`;
   - the PR's head has a clean Claude review: the workflow's sha-stamped verdict comment says
     "No issues found" (the check run alone is green even when the review found issues).
   Still running: wait for the next pass. Red, or a review with findings: refuse the commit.
   `--accept SHA` deploys a refused commit anyway (an owner's call, recorded in the state).
3. Prepare the OTHER slot: check out the commit, reinstall the venv if blinky-server's
   pyproject.toml differs from what that slot last installed, rebuild the console if
   blinky-console/ does. The running server's tree is never touched.
4. Wait while the server is busy (a flash, a fleet deploy, a test job): a restart mid-flash can
   leave a device in its bootloader. The check runs right before the restart, after the slow
   preparation, and a busy server leaves the prepared slot for the next pass.
5. Point ~/blinky-deploy at the new slot and restart. Not answering /api/devices within
   HEALTH_TIMEOUT_S: point it back, restart, and refuse the commit.

State is in ~/.local/share/blinky-server/deploy.json; the log goes to the journal
(`journalctl -u blinky-deploy`). `deploy.py --status` prints the state; `--dry-run` gates the
latest master and changes nothing. Stdlib only: it runs on the system python.
"""

from __future__ import annotations

import argparse
import calendar
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any

GH_REPO = "Jdubz/blinky_time"
BRANCH = "master"
HOME = Path.home()
LINK = HOME / "blinky-deploy"
SLOTS = (HOME / "blinky-deploy-a", HOME / "blinky-deploy-b")
DEV_CLONE = HOME / "blinky_time"
SERVICE = "blinky-server.service"
BASE_URL = "http://127.0.0.1:8420"
STATE = (
    Path(os.environ.get("XDG_DATA_HOME") or HOME / ".local/share") / "blinky-server" / "deploy.json"
)
MARKER = ".deploy-installed.json"  # in each slot: what its venv and console were built from

# The checks that must have run, not just "everything that ran passed": a workflow that never
# started reports nothing, which must not read as green.
REQUIRED_ON_MASTER = ("Blinky Server Tests",)
REVIEW_CHECK = "claude-review"
# The verdict comment's stable first line (scripts/post-review-verdict.sh), posted by the
# workflow. Anyone can comment on a public repo's PR, so only the bot's comment counts.
REVIEW_HEADER = "## Claude review — "
REVIEW_AUTHOR = "github-actions[bot]"
REVIEW_CLEAN = "No issues found."
GREEN = {"success", "skipped", "neutral"}

# A required check that hasn't appeared this long after the commit isn't coming.
MISSING_CHECK_GIVE_UP_S = 6 * 3600
HEALTH_TIMEOUT_S = 90

Fetch = Callable[[str], Any]


def log(msg: str) -> None:
    print(msg, flush=True)


def run(*cmd: str, cwd: Path = LINK, timeout: float = 900) -> str:
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=False)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} -> {r.returncode}: {(r.stderr or r.stdout)[-2000:]}")
    return r.stdout


def gh_api(path: str) -> Any:
    return json.loads(run("gh", "api", path, cwd=HOME, timeout=60))


# -- the gate ----------------------------------------------------------------------------------


def checks_verdict(
    runs: list[dict[str, Any]], required: tuple[str, ...], age_s: float, where: str
) -> tuple[str, str]:
    bad = [
        f"{r['name']}={r['conclusion']}"
        for r in runs
        if r["status"] == "completed" and r["conclusion"] not in GREEN
    ]
    if bad:
        return "reject", f"{where}: {', '.join(bad)}"
    pending = [r["name"] for r in runs if r["status"] != "completed"]
    if pending:
        return "wait", f"{where}: running {', '.join(pending)}"
    names = {r["name"] for r in runs}
    missing = [n for n in required if n not in names]
    if missing:
        if age_s > MISSING_CHECK_GIVE_UP_S:
            return "reject", f"{where}: {', '.join(missing)} never ran"
        return "wait", f"{where}: {', '.join(missing)} hasn't started"
    return "deploy", f"{where}: {len(runs)} checks green"


def review_verdict(comments: list[dict[str, Any]], head: str, where: str) -> tuple[str, str]:
    """The review of the PR's last commit, from the workflow's verdict comment. The check run
    passing only proves the job ran: the review step is continue-on-error, and the workflow
    posts no verdict at all when the model produced none."""
    mine = [
        c
        for c in comments
        if (c.get("user") or {}).get("login") == REVIEW_AUTHOR
        and (c.get("body") or "").startswith(REVIEW_HEADER + head)
    ]
    if not mine:
        return "reject", f"{where}: no review verdict for {head[:12]} (the review produced none)"
    body = mine[-1]["body"].split("\n")
    headline = next((ln for ln in body[1:] if ln.strip()), "")
    if not headline.startswith(REVIEW_CLEAN):
        return "reject", f"{where}: review of {head[:12]}: {headline}"
    return "deploy", f"{where}: review clean"


def gate(sha: str, fetch: Fetch, now: float) -> tuple[str, str]:
    """("deploy" | "wait" | "reject", why) for a commit on master."""
    pulls = fetch(f"repos/{GH_REPO}/commits/{sha}/pulls")
    pr = next(
        (
            p
            for p in pulls
            if p.get("merged_at")
            and p["base"]["ref"] == BRANCH
            and p.get("merge_commit_sha") == sha
        ),
        None,
    )
    if pr is None:
        return "reject", "not the merge of a PR into master"
    where = f"PR #{pr['number']}"
    head = pr["head"]["sha"]
    commit = fetch(f"repos/{GH_REPO}/commits/{sha}")
    date = commit["commit"]["committer"]["date"]
    age = now - calendar.timegm(time.strptime(date, "%Y-%m-%dT%H:%M:%SZ"))
    runs = fetch(f"repos/{GH_REPO}/commits/{sha}/check-runs?per_page=100")["check_runs"]
    verdict = checks_verdict(runs, REQUIRED_ON_MASTER, age, "master")
    if verdict[0] != "deploy":
        return verdict
    runs = fetch(f"repos/{GH_REPO}/commits/{head}/check-runs?per_page=100")["check_runs"]
    verdict = checks_verdict(runs, (REVIEW_CHECK,), age, where)
    if verdict[0] != "deploy":
        return verdict
    comments = fetch(f"repos/{GH_REPO}/issues/{pr['number']}/comments?per_page=100")
    verdict = review_verdict(comments, head, where)
    if verdict[0] != "deploy":
        return verdict
    return "deploy", f"{where}: CI green, review clean"


# -- the server --------------------------------------------------------------------------------


def api_key() -> str:
    key = os.environ.get("BLINKY_API_KEY")
    if not key:
        key = (HOME / ".blinky-api-key").read_text().strip()
    return key


def http_get(path: str, timeout: float = 5) -> Any:
    req = urllib.request.Request(BASE_URL + path, headers={"X-API-Key": api_key()})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def busy(get: Callable[[str], Any] = http_get) -> str | None:
    """What the server is doing that a restart would break, or None. A server that doesn't
    answer is doing nothing: deploying is how it gets fixed."""
    try:
        flashes = get("/api/flash-jobs?active=true")["jobs"]
        jobs = get("/api/fleet/jobs") + get("/api/test/jobs")
    except (urllib.error.URLError, ConnectionError, TimeoutError):
        return None
    if flashes:
        return f"{len(flashes)} flash job(s) active"
    live = [j for j in jobs if j.get("status") in ("pending", "running")]
    if live:
        return f"job(s) running: {', '.join(str(j.get('type') or j.get('id')) for j in live)}"
    return None


def restart_healthy() -> str | None:
    """Restart the server; None when it is up, else why not."""
    try:
        run("sudo", "-n", "systemctl", "restart", SERVICE, cwd=HOME, timeout=180)
    except RuntimeError as e:
        return str(e)
    deadline = time.monotonic() + HEALTH_TIMEOUT_S
    while time.monotonic() < deadline:
        try:
            http_get("/api/devices")
            return None
        except (urllib.error.URLError, ConnectionError, TimeoutError, OSError):
            time.sleep(2)
    return f"{SERVICE} not answering {BASE_URL}/api/devices after {HEALTH_TIMEOUT_S} s"


# -- the slots ---------------------------------------------------------------------------------


def live_slot() -> Path:
    return LINK.resolve()


def other_slot(live: Path) -> Path:
    return SLOTS[1] if live == SLOTS[0] else SLOTS[0]


def point_at(slot: Path) -> None:
    """Swap ~/blinky-deploy to `slot` atomically: the server's unit names the link."""
    tmp = LINK.with_name(LINK.name + ".new")
    tmp.unlink(missing_ok=True)
    tmp.symlink_to(slot)
    os.replace(tmp, LINK)


def prepare(slot: Path, sha: str) -> None:
    """Make `slot` a ready tree at `sha`: venv and console built from it."""
    if not (slot / ".git").exists():
        run("git", "worktree", "add", "--quiet", "--detach", str(slot), sha, cwd=DEV_CLONE)
    # --force: server-side compiles regenerate blinky-things/Version.h in a slot's tree.
    run("git", "checkout", "--force", "--quiet", "--detach", sha, cwd=slot)
    if not (slot / ".arduino").exists():  # the pinned toolchain, shared (1.3 GB)
        (slot / ".arduino").symlink_to(DEV_CLONE / ".arduino")
    want = {
        "pyproject": run(
            "git", "rev-parse", f"{sha}:blinky-server/pyproject.toml", cwd=slot
        ).strip(),
        "console": run("git", "rev-parse", f"{sha}:blinky-console", cwd=slot).strip(),
    }
    marker = slot / MARKER
    have = json.loads(marker.read_text()) if marker.exists() else {}
    server = slot / "blinky-server"
    py = server / "venv" / "bin" / "python"
    if have.get("pyproject") != want["pyproject"] or not py.exists():
        if not py.exists():
            run("python3", "-m", "venv", str(server / "venv"), cwd=slot)
        run(str(py), "-m", "pip", "install", "-q", "-e", f"{server}[firmware,ble]", cwd=slot)
    if have.get("console") != want["console"] or not (server / "web" / "index.html").exists():
        console = slot / "blinky-console"
        run("npm", "ci", "--no-audit", "--no-fund", "--loglevel=error", cwd=console)
        run("npm", "run", "build", "--silent", cwd=console)
    marker.write_text(json.dumps(want) + "\n")


# -- one pass ----------------------------------------------------------------------------------


def load_state() -> dict[str, Any]:
    if STATE.exists():
        state: dict[str, Any] = json.loads(STATE.read_text())
        return state
    return {"deployed": None, "refused": {}, "accepted": {}, "history": []}


def save_state(state: dict[str, Any]) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    state["history"] = state["history"][-50:]
    tmp = STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n")
    tmp.replace(STATE)


def deploy_once(dry_run: bool = False) -> int:
    state = load_state()
    state.setdefault("accepted", {})
    live = live_slot()
    run("git", "fetch", "--quiet", "origin", BRANCH, cwd=live, timeout=300)
    target = run("git", "rev-parse", f"origin/{BRANCH}", cwd=live).strip()
    current = run("git", "rev-parse", "HEAD", cwd=live).strip()
    state["deployed"] = current
    if not dry_run and (target == current or target in state["refused"]):
        return 0
    if target in state["accepted"]:
        verdict, why = "deploy", f"accepted by the owner: {state['accepted'][target]}"
    else:
        verdict, why = gate(target, gh_api, time.time())
    log(f"{target[:12]}: {verdict} ({why})")
    if dry_run or verdict == "wait":
        return 0
    if verdict == "reject":
        state["refused"][target] = why
        save_state(state)
        log(f"{target[:12]}: refused; `deploy.py --accept {target[:12]} REASON` overrides")
        return 0

    new = other_slot(live)
    t0 = time.time()
    try:
        prepare(new, target)
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        log(f"{target[:12]}: preparing {new.name} failed, the server is untouched: {e}")
        state["refused"][target] = f"prepare failed: {e}"
        save_state(state)
        return 1
    doing = busy()  # after the slow part: anything that started meanwhile counts
    if doing:
        log(f"{target[:12]}: prepared in {new.name}; waiting, the server is busy ({doing})")
        return 0

    point_at(new)
    failure = restart_healthy()
    entry = {
        "sha": target,
        "from": current,
        "slot": new.name,
        "at": time.time(),
        "seconds": round(time.time() - t0),
    }
    if failure is None:
        state["deployed"] = target
        state["history"].append({**entry, "result": "deployed"})
        save_state(state)
        log(f"{target[:12]}: deployed from {new.name} in {entry['seconds']} s (was {current[:12]})")
        return 0

    log(f"{target[:12]}: FAILED ({failure}); rolling back to {current[:12]} in {live.name}")
    point_at(live)
    back = restart_healthy()
    state["refused"][target] = f"deploy failed: {failure}"
    state["history"].append({**entry, "result": "rolled back", "error": failure})
    save_state(state)
    if back:
        log(f"ROLLBACK FAILED too ({back}): the server is down; see journalctl -u blinky-server")
    return 1


def accept(sha_prefix: str, reason: str) -> int:
    state = load_state()
    state.setdefault("accepted", {})
    full = run("git", "rev-parse", "--verify", f"{sha_prefix}^{{commit}}", cwd=live_slot()).strip()
    state["accepted"][full] = reason
    state["refused"].pop(full, None)
    save_state(state)
    log(f"{full[:12]}: accepted ({reason}); the next pass deploys it if it is master's head")
    return 0


def init() -> int:
    """First install (setup.sh): slot a at origin/master, and ~/blinky-deploy pointing at it."""
    if LINK.exists() or LINK.is_symlink():
        log(f"{LINK} exists (-> {LINK.resolve()}); nothing to do")
        return 0
    run("git", "fetch", "--quiet", "origin", BRANCH, cwd=DEV_CLONE, timeout=300)
    sha = run("git", "rev-parse", f"origin/{BRANCH}", cwd=DEV_CLONE).strip()
    prepare(SLOTS[0], sha)
    point_at(SLOTS[0])
    log(f"{LINK} -> {SLOTS[0].name} at {sha[:12]}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="gate the latest master; change nothing")
    ap.add_argument("--status", action="store_true", help="print the deploy state")
    ap.add_argument(
        "--accept",
        nargs=2,
        metavar=("SHA", "REASON"),
        help="deploy a commit the gate refused (recorded with the reason)",
    )
    ap.add_argument("--init", action="store_true", help="first install, from setup.sh")
    a = ap.parse_args(argv)
    if a.init:
        return init()
    if a.status:
        print(json.dumps(load_state(), indent=2))
        return 0
    if a.accept:
        return accept(*a.accept)
    return deploy_once(a.dry_run)


if __name__ == "__main__":
    sys.exit(main())
