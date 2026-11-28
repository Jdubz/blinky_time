#!/usr/bin/env python3
# Vendored from Jdubz/blinky-firmware tools/firmware_release.py at commit
# 33e005982c34e7c3664215cacf767bd20910deb2: the download side only. blinky_time owns
# this copy since the 2026-11-24 decoupling; blinky-firmware keeps the full tool
# (package, sign, mirror, gate) for its release flow. `mirror` (mirror_entry, mirror)
# was added in decoupling step 2 from blinky-firmware master 1218847ff5a9 for the
# console's web flasher. Changes from the source are marked "[blinky_time: ...]".
"""
firmware_release.py - blinky-server's release client (blinky-firmware's
docs/RELEASE_PROCESS.md defines what a release is).

Used by blinky-server (blinky_server/firmware/deploy_pipeline.py) and
scripts/deploy.sh to download a verified release or staging build, and by
.github/workflows/blinky-console-deploy.yml to mirror the stable releases for the
console's web flasher.

A release is a GitHub Release of blinky-firmware tagged `b<N>` whose assets are:
  blinky-things-b<N>.hex, blinky-things-b<N>.uf2, manifest.json, and, once
  signed, blinky-things-b<N>.bin + blinky-things-b<N>.otam. [blinky_time: and, once
  blinky-firmware builds it (P3), the ESP32-S3's merged flash image
  blinky-things-b<N>-esp32s3.bin: optional, but when the manifest lists it, it is
  downloaded and verified like the rest, and returned as "esp32s3".] Channels:
  candidate  pre-release `b<N>` (CI-built, not yet gated)
  stable     full release `b<N>` + tag `stable-b<N>`; the latest stable is
             the release GitHub marks "Latest" (rollback = re-mark an older one)
  staging    not a release: the workflow artifact `staging-firmware-<sha>` that
             staging-build.yml builds from every green staging push (version
             b<N>-dev-<sha8>), for test devices only (download_staging)
  dev        not a release: the same dev package, built by the build service from
             an owner commit (docs/DEV_LOOP.md), for test devices only (verify_dev_package:
             blinky-server fetches it, this verifies it)

The repository is explicit: $BLINKY_FIRMWARE_REPO, default Jdubz/blinky-firmware.
It is never read from a git remote, and no firmware checkout is needed.

CLI:
  firmware_release.py download [--tag bN] [--out DIR]   (default: latest stable)
  firmware_release.py latest-stable
  firmware_release.py download-staging --sha SHA [--out DIR]   (staging build, test devices only)
  firmware_release.py verify-dev-package --sha SHA --dir DIR   (a build-service package, the same checks)
  firmware_release.py mirror --out DIR [--keep N]   (stable releases for the web flasher)

Every failure exits non-zero with a message; nothing falls back silently.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# [blinky_time: the repo is explicit; the source took it from its checkout's remote.]
FIRMWARE_REPO = os.environ.get("BLINKY_FIRMWARE_REPO", "Jdubz/blinky-firmware")
TAG_RE = re.compile(r"^b(\d+)$")
STAGING_WORKFLOW = ".github/workflows/staging-build.yml"
STAGING_BRANCH = "staging"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


class ReleaseError(Exception):
    pass


# ---------------------------------------------------------------- helpers ---

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


# [blinky_time: the ESP32-S3 image (docs/TEST_BOX.md, "ESP32-S3 flash and recovery").]
def esp32s3_name(n):
    """The ESP32-S3 merged flash image's asset name (written at 0x0 by hosttools/esp32_flash.py)."""
    return f"blinky-things-b{n}-esp32s3.bin"


def gh(*args, capture=True):
    """Run the GitHub CLI; raise with its stderr on failure.

    [blinky_time: no cwd. Every call names the repo (-R or the api path), so
    gh never depends on the directory it runs in.]"""
    if not shutil.which("gh"):
        raise ReleaseError("GitHub CLI `gh` not found on PATH (needed for releases)")
    r = subprocess.run(["gh", *args], capture_output=capture, text=True)
    if r.returncode != 0:
        raise ReleaseError(f"gh {' '.join(args)} failed: {(r.stderr or '').strip()[:500]}")
    return r.stdout if capture else ""


def repo_slug():
    # [blinky_time: FIRMWARE_REPO, not `gh repo view` in a checkout.]
    return FIRMWARE_REPO


def download_staging(sha, out_dir):
    """Download and verify the staging build of commit ``sha`` (staging-build.yml).

    Refuses unless: the artifact came from a successful staging-build.yml run in
    this repository; its manifest is a dev-channel build of exactly ``sha`` with
    the version gen_version.sh stamps; every listed file matches its sha256; and
    ``sha`` is on the staging branch.
    """
    if not SHA_RE.match(sha or ""):
        raise ReleaseError(f"not a full commit sha: {sha!r}")
    slug = repo_slug()
    name = f"staging-firmware-{sha}"
    arts = json.loads(gh("api", f"repos/{slug}/actions/artifacts?name={name}&per_page=20"))
    live = [a for a in arts.get("artifacts", []) if not a.get("expired")]
    if not live:
        raise ReleaseError(f"no staging build for {sha[:12]} (artifact {name}): "
                           "has staging-build.yml run for it?")
    art = max(live, key=lambda a: a["created_at"])
    run_id = (art.get("workflow_run") or {}).get("id")
    run = json.loads(gh("api", f"repos/{slug}/actions/runs/{run_id}"))
    if run.get("path") != STAGING_WORKFLOW or run.get("event") != "workflow_run":
        raise ReleaseError(f"{name} was not built by {STAGING_WORKFLOW} (run {run_id}: "
                           f"{run.get('path')}, {run.get('event')})")
    if (run.get("repository") or {}).get("full_name") != slug:
        raise ReleaseError(f"{name}: run {run_id} is from another repository")
    if run.get("conclusion") != "success":
        raise ReleaseError(f"{name}: build run {run_id} concluded {run.get('conclusion')}")
    cmp = json.loads(gh("api", f"repos/{slug}/compare/{sha}...{STAGING_BRANCH}"))
    if cmp.get("status") not in ("identical", "ahead"):
        raise ReleaseError(f"{sha[:12]} is not on {STAGING_BRANCH} ({cmp.get('status')})")

    # Its own subdirectory, emptied first: nothing else under out_dir is touched.
    out_dir = Path(out_dir) / sha
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    gh("run", "download", str(run_id), "--name", name, "--dir", str(out_dir), "-R", slug)
    r = verify_dev_package(out_dir, sha, name)
    return {
        "tag": f"staging-{sha[:8]}",
        "sha": sha,
        "run_id": run_id,
        "hex": r["hex"],
        "uf2": r["uf2"],
        "esp32s3": r["esp32s3"],
        "manifest": r["manifest"],
        "stable": False,
        "channel": "dev",
    }


# [blinky_time: factored out of download_staging so a build-service package (build-service/,
# docs/DEV_LOOP.md) is verified by exactly the same checks as a staging artifact.]
def verify_dev_package(pkg_dir, sha, name=None):
    """Verify a dev package in ``pkg_dir`` (a staging-firmware-<sha> artifact or a build-service
    package): its manifest is a dev-channel build of exactly ``sha`` with the version
    gen_version.sh stamps, and the hex and UF2 match their sha256s. [blinky_time: and every
    other file in pkg_dir is a regular file listed in the manifest with a matching sha256, so
    nothing unverified sits beside the image (the box's dev loop, docs/DEV_LOOP.md).] Returns
    {"manifest", "hex", "uf2", "esp32s3"} [blinky_time: "esp32s3" is the ESP32-S3 image when the
    manifest lists it, else None]; raises ReleaseError otherwise."""
    if not SHA_RE.match(sha or ""):
        raise ReleaseError(f"not a full commit sha: {sha!r}")
    pkg_dir = Path(pkg_dir)
    name = name or f"dev package {pkg_dir}"
    try:
        manifest = json.loads((pkg_dir / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ReleaseError(f"{name}: no readable manifest.json ({e})") from None
    if not isinstance(manifest, dict):
        raise ReleaseError(f"{name}: manifest.json is not an object")
    n = manifest.get("build")
    want = f"b{n}-dev-{sha[:8]}"
    if manifest.get("channel") != "dev" or manifest.get("git_sha") != sha or manifest.get("version") != want:
        raise ReleaseError(f"{name}: manifest is not the dev build of {sha[:12]} "
                           f"(channel {manifest.get('channel')}, sha {manifest.get('git_sha')}, "
                           f"version {manifest.get('version')}, want {want})")
    files = manifest.get("files") or {}
    required = [f"blinky-things-b{n}.hex", f"blinky-things-b{n}.uf2"]
    for fname in required:
        p = pkg_dir / fname
        if fname not in files:
            raise ReleaseError(f"{name}: manifest has no sha256 for {fname} - refusing to use it")
        if not p.exists():
            raise ReleaseError(f"{name}: {fname} missing")
        if sha256(p) != files[fname]:
            raise ReleaseError(f"{name}: {fname} sha256 mismatch - refusing to use it")
    for p in sorted(pkg_dir.iterdir()):
        if p.name == "manifest.json":
            continue
        if p.is_symlink() or not p.is_file():
            raise ReleaseError(f"{name}: {p.name} is not a regular file - refusing the package")
        if p.name not in files:
            raise ReleaseError(f"{name}: {p.name} is not in the manifest - refusing the package")
        if sha256(p) != files[p.name]:
            raise ReleaseError(f"{name}: {p.name} sha256 mismatch - refusing to use it")
    s3 = esp32s3_name(n)
    if s3 in files and not (pkg_dir / s3).exists():
        raise ReleaseError(f"{name}: the manifest lists {s3} but it is missing")
    return {"manifest": manifest, "hex": pkg_dir / required[0], "uf2": pkg_dir / required[1],
            "esp32s3": pkg_dir / s3 if s3 in files else None}


def list_releases():
    rows = json.loads(gh("release", "list", "--limit", "200",
                         "--json", "tagName,isPrerelease,isLatest,isDraft", "-R", repo_slug()))
    return [r for r in rows if TAG_RE.match(r["tagName"]) and not r["isDraft"]]


def tag_exists(tag):
    """True if refs/tags/<tag> exists on GitHub. Only a 404 means "no"."""
    if not shutil.which("gh"):
        raise ReleaseError("GitHub CLI `gh` not found on PATH (needed for releases)")
    r = subprocess.run(["gh", "api", f"repos/{repo_slug()}/git/ref/tags/{tag}"],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return True
    if "404" in (r.stderr or "") or "Not Found" in (r.stderr or ""):
        return False
    raise ReleaseError(f"could not check tag {tag}: {(r.stderr or '').strip()[:300]}")


def release_info(tag):
    return json.loads(gh("release", "view", tag, "--json", "tagName,isPrerelease,isDraft,assets,url",
                         "-R", repo_slug()))


def is_stable(tag):
    """Stable = published (not draft), not a pre-release, AND promoted (tag
    stable-b<N> exists). A full release without the promotion marker is not
    stable: release_promote.py is the only way to make one."""
    info = release_info(tag)
    return (not info["isDraft"] and not info["isPrerelease"]
            and tag_exists(f"stable-{tag}"))


def latest_stable_tag():
    latest = [r for r in list_releases() if r["isLatest"]]
    if not latest or not is_stable(latest[0]["tagName"]):
        raise ReleaseError("no stable release yet (the Latest release must be promoted, with tag "
                           "stable-b<N>). Cut a candidate, gate it, and promote it: "
                           "blinky-firmware docs/RELEASE_PROCESS.md")
    return latest[0]["tagName"]


def download(tag, out_dir):
    """Download a release's manifest and every file it lists, verifying each.

    hex and uf2 are required. bin + otam (signed OTA) exist from the first
    signed release on; when present, both must be listed and verified.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    m = TAG_RE.match(tag)
    if not m:
        raise ReleaseError(f"not a release tag: {tag!r} (expected b<N>)")
    n = m.group(1)
    required = [f"blinky-things-b{n}.hex", f"blinky-things-b{n}.uf2"]
    ota_names = [f"blinky-things-b{n}.bin", f"blinky-things-b{n}.otam"]
    s3 = esp32s3_name(n)
    # Replace only this release's own assets; never touch anything else in out_dir.
    for name in ["manifest.json", *required, *ota_names, s3]:
        (out_dir / name).unlink(missing_ok=True)
    gh("release", "download", tag, "--dir", str(out_dir), "--pattern", "manifest.json",
       "-R", repo_slug())
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("build") != int(n):
        raise ReleaseError(f"{tag}: manifest build {manifest.get('build')} != {n}")
    files = manifest.get("files") or {}
    listed_ota = [name for name in ota_names if name in files]
    if listed_ota and len(listed_ota) != len(ota_names):
        raise ReleaseError(f"{tag}: manifest lists {listed_ota} but not both OTA files")
    names = required + listed_ota + ([s3] if s3 in files else [])
    gh("release", "download", tag, "--dir", str(out_dir),
       *[a for name in names for a in ("--pattern", name)], "-R", repo_slug())
    for name in names:
        p = out_dir / name
        if name not in files:
            raise ReleaseError(f"{tag}: manifest has no sha256 for {name} - refusing to use it")
        if not p.exists():
            raise ReleaseError(f"{tag}: asset {name} missing")
        if sha256(p) != files[name]:
            raise ReleaseError(f"{tag}: {name} sha256 mismatch - refusing to use it")
    return {
        "tag": tag,
        "hex": out_dir / f"blinky-things-b{n}.hex",
        "uf2": out_dir / f"blinky-things-b{n}.uf2",
        "bin": out_dir / ota_names[0] if listed_ota else None,
        "otam": out_dir / ota_names[1] if listed_ota else None,
        "esp32s3": out_dir / s3 if s3 in files else None,
        "manifest": manifest,
        "stable": is_stable(tag),
    }


# ----------------------------------------------------------------- mirror ---

MIRROR_INDEX_SCHEMA = 1


def mirror_entry(tag, manifest, published_at, url):
    """One index.json entry. Paths are relative to the index (the web flasher
    resolves them against the index URL) and every file carries the sha256 the
    release manifest gave it, so the browser re-verifies what it downloads."""
    n = TAG_RE.match(tag).group(1)
    files = manifest["files"]
    entry = {
        "tag": tag,
        "build": manifest["build"],
        "version": manifest["version"],
        "git_sha": manifest["git_sha"],
        "settings_version": manifest["settings_version"],
        "device_version": manifest["device_version"],
        "published_at": published_at,
        "url": url,
        "files": {},
    }
    for kind in ("hex", "uf2", "bin", "otam"):
        name = f"blinky-things-b{n}.{kind}"
        if kind in ("bin", "otam") and name not in files:
            continue  # released before signed OTA: not installable from the web updater
        entry["files"][kind] = {"path": f"{tag}/{name}", "sha256": files[name]}
    return entry


def mirror(out_dir, keep):
    """Download the newest `keep` stable releases into out_dir/<tag>/ and write
    out_dir/index.json for the web flasher (blinky-console #firmware).

    Only stable releases are mirrored: the web flasher installs firmware on
    devices nobody bench-gated, which is exactly what stable is for. "No
    stable release yet" is written as latest=null, which the flasher shows as
    a refusal; any other failure (download, sha256) aborts the mirror.
    """
    if keep < 1:
        raise ReleaseError(f"--keep must be >= 1, got {keep}")
    out_dir = Path(out_dir)
    if out_dir.exists():
        shutil.rmtree(out_dir)  # never serve a release that is no longer stable
    out_dir.mkdir(parents=True)
    rows = json.loads(gh("release", "list", "--limit", "200",
                         "--json", "tagName,isPrerelease,isLatest,isDraft,publishedAt",
                         "-R", repo_slug()))
    rows = [r for r in rows if TAG_RE.match(r["tagName"]) and not r["isDraft"] and not r["isPrerelease"]]
    rows.sort(key=lambda r: int(TAG_RE.match(r["tagName"]).group(1)), reverse=True)
    stable = [r for r in rows if tag_exists(f"stable-{r['tagName']}")]
    latest = [r["tagName"] for r in stable if r["isLatest"]]

    releases = []
    for r in stable[:keep]:
        tag = r["tagName"]
        got = download(tag, out_dir / tag)
        # [blinky_time: download() re-checks stable; a release withdrawn since the listing
        # (marker deleted, made a pre-release) aborts the mirror instead of being served.]
        if not got["stable"]:
            raise ReleaseError(f"{tag} stopped being stable during the mirror; run it again")
        # download() leaves only this release's three assets in the directory.
        releases.append(mirror_entry(tag, got["manifest"], r["publishedAt"], release_info(tag)["url"]))
    if latest and latest[0] not in [e["tag"] for e in releases]:
        raise ReleaseError(f"latest stable {latest[0]} is older than the newest {keep} stable "
                           "releases; raise --keep so the flasher's default is mirrored")
    index = {
        "schema": MIRROR_INDEX_SCHEMA,
        "latest": latest[0] if latest else None,
        "releases": releases,
    }
    (out_dir / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    return index


def _opt(path):
    return str(path) if path else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("download")
    d.add_argument("--tag", help="b<N> (default: latest stable)")
    d.add_argument("--out", default=str(Path(tempfile.gettempdir()) / "blinky-release"))
    ds = sub.add_parser("download-staging")
    ds.add_argument("--sha", required=True, help="the staging commit (full sha)")
    ds.add_argument("--out", default=str(Path(tempfile.gettempdir()) / "blinky-staging"))
    vp = sub.add_parser("verify-dev-package")
    vp.add_argument("--sha", required=True, help="the commit the package must be (full sha)")
    vp.add_argument("--dir", required=True, help="the package directory (manifest.json, hex, uf2)")
    sub.add_parser("latest-stable")
    mi = sub.add_parser("mirror")
    mi.add_argument("--out", required=True, help="directory to (re)create; served as /firmware/")
    mi.add_argument("--keep", type=int, default=5, help="newest stable releases to mirror")
    args = ap.parse_args()
    try:
        if args.cmd == "download":
            r = download(args.tag or latest_stable_tag(), args.out)
            print(json.dumps({"tag": r["tag"], "stable": r["stable"], "hex": str(r["hex"]),
                              "uf2": str(r["uf2"]), "esp32s3": _opt(r["esp32s3"]),
                              "version": r["manifest"]["version"]}))
        elif args.cmd == "download-staging":
            r = download_staging(args.sha, args.out)
            print(json.dumps({"tag": r["tag"], "stable": False, "channel": "dev", "sha": r["sha"],
                              "run_id": r["run_id"], "hex": str(r["hex"]), "uf2": str(r["uf2"]),
                              "esp32s3": _opt(r["esp32s3"]), "version": r["manifest"]["version"]}))
        elif args.cmd == "verify-dev-package":
            r = verify_dev_package(args.dir, args.sha)
            print(json.dumps({"sha": args.sha, "hex": str(r["hex"]), "uf2": str(r["uf2"]),
                              "esp32s3": _opt(r["esp32s3"]), "version": r["manifest"]["version"]}))
        elif args.cmd == "latest-stable":
            print(latest_stable_tag())
        elif args.cmd == "mirror":
            index = mirror(args.out, args.keep)
            if index["latest"] is None:
                print("WARNING: no stable release yet; the web flasher will refuse to flash",
                      file=sys.stderr)
            print(json.dumps({"latest": index["latest"],
                              "mirrored": [e["tag"] for e in index["releases"]]}))
    except ReleaseError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
