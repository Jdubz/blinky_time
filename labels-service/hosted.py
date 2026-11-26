"""Building blocks for the hosted (public) drum reviewer. server.py wires them together.

* JsonLog: one JSON object per line on stdout, for the log pipeline.
* Salt.ip_hash: a keyed hash of the client address, so logs can tell clients apart without
  recording an address.
* RateLimiter: token buckets per (kind, client).
* Checkout: the service's own clone of the corpus, refreshed from GitHub.
* AutoPublisher: publishes the tracks' shared labels to the corpus by itself, in batches: one commit
  on a labels/auto-<time> branch, pushed with a deploy key; a corpus workflow opens the pull request
  with auto-merge. PR state is read from the public API, without a token.

Python standard library only.
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import hmac
import json
import os
import re
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable, TextIO

NAME_PUNCT = " .'-_"  # besides letters, marks and digits in any script: no '@', markup or control chars


def now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()


def atomic_write_text(path: Path, text: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def git_blob_sha(data: bytes) -> str:
    """The sha git (and GitHub's contents API) gives a file with these bytes."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


# --- logging ------------------------------------------------------------------------------------


class JsonLog:
    def __init__(self, stream: TextIO | None = None):
        self.stream = stream or sys.stdout
        self._lock = threading.Lock()

    def emit(self, event: str, **fields: Any) -> None:
        rec = {"ts": now_iso(), "event": event, **{k: v for k, v in fields.items() if v is not None}}
        line = json.dumps(rec, ensure_ascii=False, separators=(",", ":"))
        with self._lock:
            self.stream.write(line + "\n")
            self.stream.flush()


class Salt:
    """The key for ip_hash, created once in the state directory (0600). Losing it only means hashes
    from before and after stop matching."""

    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_file():
            atomic_write_text(path, secrets.token_hex(32) + "\n", mode=0o600)
        self.key = bytes.fromhex(path.read_text().strip())

    def ip_hash(self, ip: str) -> str:
        return hmac.new(self.key, ip.encode(), hashlib.sha256).hexdigest()[:16]


# --- rate limits --------------------------------------------------------------------------------


class RateLimiter:
    """Token buckets. rules: {kind: (capacity, seconds to refill one token... as capacity per
    `period` seconds)}. allow() takes one token and returns (True, 0) or (False, retry_after_s)."""

    def __init__(self, rules: dict[str, tuple[int, float]], clock: Callable[[], float] = time.monotonic):
        self.rules = rules
        self.clock = clock
        self._b: dict[tuple[str, str], tuple[float, float]] = {}
        self._lock = threading.Lock()

    def allow(self, kind: str, key: str) -> tuple[bool, int]:
        cap, period = self.rules[kind]
        rate = cap / period
        now = self.clock()
        with self._lock:
            tokens, last = self._b.get((kind, key), (float(cap), now))
            tokens = min(float(cap), tokens + (now - last) * rate)
            if tokens >= 1:
                self._b[(kind, key)] = (tokens - 1, now)
                ok, wait = True, 0
            else:
                self._b[(kind, key)] = (tokens, now)
                ok, wait = False, max(1, int((1 - tokens) / rate + 0.999))
            if len(self._b) > 20_000:  # bound memory: forget buckets that have refilled
                for k, (t, l) in list(self._b.items()):
                    c, p = self.rules[k[0]]
                    if t + (now - l) * c / p >= c:
                        del self._b[k]
        return ok, wait


# --- the corpus checkout ---------------------------------------------------------------------------


class RWLock:
    """Many readers or one writer: requests read the checkout, a refresh rewrites it."""

    def __init__(self) -> None:
        self._c = threading.Condition()
        self._readers = 0
        self._writer = False

    def read(self) -> "_Guard":
        return _Guard(self._acquire_r, self._release_r)

    def write(self) -> "_Guard":
        return _Guard(self._acquire_w, self._release_w)

    def _acquire_r(self) -> None:
        with self._c:
            while self._writer:
                self._c.wait()
            self._readers += 1

    def _release_r(self) -> None:
        with self._c:
            self._readers -= 1
            self._c.notify_all()

    def _acquire_w(self) -> None:
        with self._c:
            while self._writer or self._readers:
                self._c.wait()
            self._writer = True

    def _release_w(self) -> None:
        with self._c:
            self._writer = False
            self._c.notify_all()


class _Guard:
    def __init__(self, a: Callable[[], None], r: Callable[[], None]):
        self.a, self.r = a, r

    def __enter__(self) -> None:
        self.a()

    def __exit__(self, *exc: Any) -> None:
        self.r()


class Checkout:
    """The service's own clone of the corpus (never anyone's working copy): cloned on first start,
    then fetched and hard-reset to origin/<branch> when it moves. Only data is read from it; the
    code that runs is the image's."""

    def __init__(self, root: Path, url: str, branch: str = "main", rw: RWLock | None = None,
                 log: JsonLog | None = None):
        self.root, self.url, self.branch = root, url, branch
        self.rw = rw or RWLock()
        self.log = log or JsonLog()

    def git(self, *args: str, input: str | None = None, env: dict[str, str] | None = None) -> str:
        return self._git(*args, input=input, env=env)

    def _git(self, *args: str, cwd: Path | None = None, input: str | None = None,
             env: dict[str, str] | None = None) -> str:
        env = {**os.environ, "GIT_TERMINAL_PROMPT": "0", **(env or {})}
        r = subprocess.run(["git", *args], cwd=cwd or self.root, capture_output=True, text=True,
                           timeout=600, env=env, input=input)
        if r.returncode:
            raise RuntimeError(f"git {' '.join(args[:2])} failed: {r.stderr.strip()[-400:]}")
        return r.stdout

    def ensure(self) -> None:
        if (self.root / ".git").is_dir():
            return
        self.root.parent.mkdir(parents=True, exist_ok=True)
        with self.rw.write():
            self._git("clone", "--branch", self.branch, "--single-branch", "--no-tags", self.url,
                      str(self.root), cwd=self.root.parent)
        self.log.emit("checkout_cloned", head=self.head())

    def head(self) -> str:
        return self._git("rev-parse", "HEAD").strip()

    def refresh(self) -> bool:
        """Fetch; when origin moved, reset the working tree to it. True if it changed."""
        self._git("fetch", "--no-tags", "--prune", "origin", f"+refs/heads/{self.branch}:"
                  f"refs/remotes/origin/{self.branch}")
        new = self._git("rev-parse", f"refs/remotes/origin/{self.branch}").strip()
        if new == self.head():
            return False
        with self.rw.write():
            self._git("reset", "--hard", "--quiet", new)
            self._git("clean", "-fdq")
        self.log.emit("checkout_refreshed", head=new)
        return True


# --- publishing -----------------------------------------------------------------------------------


class GitHubError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(f"GitHub {status}: {message}")
        self.status, self.message = status, message


class PublicGitHub:
    """Unauthenticated reads of the public corpus repo's pull requests (60 an hour per address are
    allowed; the publisher needs one every couple of minutes at most). No token is held."""

    def __init__(self, repo: str, api: str = "https://api.github.com", timeout: float = 15):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
            raise ValueError(f"bad repo {repo!r}")
        self.repo, self.api, self.timeout = repo, api.rstrip("/"), timeout

    def pr_for_branch(self, branch: str) -> dict[str, Any] | None:
        owner = self.repo.split("/")[0]
        url = (f"{self.api}/repos/{self.repo}/pulls?state=all&per_page=5&head="
               f"{urllib.parse.quote(owner + ':' + branch, safe='')}")
        req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                   "User-Agent": "blinky-drum-reviewer"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                prs = json.loads(r.read() or b"[]")
        except urllib.error.HTTPError as e:
            raise GitHubError(e.code, e.read()[:200].decode(errors="replace")) from None
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise GitHubError(0, f"unreachable: {e}") from None
        return prs[0] if prs else None


class AutoPublisher:
    """Publishes the tracks' shared labels to the corpus by itself.

    Once nobody has edited for `quiet_s`, or `max_wait_s` after the first unpublished edit while
    people keep editing, every track whose labels differ from main's file goes into one commit on
    top of main, on a branch labels/auto-<UTC time>, pushed with the service's deploy key (write on
    the corpus repo only; over ssh.github.com:443 with GitHub's host keys pinned). The corpus workflow
    labels-pr.yml opens the PR and turns on auto-merge; the required "Scoring tests" check (label
    checks: schema, no empty class, no mass deletion) gates the merge. Partial progress is published:
    the scorer only uses a human file once it's complete.

    While that PR is open, later publishes rebuild the same branch (main + every changed file) and
    force-push it, also when main moves (main requires up-to-date branches). A PR that fails CI stays
    open for the owner; editing goes on. When the batch lands on main the branch is deleted and the
    next publish starts a new one. A track whose file changes on main from outside (an owner's edit
    or revert) takes main's version: git history is the undo.

    The commits are built with git plumbing and a scratch index: the clone's working tree is never
    written."""

    def __init__(self, checkout: Checkout, push_url: str, gh: PublicGitHub, state_file: Path, *,
                 key_file: Path | None = None, known_hosts: Path | None = None, base: str = "main",
                 quiet_s: float = 180.0, max_wait_s: float = 900.0, poll_every: float = 120.0,
                 email: str = "drum-reviewer@blinky-time.joshwentworth.com",
                 log: JsonLog | None = None, clock: Callable[[], float] = time.time):
        self.co, self.push_url, self.gh, self.base = checkout, push_url, gh, base
        self.quiet_s, self.max_wait_s, self.poll_every = quiet_s, max_wait_s, poll_every
        self.email, self.log, self.clock = email, log or JsonLog(), clock
        self.state_file = state_file
        self.env: dict[str, str] = {}
        if push_url.startswith("ssh://"):
            if key_file is None or known_hosts is None:
                raise ValueError("an ssh push URL needs a deploy key and pinned known_hosts")
            self.env["GIT_SSH_COMMAND"] = (
                f"ssh -i {key_file} -o IdentitiesOnly=yes -o IdentityAgent=none -o BatchMode=yes "
                f"-o StrictHostKeyChecking=yes -o UserKnownHostsFile={known_hosts} "
                "-o GlobalKnownHostsFile=/dev/null -o ConnectTimeout=20")
        self.st: dict[str, Any] = {"branch": None, "commit": None, "base": None, "files": {},
                                   "pr": None, "url": None, "published": {}, "refused": {}}
        if state_file.is_file():
            self.st.update(json.loads(state_file.read_text()))
        self._last_poll = 0.0
        self._dirty_since: float | None = None
        self._lock = threading.Lock()

    def _save(self) -> None:
        atomic_write_text(self.state_file, json.dumps(self.st, indent=1))

    def _git(self, *args: str, input: str | None = None, env: dict[str, str] | None = None) -> str:
        return self.co.git(*args, input=input, env={**self.env, **(env or {})})

    def _main(self) -> str:
        return self._git("rev-parse", f"refs/remotes/origin/{self.base}").strip()

    def _blob_at(self, commit: str, path: str) -> str | None:
        out = self._git("ls-tree", commit, "--", path).split()
        return out[2] if len(out) >= 3 else None

    def _commit(self, parent: str, blobs: dict[str, str], message: str) -> str:
        with tempfile.TemporaryDirectory() as d:
            idx = {"GIT_INDEX_FILE": str(Path(d) / "index")}
            self._git("read-tree", parent, env=idx)
            for path, blob in sorted(blobs.items()):
                self._git("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}", env=idx)
            tree = self._git("write-tree", env=idx).strip()
        who = {"GIT_AUTHOR_NAME": "blinky drum reviewer", "GIT_AUTHOR_EMAIL": self.email,
               "GIT_COMMITTER_NAME": "blinky drum reviewer", "GIT_COMMITTER_EMAIL": self.email}
        return self._git("commit-tree", tree, "-p", parent, "-F", "-", input=message, env=who).strip()

    def status(self, path: str, text: str | None) -> dict[str, Any]:
        """For the page: when this track's current labels were published (landed on main)."""
        if text is None:
            return {"published": None, "unpublished": False}
        blob = git_blob_sha(text.encode())
        rec = self.st["published"].get(path)
        if rec and rec[0] == blob:
            return {"published": rec[1], "unpublished": False}
        return {"published": rec[1] if rec else None, "unpublished": True}

    def tick(self, tracks: dict[str, tuple[str, str | None]], edited_at: float | None,
             adopt: Callable[[str, str], None], check: Callable[[str, str, str | None], list[str]]) -> None:
        """`tracks`: {path: (current file text, blob of main's file the state started from)} of every
        edited track. `adopt(path, main_text)` makes a track take main's version.
        `check(path, text, main_text)`: the label checks' problems."""
        with self._lock:
            main = self._main()
            now = self.clock()
            files = self.st["files"]
            # 1. The open batch: landed on main? closed without merging?
            if self.st["branch"]:
                if all(self._blob_at(main, p) == b for p, b in files.items()):
                    for p, b in files.items():
                        self.st["published"][p] = [b, now_iso()]
                    self.log.emit("batch_landed", branch=self.st["branch"], pr=self.st["pr"],
                                  tracks=len(files))
                    self._drop_branch()
                elif now - self._last_poll >= self.poll_every:
                    self._last_poll = now
                    try:
                        pr = self.gh.pr_for_branch(self.st["branch"])
                    except GitHubError as e:
                        pr = None
                        self.log.emit("pr_poll_failed", branch=self.st["branch"], error=e.message[:200])
                    if pr and pr.get("number") != self.st["pr"]:
                        self.st.update(pr=pr["number"], url=pr["html_url"])
                        self.log.emit("pr_opened", pr=pr["number"], branch=self.st["branch"])
                    if pr and pr.get("state") == "closed" and not pr.get("merged_at"):
                        self.log.emit("batch_closed", branch=self.st["branch"], pr=pr["number"])
                        self._drop_branch()
            # 2. A file changed on main from outside (not by us, and not what the state started from)
            #    wins: an owner's edit or revert. Git history is the undo.
            current: dict[str, str] = {}
            for p, (text, base_blob) in tracks.items():
                mb = self._blob_at(main, p)
                pub = self.st["published"].get(p)
                known = {base_blob, pub[0] if pub else None}
                if mb and mb not in known and p not in files and git_blob_sha(text.encode()) != mb:
                    text = self._git("cat-file", "blob", mb)
                    adopt(p, text)
                    self.st["published"][p] = [mb, now_iso()]
                    self.log.emit("adopted_main", path=p)
                current[p] = text
            # 3. What differs from main now.
            changed = {p: t for p, t in current.items() if git_blob_sha(t.encode()) != self._blob_at(main, p)}
            if not changed:
                self._dirty_since = None
                self._save()
                return
            self._dirty_since = self._dirty_since or now
            quiet = edited_at is None or now - edited_at >= self.quiet_s
            if not quiet and now - self._dirty_since < self.max_wait_s:
                self._save()
                return
            blobs: dict[str, str] = {}
            for p, t in changed.items():
                mb = self._blob_at(main, p)
                problems = check(p, t, self._git("cat-file", "blob", mb) if mb else None)
                b = git_blob_sha(t.encode())
                if problems:
                    if self.st["refused"].get(p) != b:  # log each refused version once
                        self.st["refused"][p] = b
                        self.log.emit("publish_refused", path=p, problems=problems)
                    continue
                self.st["refused"].pop(p, None)
                blobs[p] = self._git("hash-object", "-w", "--stdin", input=t).strip()
            if not blobs:
                self._save()
                return
            if self.st["branch"] and blobs == files and self.st["base"] == main:
                self._save()
                return  # the open PR already has exactly this
            self._push(main, blobs, now)
            self._dirty_since = None
            self._save()

    def _push(self, main: str, blobs: dict[str, str], now: float) -> None:
        names = [Path(p).name.split(".")[0] for p in sorted(blobs)]
        msg = (f"labels: drum reviewer autosave ({len(blobs)} track{'s' if len(blobs) != 1 else ''})\n\n"
               + "\n".join(f"- {n}" for n in names)
               + "\n\nPublished by the labels service (blinky_time labels-service/): the shared labels "
                 "of the tracks above, as they stand. Corpus CI checks the files; auto-merge merges "
                 "them.\n")
        commit = self._commit(main, blobs, msg)
        new = self.st["branch"] is None
        # Unique: the workflow never reopens a branch whose PR was closed.
        branch = self.st["branch"] or (f"labels/auto-{_dt.datetime.now(_dt.timezone.utc):%Y%m%d-%H%M%S}"
                                       f"-{secrets.token_hex(2)}")
        args = ["push", "--quiet"]
        if not new:
            args.append(f"--force-with-lease=refs/heads/{branch}:{self.st['commit']}")
        try:
            self._git(*args, self.push_url, f"{commit}:refs/heads/{branch}")
        except RuntimeError as e:
            self.log.emit("publish_failed", branch=branch, error=str(e)[-300:])
            return
        if new:
            self.st.update(pr=None, url=None)
        self.st.update(branch=branch, commit=commit, base=main, files=blobs)
        self.log.emit("published", branch=branch, tracks=len(blobs), names=names,
                      new_branch=new, commit=commit[:12])

    def _drop_branch(self) -> None:
        try:
            self._git("push", "--quiet", self.push_url, f":refs/heads/{self.st['branch']}")
        except RuntimeError:
            pass  # already gone (merged with delete, or deleted by hand)
        self.st.update(branch=None, commit=None, base=None, files={}, pr=None, url=None)
