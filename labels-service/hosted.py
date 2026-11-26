"""Building blocks for the hosted (public) drum reviewer. server.py wires them together.

* JsonLog: one JSON object per line on stdout, for the log pipeline.
* Salt.ip_hash: a keyed hash of the client address, so logs can tell clients apart without
  recording an address.
* RateLimiter: token buckets per (kind, client).
* Checkout: the service's own clone of the corpus, refreshed from GitHub.
* Publication: what the corpus should get from the shared labels (the corpus pulls it: the service
  holds no credential), what landed, and adopting changes made on main from outside.

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


class Publication:
    """What the corpus should get from the shared labels, and what it already has.

    The service holds no credential at all: it never writes to GitHub. blinky-corpus's workflow
    labels-sync.yml (scheduled, so it runs the corpus's own trusted code) pulls `export()` from the
    public API, checks the files with the corpus's label checks, commits them as one batch on a
    labels/auto-* branch and auto-merges it. A compromised service can only offer bad label files,
    which those checks exist for.

    `export()` lists every edited track whose labels differ from main's file and pass the
    anti-vandalism checks, and says whether it's `ready`: nobody has edited for `quiet_s`, or it's
    `max_wait_s` since the first unpublished edit while people keep editing.

    `tick()` (after each refresh of the clone) notices a track's labels landing on main (published),
    and makes a track take main's version when main's file changed from outside: an owner's edit or a
    revert. "Ours" is any version we exported; before one of ours has landed, also the version the
    state started from. Git history is the undo."""

    def __init__(self, checkout: Checkout, state_file: Path, *, base: str = "main",
                 quiet_s: float = 180.0, max_wait_s: float = 900.0, log: JsonLog | None = None,
                 clock: Callable[[], float] = time.time):
        self.co, self.base, self.quiet_s, self.max_wait_s = checkout, base, quiet_s, max_wait_s
        self.log, self.clock, self.state_file = log or JsonLog(), clock, state_file
        self.st: dict[str, Any] = {"exported": {}, "published": {}, "refused": {}, "dirty_since": None}
        if state_file.is_file():
            self.st.update(json.loads(state_file.read_text()))
        self._lock = threading.Lock()

    def _save(self) -> None:
        atomic_write_text(self.state_file, json.dumps(self.st, indent=1))

    def _main_blob(self, path: str) -> str | None:
        out = self.co.git("ls-tree", f"refs/remotes/origin/{self.base}", "--", path).split()
        return out[2] if len(out) >= 3 else None

    def status(self, path: str, text: str | None) -> dict[str, Any]:
        """For the page: when this track's labels were published (landed on main)."""
        pub = self.st["published"].get(path)
        if text is None or (pub and pub[0] == git_blob_sha(text.encode())):
            return {"published": pub[1] if pub else None, "unpublished": False}
        return {"published": pub[1] if pub else None, "unpublished": True}

    def export(self, tracks: dict[str, str], edited_at: float | None,
               check: Callable[[str, str, str | None], list[str]]) -> dict[str, Any]:
        """{"ready", "tracks": {path: text}, "held": {path: problems}} for the corpus workflow."""
        with self._lock:
            now = self.clock()
            out, held = {}, {}
            for p, text in sorted(tracks.items()):
                b = git_blob_sha(text.encode())
                mb = self._main_blob(p)
                if b == mb:
                    continue
                problems = check(p, text, self.co.git("cat-file", "blob", mb) if mb else None)
                if problems:
                    held[p] = problems
                    if self.st["refused"].get(p) != b:  # log each refused version once
                        self.st["refused"][p] = b
                        self.log.emit("publish_refused", path=p, problems=problems)
                    continue
                out[p] = text
                ex = self.st["exported"].setdefault(p, [])
                if b not in ex:
                    ex.append(b)
                    del ex[:-50]
            if out:
                self.st["dirty_since"] = self.st["dirty_since"] or now
            else:
                self.st["dirty_since"] = None
            quiet = edited_at is None or now - edited_at >= self.quiet_s
            ready = bool(out) and (quiet or now - (self.st["dirty_since"] or now) >= self.max_wait_s)
            self._save()
            return {"ready": ready, "tracks": out, "held": held}

    def tick(self, tracks: dict[str, tuple[str, str | None]], adopt: Callable[[str, str | None], None]) -> None:
        """`tracks`: {path: (current file text, blob of main's file the state started from)} of every
        edited track. `adopt(path, main_text)`: the track takes main's version (None: main has no file
        any more, so back to its seed)."""
        with self._lock:
            for p, (text, base_blob) in tracks.items():
                mb = self._main_blob(p)
                mine = git_blob_sha(text.encode())
                exported = set(self.st["exported"].get(p, []))
                pub = self.st["published"].get(p)
                if mb == mine:
                    if not pub or pub[0] != mb:
                        self.st["published"][p] = [mb, now_iso()]
                        self.log.emit("published", path=p)
                    continue
                if mb in exported:  # an earlier version of ours landed; the newer one follows
                    self.st["published"][p] = [mb, now_iso()]
                    continue
                ours_before_landing = base_blob if pub is None else None
                if mb is None:
                    if pub is not None:  # a file of ours was deleted on main: back to the seed
                        adopt(p, None)
                        self.st["published"].pop(p, None)
                        self.st["exported"].pop(p, None)
                        self.log.emit("adopted_main", path=p, deleted=True)
                    continue
                if mb != ours_before_landing:  # changed on main from outside: it wins
                    adopt(p, self.co.git("cat-file", "blob", mb))
                    self.st["published"][p] = [mb, now_iso()]
                    self.st["exported"].pop(p, None)
                    self.log.emit("adopted_main", path=p)
            self._save()
