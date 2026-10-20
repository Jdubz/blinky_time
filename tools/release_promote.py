#!/usr/bin/env python3
"""
release_promote.py - promote a gated candidate to stable, or roll back
(docs/RELEASE_PROCESS.md).

  python tools/release_promote.py b214            # candidate -> stable
  python tools/release_promote.py b212 --rollback # make an older stable the latest again

Promote refuses unless the release carries a passing gate-<device>.json whose
version matches the release manifest. Promotion marks the release as a full
(non-pre-) release, makes it GitHub's "Latest" (what deploy tools install by
default) and tags the commit stable-b<N>. Rollback only re-marks an
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
from firmware_release import ReleaseError, gh  # noqa: E402


def repo_slug():
    return json.loads(gh("repo", "view", "--json", "nameWithOwner"))["nameWithOwner"]


def tag_exists(tag):
    try:
        gh("api", f"repos/{repo_slug()}/git/ref/tags/{tag}")
        return True
    except ReleaseError:
        return False


def gate_records(tag, manifest):
    info = firmware_release.release_info(tag)
    names = [a["name"] for a in info["assets"] if a["name"].startswith("gate-") and a["name"].endswith(".json")]
    if not names:
        raise ReleaseError(f"{tag} has no gate record: run tools/release_gate.py {tag} on a test device first")
    tmp = Path(tempfile.mkdtemp(prefix="blinky-promote-"))
    records = []
    for n in names:
        gh("release", "download", tag, "--dir", str(tmp), "--pattern", n)
        rec = json.loads((tmp / n).read_text(encoding="utf-8"))
        if rec.get("version") != manifest["version"]:
            raise ReleaseError(f"{n}: gated version {rec.get('version')} != release {manifest['version']}")
        records.append((n, rec))
    passing = [n for n, r in records if r.get("passed") is True]
    if not passing:
        raise ReleaseError(f"{tag}: no passing gate record ({', '.join(names)} all failed)")
    return passing


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
            return 0

        rel = firmware_release.download(args.tag, Path(tempfile.gettempdir()) / "blinky-promote-assets")
        passing = gate_records(args.tag, rel["manifest"])
        gh("release", "edit", args.tag, "--prerelease=false", "--latest", "--title", f"{args.tag} (stable)")
        if not tag_exists(stable_tag):
            gh("api", f"repos/{repo_slug()}/git/refs", "-f", f"ref=refs/tags/{stable_tag}",
               "-f", f"sha={rel['manifest']['git_sha']}")
        print(f"PROMOTED: {args.tag} ({rel['manifest']['version']}) is stable and Latest; "
              f"gate: {', '.join(passing)}; tag {stable_tag}")
        return 0
    except ReleaseError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
