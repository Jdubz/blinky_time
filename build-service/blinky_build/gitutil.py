"""Run git without any configuration or code the repository could supply.

Every call gets an environment with no system or global config and these overrides:
no hooks (core.hooksPath=/dev/null), no fsmonitor, no ext:: transport, no submodule
recursion. A repository's own .git/config is written only by this service.
"""

import os
import subprocess
import tempfile
from pathlib import Path

SAFE_CONFIG = (
    "core.hooksPath=/dev/null",
    "core.fsmonitor=false",
    "protocol.ext.allow=never",
    "submodule.recurse=false",
    "fetch.recurseSubmodules=false",
    "gc.autoDetach=false",
    "advice.detachedHead=false",
    "init.defaultBranch=unused",
)

_HOME = None


class GitError(Exception):
    def __init__(self, args, returncode, stderr):
        self.returncode = returncode
        self.stderr = (stderr or "").strip()
        super().__init__(f"git {' '.join(args)} failed ({returncode}): {self.stderr[:800]}")


def _home():
    # An empty HOME of our own: git never reads a user's ~/.gitconfig or credentials.
    global _HOME
    if _HOME is None:
        _HOME = tempfile.mkdtemp(prefix="blinky-build-git-home-")
    return _HOME


def safe_env():
    return {
        "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
        "HOME": _home(),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_ASKPASS": "/bin/false",
        "SSH_ASKPASS": "/bin/false",
    }


def git(*args, cwd=None, check=True, timeout=600, input=None):
    """Run git with SAFE_CONFIG; return the CompletedProcess (text). Raise GitError on failure."""
    cmd = ["git"]
    for c in SAFE_CONFIG:
        cmd += ["-c", c]
    cmd += [str(a) for a in args]
    r = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=safe_env(),
        capture_output=True,
        text=True,
        timeout=timeout,
        input=input,
    )
    if check and r.returncode != 0:
        raise GitError([str(a) for a in args], r.returncode, r.stderr)
    return r


def out(*args, **kw):
    return git(*args, **kw).stdout.strip()


def is_repo(path):
    p = Path(path)
    return (p / "HEAD").is_file() or (p / ".git").exists()
