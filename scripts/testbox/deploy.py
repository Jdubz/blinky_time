#!/usr/bin/env python3
"""Deploy blinky-server on the test box from master, once CI and review have passed.

Run by blinky-deploy.timer (every 2 min) from the deploy worktree (~/blinky-deploy), which is
the only tree blinky-server.service runs from. The dev checkout (~/blinky_time) is never touched,
so branch work there can't reach the running server. One pass:

1. Fetch master. Nothing new: done.
2. Gate the new commit (`gate()`):
   - it must be the merge of a PR into master;
   - every check run on the master commit has completed green, including `Blinky Server Tests`;
   - the PR's head passed `claude-review`.
   Anything still running: wait for the next pass. Anything red: refuse that commit for good.
3. Wait while the server is busy: a flash, a fleet deploy or a test job. A restart mid-flash can
   leave a device in its bootloader.
4. Check out the commit and reinstall what changed: the venv when blinky-server's pyproject.toml
   changed, the web console when blinky-console/ did. Restart. If the server isn't healthy
   within HEALTH_TIMEOUT_S, roll back to the previous commit and refuse this one.

State is in ~/.local/share/blinky-server/deploy.json; the log goes to the journal
(`journalctl -u blinky-deploy`). `deploy.py --status` prints the state; `--dry-run` gates and
reports without deploying. Stdlib only: it runs on the system python.
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
REPO = Path(__file__).resolve().parents[2]
SERVER = REPO / "blinky-server"
SERVICE = "blinky-server.service"
BASE_URL = "http://127.0.0.1:8420"
STATE = (
    Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    / "blinky-server"
    / "deploy.json"
)

# The checks that must have run, not just "everything that ran passed": a workflow that never
# started reports nothing, which must not read as green.
REQUIRED_ON_MASTER = ("Blinky Server Tests",)
REQUIRED_ON_PR_HEAD = ("claude-review",)
GREEN = {"success", "skipped", "neutral"}

# A required check that hasn't appeared this long after the commit isn't coming.
MISSING_CHECK_GIVE_UP_S = 6 * 3600
HEALTH_TIMEOUT_S = 90

Fetch = Callable[[str], Any]


def log(msg: str) -> None:
    print(msg, flush=True)


def run(*cmd: str, cwd: Path = REPO, timeout: float = 900) -> str:
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=False)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} -> {r.returncode}: {(r.stderr or r.stdout)[-2000:]}")
    return r.stdout


def gh_api(path: str) -> Any:
    return json.loads(run("gh", "api", path, timeout=60))


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
    commit = fetch(f"repos/{GH_REPO}/commits/{sha}")
    age = now - calendar.timegm(
        time.strptime(commit["commit"]["committer"]["date"], "%Y-%m-%dT%H:%M:%SZ")
    )
    for ref, required, where in (
        (sha, REQUIRED_ON_MASTER, "master"),
        (pr["head"]["sha"], REQUIRED_ON_PR_HEAD, f"PR #{pr['number']}"),
    ):
        runs = fetch(f"repos/{GH_REPO}/commits/{ref}/check-runs?per_page=100")["check_runs"]
        verdict = checks_verdict(runs, required, age, where)
        if verdict[0] != "deploy":
            return verdict
    return "deploy", f"PR #{pr['number']}: CI and review green"


def api_key() -> str:
    key = os.environ.get("BLINKY_API_KEY")
    if not key:
        key = (Path.home() / ".blinky-api-key").read_text().strip()
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


def load_state() -> dict[str, Any]:
    if STATE.exists():
        state: dict[str, Any] = json.loads(STATE.read_text())
        return state
    return {"deployed": None, "refused": {}, "history": []}


def save_state(state: dict[str, Any]) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    state["history"] = state["history"][-50:]
    tmp = STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n")
    tmp.replace(STATE)


def changed(old: str, new: str) -> list[str]:
    return run("git", "diff", "--name-only", old, new).split()


def install(sha: str, files: list[str] | None) -> None:
    """Check out `sha` and rebuild what `files` (None: everything) touches."""
    # --force: server-side compiles regenerate blinky-things/Version.h in this tree.
    run("git", "checkout", "--force", "--quiet", "--detach", sha)
    py = SERVER / "venv" / "bin" / "python"
    if files is None or "blinky-server/pyproject.toml" in files or not py.exists():
        if not py.exists():
            run("python3", "-m", "venv", str(SERVER / "venv"))
        run(str(py), "-m", "pip", "install", "-q", "-e", f"{SERVER}[firmware,ble]")
    if (
        files is None
        or any(f.startswith("blinky-console/") for f in files)
        or not (SERVER / "web" / "index.html").exists()
    ):
        console = REPO / "blinky-console"
        run("npm", "ci", "--no-audit", "--no-fund", "--loglevel=error", cwd=console)
        run("npm", "run", "build", "--silent", cwd=console)


def restart_healthy() -> str | None:
    """Restart the server; None when it is up, else why not."""
    try:
        run("sudo", "-n", "systemctl", "restart", SERVICE, timeout=180)
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


def deploy_once(dry_run: bool = False) -> int:
    state = load_state()
    run("git", "fetch", "--quiet", "origin", BRANCH, timeout=300)
    target = run("git", "rev-parse", f"origin/{BRANCH}").strip()
    current = run("git", "rev-parse", "HEAD").strip()
    if state["deployed"] is None:
        state["deployed"] = current
    if not dry_run and (target == state["deployed"] or target in state["refused"]):
        return 0
    verdict, why = gate(target, gh_api, time.time())
    log(f"{target[:12]}: {verdict} ({why})")
    if dry_run or verdict == "wait":
        return 0
    if verdict == "reject":
        state["refused"][target] = why
        save_state(state)
        return 0
    doing = busy()
    if doing:
        log(f"{target[:12]}: waiting, the server is busy ({doing})")
        return 0

    prev = state["deployed"]
    t0 = time.time()
    try:
        install(target, changed(prev, target))
        failure = restart_healthy()
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        failure = str(e)
    entry = {"sha": target, "from": prev, "at": time.time(), "seconds": round(time.time() - t0)}
    if failure is None:
        state["deployed"] = target
        state["history"].append({**entry, "result": "deployed"})
        save_state(state)
        log(f"{target[:12]}: deployed in {entry['seconds']} s (was {prev[:12]})")
        return 0

    log(f"{target[:12]}: FAILED ({failure}); rolling back to {prev[:12]}")
    state["refused"][target] = f"deploy failed: {failure}"
    state["history"].append({**entry, "result": "rolled back", "error": failure})
    save_state(state)
    try:
        install(prev, changed(prev, target))
        back = restart_healthy()
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        back = str(e)
    if back:
        log(f"ROLLBACK FAILED too ({back}): the server is down; see journalctl -u blinky-server")
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="gate the latest master; change nothing")
    ap.add_argument("--status", action="store_true", help="print the deploy state")
    a = ap.parse_args(argv)
    if a.status:
        print(json.dumps(load_state(), indent=2))
        return 0
    return deploy_once(a.dry_run)


if __name__ == "__main__":
    sys.exit(main())
