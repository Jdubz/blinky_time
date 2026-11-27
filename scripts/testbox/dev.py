#!/usr/bin/env python3
"""The box's dev loop (docs/DEV_LOOP.md): build a blinky-firmware commit on the build service,
flash it to the box's test chips and run only the checks the change needs, in minutes, before
CI. Run it on the box from inside a blinky-firmware checkout (or with --repo PATH).

  dev.py try [REF] [--hw] [--tracks a,b] [--health MIN] [--pace] [--camera] [--wip]
      everything in one command: enters dev mode if the box isn't in it (and says so), builds,
      flashes, runs the info check plus the ones asked for (--hw: the blocking hardware
      checks, as a PR head gets them), and prints a short report
  dev.py mode dev|queue [--note TEXT] [--preempt]
  dev.py status                       the mode, the queue, the chips, each strict rule's streak
  dev.py build [REF] [--wip]          prints the build id
  dev.py flash BUILD_ID
  dev.py test CHECK...                info | health[=MIN] | validate=a,b | pace[=a,b] | cmd=c1;c2
  dev.py test --hw [--phases P,..] [--expect VERSION] [--regions MAP] [--lights-brightness N]
      the hardware checks (upload, lights, audio, response, interfaces) on the chip as it is

No push needed: a commit that isn't on any of the remote's branches goes to the build service
as a git bundle of <base>..<sha>, where base is its merge-base with a fetched origin/staging
(origin/master if there is no staging). If the service lacks that base (409), the merge-base with
the other branch is tried, and if that fails too it says what to push. A commit already on a
branch goes by sha.

A dirty tree is refused (b210 and b211 reached a device from dirty trees: blinky-firmware
docs/RELEASE_PROCESS.md) unless --wip: then the working tree, untracked files that git doesn't
ignore included, becomes a throwaway commit on top of HEAD, made through a temporary index and
``git commit-tree`` and kept at refs/dev/wip. Your branch, index and stash are never touched.

The box stays in dev mode until you switch it back (``dev.py mode queue``): no expiry, by the
owner's decision. While it is in dev mode the hardware runners (staging, PR queue) hold their
work, which `status` shows.

Every run's report ends with the strict rules (blinky-server testing/strict_rules.py): the bench
gate's limits, non-blocking warnings until the owner makes each one block. `status` shows how
many builds in a row each has passed (readiness).

Config: the build service from ~/.config/blinky/build.env (BLINKY_BUILD_URL,
BLINKY_BUILD_TOKEN; the environment overrides it). There is no default address: the repo is
public. The box's server is http://127.0.0.1:8420 (BLINKY_SERVER_URL overrides it), its API key
~/.blinky-api-key (or BLINKY_API_KEY). Stdlib only, like staging.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from collections.abc import Callable
from pathlib import Path
from typing import Any

import strict

HOME = Path.home()
GH_REPO = "Jdubz/blinky-firmware"
REMOTE_RE = re.compile(r"[:/]Jdubz/blinky-firmware(\.git)?/?$")
BUILD_ENV = HOME / ".config" / "blinky" / "build.env"
SERVER_URL = os.environ.get("BLINKY_SERVER_URL", "http://127.0.0.1:8420")
WIP_REF = "refs/dev/wip"
BUNDLE_REF = "refs/dev/build"
BRANCHES = ("staging", "master")  # bundle bases, in order
CAMERA_REVIEW = Path(__file__).resolve().parent / "camera" / "camera_review.py"
CAMERA_REGIONS = HOME / ".local" / "share" / "blinky-camera" / "regions-grid-1000us.npz"
POLL_S = 3.0
BUILD_WAIT_S = 20 * 60
FLASH_WAIT_S = 20 * 60
MODE_WAIT_S = 45 * 60
RESET_RETRY_MAX_S = 65  # the server's 60 s rule between resets: wait it out once
TERMINAL = ("complete", "error", "cancelled")  # server job states (work/jobs.py)
# The info timers the report prints first, in this order; the rest follow.
TIMERS = ("acfUs", "acfSliceUs", "acfStageUs", "plpUs", "onsetUs", "maxFrameMs", "heapPeak")

Server = Callable[..., Any]


class DevError(Exception):
    """Something the user must act on: printed as one line, exit 1."""


def say(msg: str = "") -> None:
    print(msg, flush=True)


# -- config ------------------------------------------------------------------------------------


def env_file(path: Path) -> dict[str, str]:
    """KEY=VALUE lines (``export`` and quotes allowed), as a shell sources them."""
    out: dict[str, str] = {}
    try:
        text = path.read_text()
    except OSError:
        return out
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.removeprefix("export ").partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        out[key.strip()] = value
    return out


def build_config(path: Path = BUILD_ENV) -> tuple[str, str]:
    file = env_file(path)
    url = os.environ.get("BLINKY_BUILD_URL") or file.get("BLINKY_BUILD_URL", "")
    token = os.environ.get("BLINKY_BUILD_TOKEN") or file.get("BLINKY_BUILD_TOKEN", "")
    if not url or not token:
        raise DevError(
            f"no build service configured: set BLINKY_BUILD_URL and BLINKY_BUILD_TOKEN in {path}"
        )
    return url.rstrip("/"), token


def api_key() -> str:
    key = os.environ.get("BLINKY_API_KEY")
    return key or (HOME / ".blinky-api-key").read_text().strip()


# -- HTTP --------------------------------------------------------------------------------------


def _detail(body: bytes) -> str:
    try:
        got = json.loads(body or b"{}")
    except ValueError:
        return body.decode(errors="replace")[:300]
    if isinstance(got, dict):
        return str(got.get("detail") or got.get("error") or got)[:300]
    return str(got)[:300]


class ServerError(DevError):
    def __init__(self, code: int, detail: str, retry_after: str | None = None) -> None:
        super().__init__(f"the box's server answered {code}: {detail}")
        self.code, self.detail, self.retry_after = code, detail, retry_after


def http(method: str, path: str, body: Any = None, timeout: float = 30) -> Any:
    """The box's server (localhost): JSON in and out, the API key on every call."""
    req = urllib.request.Request(
        SERVER_URL + path,
        data=json.dumps(body).encode() if body is not None else None,
        method=method,
        headers={"X-API-Key": api_key(), "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        raise ServerError(e.code, _detail(e.read()), e.headers.get("Retry-After")) from e
    except (urllib.error.URLError, OSError) as e:
        raise DevError(f"the box's server isn't answering at {SERVER_URL}: {e}") from e


class BaseMissing(DevError):
    """The build service lacks the bundle's prerequisite commits (409)."""


class Untrusted(DevError):
    """The build service refused the commit as not an owner commit (403)."""


class ServiceError(DevError):
    """Any other HTTP error from the build service, with its status code."""

    def __init__(self, msg: str, code: int) -> None:
        super().__init__(msg)
        self.code = code


class BuildService:
    """The build service's contract (docs/DEV_LOOP.md), bearer-token auth:
    POST /v1/builds (JSON {repo, sha} or multipart bundle + sha) -> 202 {id, state, sha};
    403 an untrusted commit, 409 bundle prerequisites missing. GET /v1/builds/{id}."""

    def __init__(self, url: str, token: str) -> None:
        self.url, self.token = url, token

    def _call(
        self, method: str, path: str, data: bytes | None = None, ctype: str | None = None
    ) -> Any:
        headers = {"Authorization": f"Bearer {self.token}"}
        if ctype:
            headers["Content-Type"] = ctype
        req = urllib.request.Request(self.url + path, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                return json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            detail = _detail(e.read())
            if e.code == 409:
                raise BaseMissing(f"the build service lacks the bundle's base: {detail}") from e
            if e.code == 403:
                raise Untrusted(
                    f"the build service refused the commit (403: {detail}). It builds only owner "
                    "commits: a branch of the repo, or a bundle from the box (docs/DEV_LOOP.md)"
                ) from e
            raise ServiceError(f"the build service answered {e.code}: {detail}", e.code) from e
        except (urllib.error.URLError, OSError) as e:
            raise DevError(f"the build service isn't answering: {e}") from e

    def submit_sha(self, sha: str) -> dict[str, Any]:
        body = json.dumps({"repo": GH_REPO, "sha": sha}).encode()
        rec: dict[str, Any] = self._call("POST", "/v1/builds", body, "application/json")
        return rec

    def submit_bundle(self, bundle: Path, sha: str) -> dict[str, Any]:
        boundary = uuid.uuid4().hex
        parts = [
            f'--{boundary}\r\nContent-Disposition: form-data; name="sha"\r\n\r\n{sha}\r\n'.encode(),
            (
                f'--{boundary}\r\nContent-Disposition: form-data; name="bundle"; '
                f'filename="{sha[:12]}.bundle"\r\nContent-Type: application/octet-stream\r\n\r\n'
            ).encode(),
            bundle.read_bytes(),
            f"\r\n--{boundary}--\r\n".encode(),
        ]
        ctype = f"multipart/form-data; boundary={boundary}"
        rec: dict[str, Any] = self._call("POST", "/v1/builds", b"".join(parts), ctype)
        return rec

    def get(self, build_id: str) -> dict[str, Any]:
        rec: dict[str, Any] = self._call("GET", f"/v1/builds/{build_id}")
        return rec


# -- git ---------------------------------------------------------------------------------------


class Repo:
    def __init__(self, path: Path) -> None:
        try:
            top = subprocess.run(
                ["git", "-C", str(path), "rev-parse", "--show-toplevel"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        except (subprocess.CalledProcessError, OSError) as e:
            raise DevError(
                f"{path} is not a git checkout (run from blinky-firmware, or --repo)"
            ) from e
        self.root = Path(top)

    def git(self, *args: str, env: dict[str, str] | None = None, check: bool = True) -> str:
        r = subprocess.run(
            ["git", *args], cwd=self.root, capture_output=True, text=True, env=env, check=False
        )
        if check and r.returncode != 0:
            raise DevError(f"git {' '.join(args[:3])} failed: {r.stderr.strip()[-500:]}")
        return r.stdout

    def check_origin(self) -> None:
        url = self.git("remote", "get-url", "origin", check=False).strip()
        if not REMOTE_RE.search(url):
            raise DevError(
                f"{self.root} is not a blinky-firmware checkout (origin: {url or 'none'})"
            )

    def rev(self, ref: str) -> str:
        sha = self.git("rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}", check=False).strip()
        if not sha:
            raise DevError(f"no commit {ref!r} in {self.root}")
        return sha

    def dirty(self) -> list[str]:
        """What `git status` shows: changes, staged or not, and untracked files git doesn't
        ignore."""
        return [ln for ln in self.git("status", "--porcelain").splitlines() if ln.strip()]

    def branch(self) -> str | None:
        name = self.git("symbolic-ref", "--quiet", "--short", "HEAD", check=False).strip()
        return name or None

    def wip_commit(self) -> str:
        """The working tree as a commit on top of HEAD, at refs/dev/wip: a temporary index
        (seeded from the real one, which is only read), ``git add -A`` into it, ``write-tree``,
        ``commit-tree -p HEAD``. The branch, the index and the stash are never touched."""
        head = self.rev("HEAD")
        real = self.root / self.git("rev-parse", "--git-path", "index").strip()
        with tempfile.TemporaryDirectory(prefix="dev-wip-") as td:
            index = Path(td) / "index"
            if real.is_file():
                shutil.copyfile(real, index)
            env = {**os.environ, "GIT_INDEX_FILE": str(index)}
            self.git("add", "-A", env=env)
            tree = self.git("write-tree", env=env).strip()
        msg = f"dev.py --wip: the working tree on {self.branch() or head[:12]}"
        env = dict(os.environ)
        if not self.git("config", "user.email", check=False).strip():
            env.setdefault("GIT_AUTHOR_NAME", "dev.py")
            env.setdefault("GIT_AUTHOR_EMAIL", "dev.py@localhost")
            env.setdefault("GIT_COMMITTER_NAME", "dev.py")
            env.setdefault("GIT_COMMITTER_EMAIL", "dev.py@localhost")
        sha = self.git("commit-tree", tree, "-p", head, "-m", msg, env=env).strip()
        self.git("update-ref", WIP_REF, sha)
        return sha

    def fetch(self) -> None:
        self.git("fetch", "--quiet", "origin")

    def on_remote(self, sha: str) -> bool:
        """sha is on one of the remote's branches (as last fetched)."""
        return bool(self.git("branch", "-r", "--contains", sha, check=False).strip())

    def merge_base(self, sha: str, ref: str) -> str | None:
        if not self.git("rev-parse", "--verify", "--quiet", ref, check=False).strip():
            return None
        return self.git("merge-base", sha, ref, check=False).strip() or None

    def bundle(self, sha: str, base: str, out: Path) -> Path:
        """A bundle of base..sha (its one ref, refs/dev/build, at sha)."""
        self.git("update-ref", BUNDLE_REF, sha)
        self.git("bundle", "create", "--quiet", str(out), BUNDLE_REF, f"^{base}")
        return out


# -- build -------------------------------------------------------------------------------------


def resolve(repo: Repo, ref: str | None, wip: bool) -> str:
    """The commit to build: REF, or HEAD; a dirty tree is refused for HEAD unless --wip."""
    if ref is not None:
        if wip:
            raise DevError("--wip builds the working tree: give no REF with it")
        return repo.rev(ref)
    dirty = repo.dirty()
    if dirty and not wip:
        shown = ", ".join(ln[3:] for ln in dirty[:5]) + (" ..." if len(dirty) > 5 else "")
        raise DevError(
            f"the working tree is dirty ({shown}): commit it, or --wip to build it as a "
            "throwaway commit (refs/dev/wip; your branch, index and stash stay as they are)"
        )
    if wip and dirty:
        sha = repo.wip_commit()
        say(f"--wip: the working tree is commit {sha[:12]} ({WIP_REF})")
        return sha
    return repo.rev("HEAD")


def submit(repo: Repo, svc: BuildService, sha: str) -> dict[str, Any]:
    """Send sha to the build service: by sha when it is on a remote branch, else as a bundle on
    the merge-base with origin/staging, then origin/master (on a 409). Returns its record."""
    repo.fetch()
    if repo.on_remote(sha):
        say(f"{sha[:12]} is on a branch of {GH_REPO}: building it by sha")
        return svc.submit_sha(sha)
    tried: list[tuple[str, str]] = []  # (branch, base) the service lacked
    bases = [(b, repo.merge_base(sha, f"origin/{b}")) for b in BRANCHES]
    with tempfile.TemporaryDirectory(prefix="dev-bundle-") as td:
        for branch, base in bases:
            if base is None or base in (t[1] for t in tried):
                continue
            path = repo.bundle(sha, base, Path(td) / f"{sha[:12]}.bundle")
            size = path.stat().st_size
            say(f"bundle {base[:12]}..{sha[:12]} (merge-base with origin/{branch}, {size} bytes)")
            try:
                return svc.submit_bundle(path, sha)
            except BaseMissing as e:
                say(f"  {e}")
                tried.append((branch, base))
    current = repo.branch()
    push = (
        f"git push origin {current}"
        if current
        else f"git push origin {sha}:refs/heads/dev/{sha[:8]}"
    )
    what = (
        "; ".join(f"{base[:12]} (origin/{b})" for b, base in tried)
        or "no merge-base with origin/staging or origin/master"
    )
    raise DevError(
        f"the build service has none of the bases tried ({what}): push the commit so it can "
        f"fetch it ({push}), then run again"
    )


def started(rec: dict[str, Any], sha: str) -> str:
    """Say what the service did with the submission: a new build is queued, a commit already
    built is ``done`` at once (a cache hit), one already building is that build. Its id."""
    state = rec.get("state")
    if rec.get("sha") not in (None, sha):
        raise DevError(f"the build service answered for {rec.get('sha')}, not {sha}")
    note = "already built (cache hit)" if state == "done" else state
    say(f"build {rec['id']} of {sha[:12]}: {note}")
    return str(rec["id"])


def wait_build(svc: BuildService, build_id: str, wait_s: float = BUILD_WAIT_S) -> dict[str, Any]:
    deadline = time.monotonic() + wait_s
    last = None
    while True:
        rec = svc.get(build_id)
        if rec.get("state") != last:
            last = rec.get("state")
            say(f"build {build_id}: {last}")
        if rec.get("state") == "done":
            if rec.get("duration_s") is not None:
                say(f"build {build_id}: built in {float(rec['duration_s']):.1f} s")
            return rec
        if rec.get("state") == "failed":
            tail = str(rec.get("log_tail") or "").rstrip()
            raise DevError(
                f"build {build_id} failed: {rec.get('error')}" + (f"\n{tail}" if tail else "")
            )
        if time.monotonic() > deadline:
            raise DevError(f"build {build_id} not done after {wait_s / 60:.0f} min ({last})")
        time.sleep(POLL_S)


def build(repo: Repo, svc: BuildService, ref: str | None, wip: bool) -> dict[str, Any]:
    sha = resolve(repo, ref, wip)
    rec = submit(repo, svc, sha)
    done = wait_build(svc, started(rec, sha))
    say(f"build {done['id']}: {done.get('version')}")
    return done


# -- the box -----------------------------------------------------------------------------------


def test_devices(server: Server) -> list[str]:
    """Connected, configured nRF52840s: every chip on the box is a test chip (staging.py)."""
    return [
        d["id"]
        for d in server("GET", "/api/devices")
        if d.get("state") == "connected"
        and d.get("platform") == "nrf52840"
        and d.get("device_type")
    ]


def chips(server: Server, named: list[str] | None) -> list[str]:
    """The chips named, or every test chip on the box; none is an error, said plainly."""
    devices = named or test_devices(server)
    if not devices:
        raise DevError("no connected, configured test chip on the box")
    return devices


def _hhmm(t: Any) -> str:
    return time.strftime("%H:%M", time.localtime(float(t or 0)))


def _age(t: Any) -> str:
    s = max(0, int(time.time() - float(t or 0)))
    return f"{s // 3600} h {s % 3600 // 60} min" if s >= 3600 else f"{s // 60} min"


def set_mode(
    server: Server, mode: str, note: str | None, preempt: bool, wait: bool = True
) -> dict[str, Any]:
    st: dict[str, Any] = server(
        "PUT", "/api/box/mode", {"mode": mode, "note": note, "preempt": preempt}
    )
    if st.get("switching") and wait:
        st = wait_dev(server, st)
    return st


def wait_dev(server: Server, st: dict[str, Any], wait_s: float = MODE_WAIT_S) -> dict[str, Any]:
    """Until a switch to dev that waits for a hardware test has happened."""
    if st.get("mode") == "dev":
        return st
    suites = ", ".join((st.get("switching") or {}).get("waiting_for") or [])
    say(f"switching to dev mode once hardware test {suites} ends (--preempt cancels it instead)")
    deadline = time.monotonic() + wait_s
    while st.get("mode") != "dev":
        if not st.get("switching"):
            raise DevError("the switch to dev mode was called off (someone set queue mode)")
        if time.monotonic() > deadline:
            raise DevError(f"still waiting for hardware test {suites} after {wait_s / 60:.0f} min")
        time.sleep(10 * POLL_S)
        st = server("GET", "/api/box/mode")
    say("box in dev mode")
    return st


def ensure_dev(server: Server, note: str, preempt: bool) -> dict[str, Any]:
    """dev mode for `try`: entered if the box isn't in it, and said so."""
    st: dict[str, Any] = server("GET", "/api/box/mode")
    if st.get("mode") == "dev":
        return st
    say(
        "the box is in queue mode: switching it to dev mode for this run. It stays there (no "
        "expiry): `dev.py mode queue` when you're done, so the staging runner can carry on."
    )
    return set_mode(server, "dev", note, preempt, wait=False)


def poll_job(server: Server, path: str, wait_s: float, what: str) -> dict[str, Any]:
    deadline = time.monotonic() + wait_s
    last = None
    while True:
        job: dict[str, Any] = server("GET", path)
        msg = job.get("progressMessage")
        if msg and msg != last:
            say(f"  {what}: {msg}")
            last = msg
        if job.get("status") in TERMINAL:
            return job
        if time.monotonic() > deadline:
            raise DevError(f"{what} not finished after {wait_s / 60:.0f} min ({path})")
        time.sleep(POLL_S)


def flash(server: Server, build_id: str, devices: list[str]) -> dict[str, Any]:
    body = {"build_id": build_id, "device_ids": devices}
    try:
        sub = server("POST", "/api/dev/deploy", body)
    except ServerError as e:
        wait = float(e.retry_after) if e.code == 409 and e.retry_after else None
        if wait is None or wait > RESET_RETRY_MAX_S:
            raise
        say(f"{e.detail}: waiting {wait:.0f} s")
        time.sleep(wait)
        sub = server("POST", "/api/dev/deploy", body)
    say(f"flashing build {build_id} to {', '.join(d[:12] for d in devices)} (job {sub['job_id']})")
    job = poll_job(server, f"/api/fleet/jobs/{sub['job_id']}", FLASH_WAIT_S, "flash")
    res = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(res, dict):
        return {"status": "error", "message": job.get("error") or job.get("status")}
    return res


def parse_checks(tokens: list[str]) -> list[dict[str, Any]]:
    """info | health[=MIN] | validate=a,b | pace[=a,b] | cmd=c1;c2 (or commands=...)."""
    checks: list[dict[str, Any]] = []
    for tok in tokens:
        kind, _, arg = tok.partition("=")
        if kind == "info" and not arg:
            checks.append({"kind": "info"})
        elif kind == "health":
            try:
                minutes = float(arg) if arg else 5.0
            except ValueError:
                raise DevError(f"health={arg!r}: the minutes are a number (health=2)") from None
            checks.append({"kind": "health", "minutes": minutes})
        elif kind == "validate" and arg:
            checks.append({"kind": "validate", "tracks": [t for t in arg.split(",") if t]})
        elif kind == "pace":
            checks.append({"kind": "pace", **({"tracks": arg.split(",")} if arg else {})})
        elif kind in ("cmd", "commands") and arg:
            checks.append({"kind": "commands", "commands": [c.strip() for c in arg.split(";")]})
        else:
            raise DevError(
                f"unknown check {tok!r}: info | health[=MIN] | validate=a,b | pace[=a,b] | "
                "cmd=c1;c2"
            )
    return checks


def run_checks(server: Server, devices: list[str], checks: list[dict[str, Any]]) -> dict[str, Any]:
    sub = server("POST", "/api/dev/run", {"device_ids": devices, "checks": checks})
    say(f"testing: {', '.join(c['kind'] for c in checks)} (job {sub['job_id']})")
    minutes = sum(float(c.get("minutes") or 0) for c in checks)
    tracks = sum(len(c.get("tracks") or []) for c in checks) + 2 * sum(
        c["kind"] == "pace" for c in checks
    )
    wait_s = (10 + minutes + 2 * tracks) * 60
    job = poll_job(server, f"/api/test/jobs/{sub['job_id']}", wait_s, "test")
    res = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(res, dict):
        return {"passed": False, "checks": [], "error": job.get("error") or job.get("status")}
    return res


HW_WAIT_S = 20 * 60  # the hardware checks: ~4 min, plus the 75 s stability wait after a flash
HW_MARK = {"yes": "yes", "no": "NO", "not applicable": "n/a"}


def run_hw(server: Server, devices: list[str], **opts: Any) -> dict[str, Any]:
    """The hardware checks on the chips as they are (POST /api/dev/hardware-checks)."""
    body: dict[str, Any] = {"device_ids": devices}
    body.update({k: v for k, v in opts.items() if v is not None})
    sub = server("POST", "/api/dev/hardware-checks", body)
    say(f"hardware checks (job {sub['job_id']})")
    job = poll_job(server, f"/api/test/jobs/{sub['job_id']}", HW_WAIT_S, "hardware checks")
    res = job.get("result") if job.get("status") == "complete" else None
    if not isinstance(res, dict):
        return {"passed": False, "checks": [], "rig_error": job.get("error") or job.get("status")}
    return res


def report_hw(res: dict[str, Any]) -> list[str]:
    lines = []
    for p in res.get("phases") or []:
        secs = f" {p['seconds']:.0f} s" if isinstance(p.get("seconds"), (int, float)) else ""
        lines.append(f"phase {p['name']}: {p['result']}{secs}")
    for c in res.get("checks") or []:
        mark = HW_MARK.get(c["result"], c["result"])
        lines.append(f"  {c['phase']}/{c['name']} {c['device'][:8]}: {mark}: {c['evidence']}")
    lines += strict.text_lines(res.get("strict"))
    if res.get("rig_error"):
        lines.append(f"hardware checks: RIG ERROR (no verdict on the firmware): {res['rig_error']}")
    else:
        lines.append(f"hardware checks: {'PASSED' if res.get('passed') else 'FAILED'}")
    if res.get("path"):
        lines.append(f"report: {res['path']}")
    return lines


# -- the report --------------------------------------------------------------------------------


def _mark(ok: Any) -> str:
    return "ok" if ok else "FAIL"


def _kv(d: dict[str, Any], skip: tuple[str, ...] = ()) -> str:
    return ", ".join(f"{k}={v}" for k, v in d.items() if k not in skip)


def timers_line(t: dict[str, Any]) -> str:
    first = [f"{k}={t[k]}" for k in TIMERS if k in t]
    rest = [f"{k}={v}" for k, v in t.items() if k not in TIMERS]
    return ", ".join(first + rest)


def report_flash(res: dict[str, Any]) -> list[str]:
    if res.get("status") != "ok":
        return [f"flash: FAILED at {res.get('failed_step', '?')}: {res.get('message')}"]
    lines = [f"flash: ok, {res.get('version')} ({res.get('message')})"]
    for dev, r in ((res.get("steps") or {}).get("flash") or {}).get("per_device", {}).items():
        lines.append(f"  {dev}: {r.get('flash')} {r.get('version') or ''}".rstrip())
    return lines


def latency_note(run: dict[str, Any] | None) -> str:
    """A validated track's detection latency (blinky-server testing/detection_latency.py):
    ", latency +1.2 ms", with why it isn't counted when it isn't."""
    if not run:
        return ""
    off = run.get("offset_ms")
    shown = f", latency {off:+.1f} ms" if isinstance(off, (int, float)) else ", latency n/a"
    return shown + ("" if run.get("counted") else f" (not counted: {run.get('why')})")


def latency_lines(res: dict[str, Any]) -> list[str]:
    """Per device, the run's detection latency median over the tracks that count."""
    out = []
    for dev, d in (res.get("detection_latency") or {}).items():
        med = d.get("median_ms")
        out.append(
            f"detection latency {dev[:12]}: median "
            + (f"{med:+.1f} ms" if isinstance(med, (int, float)) else "n/a")
            + f" over {d.get('counted', 0)} of {len(d.get('runs') or [])} tracks "
            "(lagCheckMs - lagMs; 0 = the 45 ms calibration, + = later)"
        )
    return out


def report_run(res: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    # Each device's scored runs in the order the report lists them (one per validated track),
    # by the id's first 12 characters, as the scores are keyed (the summary has the full id).
    latency = {
        dev[:12]: list(d.get("runs") or [])
        for dev, d in (res.get("detection_latency") or {}).items()
    }
    if res.get("error"):
        lines.append(f"test run: FAILED: {res['error']}")
    for c in res.get("checks") or []:
        kind = c["kind"]
        if c.get("detail"):
            lines.append(f"{kind}: {c['detail']}")
            continue
        if kind == "info":
            for dev, d in c["devices"].items():
                lines.append(f"info {dev[:12]} {d.get('version')}: {timers_line(d['timers'])}")
        elif kind == "health":
            for dev, d in c["devices"].items():
                st = d["stability"]
                lines.append(
                    f"health {c['minutes']:g} min {dev[:12]}: no reboot {_mark(st['pass'])}, "
                    f"{len(d.get('windows') or [])} loop windows read (judged below), overruns "
                    f"+{st['overrun_growth']}{', REBOOTED' if st['rebooted'] else ''}"
                )
        elif kind == "validate":
            lines.append(
                f"validate: {c['played']}/{c['tracks']} played (scores reported, not gated)"
            )
            for r in c["results"]:
                if r.get("failed"):
                    lines.append(f"  {r['track']}: playback failed")
                    continue
                for dev, s in (r.get("scores") or {}).items():
                    drums = s.get("drums") or {}
                    keys = ("drumF", "kickR50", "snareR50", "onDrum", "loneHat")
                    shown = ", ".join(f"{k}={drums.get(k)}" for k in keys if k in drums)
                    queue = latency.get(dev[:12]) or []
                    note = latency_note(queue.pop(0) if queue else None)
                    lines.append(f"  {r['track']} {dev}: {shown or _kv(s)}{note}")
        elif kind == "pace":
            for dev, d in c["devices"].items():
                m, p = d["music"], d["beatpace"]
                lines.append(
                    f"pace {c['track']} {dev[:12]} (reported): music {_kv(m)}; beat pace {_kv(p)}"
                )
        elif kind == "commands":
            for dev, replies in c["devices"].items():
                for r in replies:
                    first = (r["reply"].strip().splitlines() or [""])[0][:100]
                    lines.append(f"cmd {dev[:12]} `{r['command']}`: {first}")
    lines += latency_lines(res)
    for dev, f in (res.get("final") or {}).items():
        lines.append(
            f"asserts {dev[:12]}: no reboot {_mark(f['no_reboot']['pass'])}, "
            f"alerts {_mark(f['alerts']['pass'])}"
            + "".join(f"\n  {a}" for a in f["alerts"].get("lines", [])[:10])
        )
    lines += strict.text_lines(res.get("strict"))
    lines.append(f"verdict (health + asserts): {'PASSED' if res.get('passed') else 'FAILED'}")
    return lines


def report_mode(st: dict[str, Any]) -> list[str]:
    waiting = st.get("waiting") or []
    if st.get("mode") == "dev":
        head = f"box: DEV mode since {_hhmm(st.get('since'))} ({_age(st.get('since'))})"
    elif st.get("switching"):
        sw = st["switching"]
        head = f"box: switching to dev, waiting for hardware test {', '.join(sw['waiting_for'])}"
    else:
        head = "box: queue mode"
    lines = [head + (f": {st['note']}" if st.get("note") else "")]
    held = st.get("mode") == "dev" or st.get("switching")
    lines.append(f"waiting {'behind dev mode' if held else 'for the box'}: {len(waiting)}")
    for w in waiting:
        extra = ", preempted" if w.get("preempted") else ""
        what = f" ({w['what']})" if w.get("what") else ""
        lines.append(
            f"  {w.get('runner')}: {str(w.get('sha'))[:12]}{what} for {_age(w.get('since'))}{extra}"
        )
    if st.get("mode") == "dev" or st.get("switching"):
        lines.append("leave dev mode: dev.py mode queue")
    return lines


# -- commands ----------------------------------------------------------------------------------


def cmd_status(server: Server) -> int:
    for line in report_mode(server("GET", "/api/box/mode")):
        say(line)
    for d in server("GET", "/api/devices"):
        if d.get("platform") == "nrf52840":
            say(
                f"chip {d['id'][:12]} {d.get('device_type') or 'unconfigured'}: {d.get('state')}, {d.get('version')}"
            )
    for s in (server("GET", "/api/work") or {}).get("in_flight") or []:
        say(
            f"running: {s.get('workflow_id')} {str(s.get('step')).rsplit('.', 1)[-1]} ({s.get('running_s')} s)"
        )
    try:
        ready = server("GET", "/api/test/strict-rules")
    except DevError as e:  # an older server without the route
        say(f"strict rules: not reported by the server ({e})")
    else:
        for line in strict.readiness_text(ready):
            say(line)
    return 0


def camera(devices: list[str], tracks: list[str]) -> int:
    """camera_review.py: its own validation job, the chips lit, the region map of the grid."""
    if not CAMERA_REGIONS.is_file():
        raise DevError(f"no camera region map at {CAMERA_REGIONS} (camera_calibrate.py makes it)")
    args = [sys.executable, str(CAMERA_REVIEW), "--regions", str(CAMERA_REGIONS)]
    for d in devices:
        args += ["--device", d]
    for t in tracks:
        args += ["--track", t]
    say("camera review (secondary, never a gate): " + " ".join(args[1:]))
    return subprocess.run(args, check=False).returncode


def cmd_try(args: argparse.Namespace, server: Server, svc: BuildService, repo: Repo) -> int:
    checks: list[dict[str, Any]] = [{"kind": "info"}]
    if args.health:
        checks.append({"kind": "health", "minutes": args.health})
    tracks = [t for t in (args.tracks or "").split(",") if t]
    if tracks:
        checks.append({"kind": "validate", "tracks": tracks})
    if args.pace:
        checks.append({"kind": "pace"})
    sha = resolve(repo, args.ref, args.wip)
    devices = chips(server, args.device)
    st = ensure_dev(server, args.note or f"dev.py try {sha[:8]}", args.preempt)
    rec = submit(repo, svc, sha)
    done = wait_build(svc, started(rec, sha))
    st = wait_dev(server, st)
    res = flash(server, str(done["id"]), devices)
    report = [f"== dev.py try {sha[:12]}: {done.get('version')} (build {done['id']})"]
    report += report_flash(res)
    ok = res.get("status") == "ok"
    if ok and args.hw:
        hw = run_hw(server, devices, expected_version=res.get("version"))
        report += report_hw(hw)
        ok = bool(hw.get("passed"))
    if ok and (not args.hw or len(checks) > 1):
        run = run_checks(server, devices, checks)
        report += report_run(run)
        ok = bool(run.get("passed"))
        if args.camera:
            rc = camera(devices, tracks)
            report.append(f"camera review: {'done' if rc == 0 else f'exit {rc}'} (not a gate)")
    report += report_mode(server("GET", "/api/box/mode"))
    say()
    for line in report:
        say(line)
    return 0 if ok else 1


def main(argv: list[str] | None = None, server: Server = http) -> int:
    ap = argparse.ArgumentParser(
        description=(__doc__ or "").split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--repo", type=Path, default=Path.cwd(), help="a blinky-firmware checkout")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mode", help="switch the box: dev | queue")
    m.add_argument("mode", choices=("dev", "queue"))
    m.add_argument("--note")
    m.add_argument(
        "--preempt",
        action="store_true",
        help="cancel a running hardware test (hardware checks or the staging quality suite)",
    )
    sub.add_parser("status", help="the box's mode, what waits behind it, the chips")
    b = sub.add_parser("build", help="build a commit on the build service")
    b.add_argument("ref", nargs="?")
    b.add_argument("--wip", action="store_true", help="build the dirty working tree")
    f = sub.add_parser("flash", help="flash a build to the box's test chips (dev mode)")
    f.add_argument("build_id")
    f.add_argument("--device", action="append")
    t = sub.add_parser("test", help="run checks on the test chips (dev mode)")
    t.add_argument(
        "checks", nargs="*", help="info | health[=MIN] | validate=a,b | pace | cmd=c1;c2"
    )
    t.add_argument("--device", action="append")
    t.add_argument("--hw", action="store_true", help="the blocking hardware checks")
    t.add_argument("--phases", help="--hw: these phases only (upload,lights,audio,...)")
    t.add_argument("--expect", metavar="VERSION", help="--hw: the version the chip must run")
    t.add_argument("--regions", help="--hw: a camera region map (default: the board's)")
    t.add_argument(
        "--lights-brightness",
        type=int,
        metavar="N",
        help="--hw: the lights check's `leds` level (0 shows the check failing)",
    )
    y = sub.add_parser("try", help="dev mode, build, flash, test, report: one command")
    y.add_argument("ref", nargs="?")
    y.add_argument("--hw", action="store_true", help="the blocking hardware checks after the flash")
    y.add_argument("--tracks", help="validate these edm/ tracks (comma-separated)")
    y.add_argument("--health", type=float, metavar="MIN", help="a health check this long")
    y.add_argument("--info", action="store_true", help="the info timers (always run)")
    y.add_argument("--pace", action="store_true", help="the music and beat-pace check")
    y.add_argument("--camera", action="store_true", help="then camera_review.py on the tracks")
    y.add_argument("--wip", action="store_true", help="build the dirty working tree")
    y.add_argument("--note")
    y.add_argument(
        "--preempt",
        action="store_true",
        help="cancel a running hardware test (hardware checks or the staging quality suite)",
    )
    y.add_argument("--device", action="append")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "mode":
            st = set_mode(server, args.mode, args.note, args.preempt)
            for line in report_mode(st):
                say(line)
            return 0
        if args.cmd == "status":
            return cmd_status(server)
        if args.cmd == "flash":
            res = flash(server, args.build_id, chips(server, args.device))
            for line in report_flash(res):
                say(line)
            return 0 if res.get("status") == "ok" else 1
        if args.cmd == "test":
            if not args.hw and not args.checks:
                raise DevError("name the checks to run, or --hw for the hardware checks")
            checks = parse_checks(args.checks)  # a typo is said before anything runs
            devices = chips(server, args.device)
            ok = True
            if args.hw:
                phases = [p for p in (args.phases or "").split(",") if p] or None
                hw = run_hw(
                    server,
                    devices,
                    phases=phases,
                    expected_version=args.expect,
                    regions=args.regions,
                    lights_brightness=args.lights_brightness,
                )
                for line in report_hw(hw):
                    say(line)
                ok = bool(hw.get("passed"))
            if checks:
                res = run_checks(server, devices, checks)
                for line in report_run(res):
                    say(line)
                ok = ok and bool(res.get("passed"))
            return 0 if ok else 1
        repo = Repo(args.repo)
        repo.check_origin()
        svc = BuildService(*build_config())
        if args.cmd == "build":
            build(repo, svc, args.ref, args.wip)
            return 0
        return cmd_try(args, server, svc, repo)
    except DevError as e:
        say(f"dev.py: {e}")
        return 1
    except KeyboardInterrupt:
        say(
            "\ndev.py: interrupted. Server jobs already submitted keep running; the box keeps its mode."
        )
        return 130


if __name__ == "__main__":
    sys.exit(main())
