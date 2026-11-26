#!/usr/bin/env python3
"""The build host's pull deployer: deploy blinky_time services from master once CI and review pass.

The build host runs services from this public repo (services.json: today the labels service). No
GitHub runner ever does that (docs/DEV_LOOP.md, "Trust": public-repo jobs never run on the owner's
machines), so the host pulls instead, like the test box's deployer (scripts/testbox/deploy.py,
whose gate checks this reuses). blinky-build-host-deploy.timer runs one pass every 2 minutes:

1. Fetch master. A service is due when master's head changes any of its paths since the commit it
   runs (none recorded yet: due).
2. Gate the head (`gate()`): it must be the merge of a PR into master; every check run on it has
   completed green, including the service's `required` ones; the PR's head has a clean Claude
   review (the workflow's sha-stamped verdict says "No issues found"). Running: wait for the next
   pass. Red or findings: refuse the commit (recorded; `--accept SHA` deploys it anyway).
3. Check the commit out in this clone (detached; the clone is the deployer's own, never a dev
   tree) and run the service's deploy script. The script builds, restarts and health-checks; its
   persistent state (volumes, firewall) is untouched by a redeploy.
4. The script fails: check out the commit the service ran before and run its script again (roll
   back), and refuse the new commit.

State: ~/.local/share/blinky-build-host/deploy.json. Log: the journal (`journalctl -u
blinky-build-host-deploy`), and Loki when LOKI_PUSH_URL is set (EnvironmentFile). `--status`
prints the state; `--dry-run` gates and changes nothing. Stdlib only (the system python).
"""

from __future__ import annotations

import argparse
import calendar
import importlib.util
import json
import os
import subprocess
import sys
import time
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
GH_REPO = "Jdubz/blinky_time"
BRANCH = "master"
STATE = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "blinky-build-host" / "deploy.json"
DEPLOY_TIMEOUT_S = 40 * 60

# The test box's gate checks: the same rules, one implementation.
_spec = importlib.util.spec_from_file_location("testbox_deploy", REPO / "scripts" / "testbox" / "deploy.py")
box = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(box)

Fetch = Callable[[str], Any]


def log(event: str, **fields: Any) -> None:
    rec = {"event": event, **fields}
    line = json.dumps(rec, separators=(",", ":"))
    print(line, flush=True)
    url = os.environ.get("LOKI_PUSH_URL")
    if url:
        body = {"streams": [{"stream": {"service": "blinky-build-host-deploy", "env": "prod"},
                             "values": [[str(time.time_ns()), line]]}]}
        try:
            req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST",
                                         headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).read()
        except OSError as e:
            print(json.dumps({"event": "loki_push_failed", "error": str(e)[:200]}), flush=True)


def git(*args: str, timeout: float = 300) -> str:
    r = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()[-500:]}")
    return r.stdout


def gate(sha: str, required: tuple[str, ...], fetch: Fetch, now: float) -> tuple[str, str]:
    """("deploy" | "wait" | "reject", why) for a commit on master (the box's rules, without its
    staging path: services here are never merged from staging)."""
    commit = fetch(f"repos/{GH_REPO}/commits/{sha}")
    age = now - calendar.timegm(time.strptime(commit["commit"]["committer"]["date"], "%Y-%m-%dT%H:%M:%SZ"))
    pulls = fetch(f"repos/{GH_REPO}/commits/{sha}/pulls")
    pr = next((p for p in pulls if p.get("merged_at") and p["base"]["ref"] == BRANCH
               and p.get("merge_commit_sha") == sha), None)
    if pr is None:
        if age > box.MISSING_CHECK_GIVE_UP_S:
            return "reject", "not the merge of a PR into master"
        return "wait", "not linked to a merged PR (yet)"
    where = f"PR #{pr['number']}"
    head = pr["head"]["sha"]
    runs = fetch(f"repos/{GH_REPO}/commits/{sha}/check-runs?per_page=100")["check_runs"]
    verdict = box.checks_verdict(runs, required, age, "master")
    if verdict[0] != "deploy":
        return verdict
    runs = fetch(f"repos/{GH_REPO}/commits/{head}/check-runs?per_page=100")["check_runs"]
    verdict = box.checks_verdict(runs, (box.REVIEW_CHECK,), age, where)
    if verdict[0] != "deploy":
        return verdict
    comments = fetch(f"repos/{GH_REPO}/issues/{pr['number']}/comments?per_page=100")
    verdict = box.review_verdict(comments, head, where)
    if verdict[0] != "deploy":
        return verdict
    return "deploy", f"{where}: CI green, review clean"


def changes(old: str | None, new: str, paths: list[str]) -> list[str]:
    if old is None:
        return ["(first deploy)"]
    return [f for f in git("diff", "--name-only", "--no-renames", old, new, "--", *paths).splitlines() if f]


def run_deploy(svc: dict[str, Any], sha: str, runner: Callable[[list[str]], int]) -> bool:
    git("checkout", "--quiet", "--detach", sha)
    t0 = time.monotonic()
    code = runner([str(REPO / svc["deploy"][0]), *svc["deploy"][1:]])
    log("deploy_ran", service=svc["name"], sha=sha[:12], code=code, seconds=round(time.monotonic() - t0))
    return code == 0


def default_runner(cmd: list[str]) -> int:
    try:
        return subprocess.run(cmd, cwd=REPO, timeout=DEPLOY_TIMEOUT_S).returncode
    except subprocess.TimeoutExpired:
        return 124


def load_state() -> dict[str, Any]:
    return json.loads(STATE.read_text()) if STATE.is_file() else {}


def save_state(state: dict[str, Any]) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=1))
    tmp.replace(STATE)


def deploy_pass(services: list[dict[str, Any]], state: dict[str, Any], fetch: Fetch,
                runner: Callable[[list[str]], int] = default_runner, now: float | None = None,
                dry_run: bool = False, accept: str | None = None) -> dict[str, Any]:
    git("fetch", "--quiet", "--no-recurse-submodules", "origin", BRANCH)
    head = git("rev-parse", f"refs/remotes/origin/{BRANCH}").strip()
    now = time.time() if now is None else now
    for svc in services:
        st = state.setdefault(svc["name"], {"deployed": None, "refused": {}})
        if head == st["deployed"] or (head in st["refused"] and head != accept):
            continue
        changed = changes(st["deployed"], head, svc["paths"])
        if not changed:
            continue  # master moved, but nothing of this service did
        verdict, why = ("deploy", "accepted by the owner") if head == accept else gate(
            head, tuple(svc.get("required", ())), fetch, now)
        if verdict == "wait":
            log("waiting", service=svc["name"], sha=head[:12], why=why)
            continue
        if verdict == "reject":
            st["refused"][head] = why
            log("refused", service=svc["name"], sha=head[:12], why=why)
            continue
        log("deploying", service=svc["name"], sha=head[:12], why=why, changed=changed[:10], dry_run=dry_run)
        if dry_run:
            continue
        if run_deploy(svc, head, runner):
            st["deployed"] = head
            st["deployed_at"] = now
            log("deployed", service=svc["name"], sha=head[:12])
            continue
        st["refused"][head] = "deploy script failed"
        if st["deployed"]:
            ok = run_deploy(svc, st["deployed"], runner)
            log("rolled_back" if ok else "rollback_failed", service=svc["name"], to=st["deployed"][:12])
        else:
            log("deploy_failed", service=svc["name"], sha=head[:12], note="nothing to roll back to")
    return state


def gh_fetch(path: str) -> Any:
    return box.gh_api(path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--status", action="store_true", help="print the state and exit")
    ap.add_argument("--dry-run", action="store_true", help="gate master's head; change nothing")
    ap.add_argument("--accept", metavar="SHA", help="deploy this refused master commit anyway (owner)")
    ap.add_argument("--services", type=Path, default=HERE / "services.json")
    args = ap.parse_args(argv)
    state = load_state()
    if args.status:
        print(json.dumps(state, indent=1))
        return 0
    services = json.loads(args.services.read_text())["services"]
    state = deploy_pass(services, state, gh_fetch, dry_run=args.dry_run, accept=args.accept)
    if not args.dry_run:
        save_state(state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
