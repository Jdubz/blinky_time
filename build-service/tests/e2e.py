#!/usr/bin/env python3
"""End to end against a running build service (CI: .github/workflows/pr-validation.yml).

Builds a real blinky-firmware commit through the SHA path, then a new commit on top of it
through the bundle path, and verifies each package with blinky-server's own verifier
(hosttools/firmware_release.py verify_dev_package, the checks download_staging runs).
Then the tracker harness check of the same commit (POST /v1/checks: the commit's own
pr-validation.yml job, in the worker's no-network sandbox, with the corpus it pins), which must
pass, as it did on GitHub. Also: health, auth, an untrusted commit (403), the cache, missing
bundle prerequisites (409), and optionally that the manifest matches the staging build's
artifact for that commit in everything but the file hashes (the hex embeds the build time).

  e2e.py --url URL --sha SHA --firmware DIR --hosttools DIR [--staging-artifact DIR]
  (token in $BLINKY_BUILD_TOKEN; DIR = a blinky-firmware clone checked out at SHA)

Stdlib only. Stdout gets a markdown summary for $GITHUB_STEP_SUMMARY.
"""

import argparse
import io
import json
import os
import secrets
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

BUILD_DEADLINE_S = 40 * 60


def say(msg):
    print(msg, file=sys.stderr, flush=True)


class Api:
    def __init__(self, url, token):
        self.url, self.token = url.rstrip("/"), token

    def req(self, method, path, body=None, ctype=None, token=True, raw=False):
        headers = {"Content-Type": ctype} if ctype else {}
        if token:
            headers["Authorization"] = f"Bearer {self.token if token is True else token}"
        r = urllib.request.Request(self.url + path, data=body, method=method, headers=headers)
        try:
            with urllib.request.urlopen(r, timeout=120) as resp:
                data = resp.read()
                return resp.status, data if raw else json.loads(data)
        except urllib.error.HTTPError as e:
            data = e.read()
            try:
                return e.code, json.loads(data)
            except ValueError:
                return e.code, data

    def post_sha(self, sha, **kw):
        body = json.dumps({"repo": "Jdubz/blinky-firmware", "sha": sha}).encode()
        return self.req("POST", "/v1/builds", body, "application/json", **kw)

    def post_bundle(self, sha, bundle):
        b = "----e2e" + secrets.token_hex(12)
        body = b"".join([
            f'--{b}\r\nContent-Disposition: form-data; name="sha"\r\n\r\n{sha}\r\n'.encode(),
            (f'--{b}\r\nContent-Disposition: form-data; name="bundle"; filename="dev.bundle"\r\n'
             "Content-Type: application/octet-stream\r\n\r\n").encode(),
            Path(bundle).read_bytes(),
            f"\r\n--{b}--\r\n".encode(),
        ])
        return self.req("POST", "/v1/builds", body, f"multipart/form-data; boundary={b}")

    def wait(self, build_id, what="builds"):
        deadline = time.monotonic() + BUILD_DEADLINE_S
        last = None
        while time.monotonic() < deadline:
            code, v = self.req("GET", f"/v1/{what}/{build_id}")
            assert code == 200, (code, v)
            if v["state"] != last:
                say(f"  {build_id}: {v['state']}")
                last = v["state"]
            if v["state"] in ("done", "failed"):
                return v
            time.sleep(5)
        raise SystemExit(f"build {build_id} not finished after {BUILD_DEADLINE_S}s")

    def package(self, build_id, dest):
        code, data = self.req("GET", f"/v1/builds/{build_id}/package", raw=True)
        assert code == 200, (code, data[:300])
        dest.mkdir(parents=True)
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            for m in tar.getmembers():
                assert m.isfile() and "/" not in m.name and not m.name.startswith("."), m.name
                (dest / m.name).write_bytes(tar.extractfile(m).read())
        return sorted(p.name for p in dest.iterdir())


def git(cwd, *args):
    env = {**os.environ, "GIT_AUTHOR_NAME": "e2e", "GIT_AUTHOR_EMAIL": "e2e@example.invalid",
           "GIT_COMMITTER_NAME": "e2e", "GIT_COMMITTER_EMAIL": "e2e@example.invalid"}
    return subprocess.run(["git", "-c", "core.hooksPath=/dev/null", *args], cwd=cwd, env=env,
                          check=True, capture_output=True, text=True).stdout.strip()


def change_and_commit(fw, note):
    """A comment appended to one compiled source: a minimal incremental change."""
    srcs = sorted(git(fw, "ls-files", "blinky-things/*.cpp").splitlines())
    assert srcs, "no blinky-things/*.cpp in the firmware checkout"
    p = Path(fw) / srcs[0]
    p.write_text(p.read_text(encoding="utf-8") + f"\n// build-service e2e: {note}\n",
                 encoding="utf-8")
    git(fw, "commit", "-q", "-am", f"e2e: {note}")
    return git(fw, "rev-parse", "HEAD"), srcs[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--firmware", required=True)
    ap.add_argument("--hosttools", required=True)
    ap.add_argument("--staging-artifact")
    args = ap.parse_args()
    api = Api(args.url, os.environ["BLINKY_BUILD_TOKEN"])
    sys.path.insert(0, str(Path(args.hosttools).resolve()))
    import firmware_release as fr

    work = Path(tempfile.mkdtemp(prefix="blinky-build-e2e-"))
    summary = ["## Build service end to end", ""]

    say("health and auth")
    assert api.req("GET", "/v1/healthz", token=False) == (200, {"ok": True})
    assert api.post_sha(args.sha, token=False)[0] == 401
    assert api.post_sha(args.sha, token="x" * 64)[0] == 401

    say("an untrusted commit is refused")
    code, body = api.post_sha(secrets.token_hex(20))
    assert code == 403 and "not on any branch" in body["error"], (code, body)

    say(f"SHA path: {args.sha}")
    code, b = api.post_sha(args.sha)
    assert code == 202 and b["sha"] == args.sha, (code, b)
    v = api.wait(b["id"])
    assert v["state"] == "done", v
    cold = v["duration_s"]
    names = api.package(b["id"], work / "sha")
    r = fr.verify_dev_package(work / "sha", args.sha)
    say(f"  verified {r['manifest']['version']}: {names} ({cold}s)")
    summary += [f"- SHA path: `{r['manifest']['version']}`, verified, **{cold}s** (cold cache)"]

    if args.staging_artifact:
        theirs = json.loads((Path(args.staging_artifact) / "manifest.json").read_text())
        ours = r["manifest"]
        same = {k: (ours.get(k), theirs.get(k)) for k in set(ours) | set(theirs) if k != "files"}
        diff = {k: v for k, v in same.items() if v[0] != v[1]}
        assert not diff, f"manifest differs from the staging artifact's: {diff}"
        assert sorted(ours["files"]) == sorted(theirs["files"]), (ours["files"], theirs["files"])
        say("  manifest equals the staging artifact's (all but file hashes)")
        summary += ["- manifest equals the `staging-firmware-<sha>` artifact's, apart from the "
                    "file hashes (the hex embeds its build time)"]

    say("the cache returns the done build")
    code, again = api.post_sha(args.sha)
    assert code == 202 and again == {"id": b["id"], "state": "done", "sha": args.sha}, again

    say("bundle path: a new commit on top")
    fw = Path(args.firmware)
    assert git(fw, "rev-parse", "HEAD") == args.sha
    new, changed = change_and_commit(fw, "an incremental change")
    git(fw, "bundle", "create", "-q", str(work / "dev.bundle"), f"{args.sha}..HEAD")
    code, bb = api.post_bundle(new, work / "dev.bundle")
    assert code == 202 and bb["sha"] == new, (code, bb)
    v2 = api.wait(bb["id"])
    assert v2["state"] == "done", v2
    warm = v2["duration_s"]
    api.package(bb["id"], work / "bundle")
    r2 = fr.verify_dev_package(work / "bundle", new)
    say(f"  verified {r2['manifest']['version']} ({warm}s)")
    summary += [f"- bundle path: `{r2['manifest']['version']}` (a comment added to `{changed}`), "
                f"verified, **{warm}s** (warm cache)"]

    say("a bundle whose prerequisite is not on a branch is refused")
    git(fw, "checkout", "-q", "--detach", new)
    unpushed, _ = change_and_commit(fw, "unpushed")
    top, _ = change_and_commit(fw, "on top")
    git(fw, "bundle", "create", "-q", str(work / "orphan.bundle"), f"{unpushed}..HEAD")
    code, body = api.post_bundle(top, work / "orphan.bundle")
    assert code == 409 and "prerequisite" in body["error"], (code, body)

    say(f"check: tracker-harness of {args.sha}")
    body = json.dumps(
        {"repo": "Jdubz/blinky-firmware", "sha": args.sha, "check": "tracker-harness"}
    ).encode()
    code, c = api.req("POST", "/v1/checks", body, "application/json")
    assert code == 202 and c["check"] == "tracker-harness", (code, c)
    v3 = api.wait(c["id"], "checks")
    assert v3["state"] == "done" and v3["conclusion"] == "success", v3
    ran = ", ".join(
        f"{s['name']} {s['seconds']}s" for s in v3["steps"] if s["outcome"] == "success"
    )
    say(f"  passed in {v3['duration_s']}s: {ran}")
    summary += [f"- check `tracker-harness`: passed in **{v3['duration_s']}s** in the sandbox "
                f"(corpus `{v3['corpus'][:12]}`): {ran}"]
    code, body = api.req("POST", "/v1/checks", json.dumps(
        {"repo": "Jdubz/blinky-firmware", "sha": secrets.token_hex(20), "check": "tracker-harness"}
    ).encode(), "application/json")
    assert code == 403, (code, body)

    summary += ["- health, auth (401), untrusted commit (403, builds and checks), cache, "
                "bundle prerequisite (409): ok"]
    print("\n".join(summary))
    say("e2e: all passed")


if __name__ == "__main__":
    main()
