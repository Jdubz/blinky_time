#!/usr/bin/env python3
"""
release_promote.py - promote a gated candidate to stable, or roll back
(docs/RELEASE_PROCESS.md).

  python tools/release_promote.py b214            # candidate -> stable
  python tools/release_promote.py b212 --rollback # make an older stable the latest again

Promote refuses unless the release carries a passing gate-<device>.json whose
version matches the release manifest. It then has CI sign the release for
OTA (sign-release.yml: the signing key is a GitHub secret and is only ever
used after a passing gate), checks the signature itself against the key the
firmware trusts, tags the commit stable-b<N>, marks the release a full
(non-pre-) release and GitHub's "Latest" (what deploy tools install by
default), and redeploys the console's firmware mirror. Rollback only re-marks an
already-promoted stable release as Latest; nothing is rebuilt.
"""

import argparse
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import firmware_release  # noqa: E402
from firmware_release import ReleaseError, gh, repo_slug, tag_exists  # noqa: E402


def sign_in_ci(tag):
    """Dispatch sign-release.yml for `tag`, wait for it, then verify the result locally."""
    import datetime
    import subprocess
    import time
    started = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(seconds=5)
    gh("workflow", "run", "sign-release.yml", "--ref", "master", "-f", f"tag={tag}")
    run_id = None
    for _ in range(30):
        time.sleep(4)
        runs = json.loads(gh("run", "list", "--workflow", "sign-release.yml", "--limit", "5",
                             "--json", "databaseId,createdAt,event"))
        fresh = [r for r in runs if r["event"] == "workflow_dispatch" and
                 datetime.datetime.fromisoformat(r["createdAt"].replace("Z", "+00:00")) >= started]
        if fresh:
            run_id = fresh[0]["databaseId"]
            break
    if run_id is None:
        raise ReleaseError("sign-release.yml was dispatched but no run appeared; check the Actions tab")
    print(f"  signing {tag} in CI (run {run_id})...")
    if subprocess.run(["gh", "run", "watch", str(run_id), "--exit-status"], cwd=REPO,
                      capture_output=True, text=True).returncode != 0:
        raise ReleaseError(f"sign-release.yml run {run_id} failed: gh run view {run_id} --log-failed")
    f = firmware_release.verify_signed(tag)
    print(f"  signed and verified: build {f['build']}, {f['size']} bytes")


def redeploy_console(what):
    """Re-mirror stable releases for the web flasher (docs/WEB_FLASHER.md).

    The console deploy also listens for tag and release events, but GitHub
    runs those with the workflow file of the *tagged* commit, which for an
    older release may predate the trigger. workflow_dispatch always runs
    master's workflow, so dispatch it explicitly. The release change above
    has already happened; a failed dispatch is reported loudly with the
    manual command.
    """
    try:
        gh("workflow", "run", "blinky-console-deploy.yml", "--ref", "master")
    except ReleaseError as e:
        raise ReleaseError(f"{what} succeeded, but the console redeploy could not be dispatched ({e}). "
                           "The web flasher still offers the old default: run "
                           "`gh workflow run blinky-console-deploy.yml --ref master`")
    print("  console redeploy dispatched (web flasher mirror)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", help="release tag, e.g. b214")
    ap.add_argument("--rollback", action="store_true",
                    help="re-mark an already-promoted stable release as Latest")
    args = ap.parse_args()
    try:
        n = firmware_release.TAG_RE.match(args.tag)
        if not n:
            raise ReleaseError(f"not a release tag: {args.tag}")
        stable_tag = f"stable-{args.tag}"
        info = firmware_release.release_info(args.tag)

        if args.rollback:
            if info["isPrerelease"] or not tag_exists(stable_tag):
                raise ReleaseError(f"{args.tag} was never promoted; rollback only targets stable releases")
            gh("release", "edit", args.tag, "--latest")
            print(f"ROLLED BACK: {args.tag} is the latest stable again (deploy tools install it by default)")
            redeploy_console("Rollback")
            return 0

        rel = firmware_release.download(args.tag, Path(tempfile.gettempdir()) / "blinky-promote-assets")
        passing = firmware_release.gate_records(args.tag, rel["manifest"])
        sign_in_ci(args.tag)
        # Tag first, then mark stable + Latest. Each step fires a console
        # redeploy whose firmware mirror re-reads "stable" (firmware_release.py);
        # in this order no run ever sees a Latest release without its stable
        # tag, which would publish `latest: null` to the web flasher. The tag
        # alone changes nothing for deploys: is_stable() also needs the
        # release to be a full release.
        if not tag_exists(stable_tag):
            gh("api", f"repos/{repo_slug()}/git/refs", "-f", f"ref=refs/tags/{stable_tag}",
               "-f", f"sha={rel['manifest']['git_sha']}")
        gh("release", "edit", args.tag, "--prerelease=false", "--latest", "--title", f"{args.tag} (stable)")
        print(f"PROMOTED: {args.tag} ({rel['manifest']['version']}) is stable and Latest; "
              f"gate: {', '.join(passing)}; tag {stable_tag}")
        redeploy_console("Promotion")
        return 0
    except ReleaseError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
