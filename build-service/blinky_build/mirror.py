"""The firmware repo's bare mirror and the trust check (docs/DEV_LOOP.md, "Trust").

The mirror fetches ONLY `+refs/heads/*:refs/heads/*` from GitHub and never fetches by
SHA. GitHub serves a fork PR's commits from the base repo by SHA (refs/pull/N/head), so
"the SHA exists" proves nothing; a commit is buildable only if it is an ancestor of
(or equal to) one of the repo's own branch heads, which only the owner can push.

Bundles from the box (an authenticated, trusted caller) are fetched into refs/dev/<id>/*,
never into refs/heads/*, so they can never make another commit look branch-trusted.
"""

import shutil
import threading
from pathlib import Path

from . import gitutil
from .bundle import parse_header
from .gitutil import GitError, git, out

HEADS_REFSPEC = "+refs/heads/*:refs/heads/*"


class Untrusted(Exception):
    """403: the commit is not an owner commit."""


class MissingPrerequisites(Exception):
    """409: a bundle needs commits the mirror's branches don't have."""


class BadInput(Exception):
    """400."""


class Mirror:
    def __init__(self, path, remote_url, repo):
        self.path = Path(path)
        self.remote_url = remote_url
        self.repo = repo
        self.lock = threading.RLock()

    # ------------------------------------------------------------------ setup ---

    def ensure(self):
        with self.lock:
            if not gitutil.is_repo(self.path):
                self.path.mkdir(parents=True, exist_ok=True)
                git("init", "--bare", "--quiet", self.path)
            # Rewritten every start: this file is ours alone, and nothing else may be in it
            # that changes what a fetch takes.
            g = ("--git-dir", self.path)
            for key in ("remote.origin.fetch", "remote.origin.url"):
                git(*g, "config", "--unset-all", key, check=False)
            git(*g, "config", "remote.origin.url", self.remote_url)
            git(*g, "config", "remote.origin.fetch", HEADS_REFSPEC)
            git(*g, "config", "remote.origin.tagOpt", "--no-tags")
            git(*g, "config", "core.hooksPath", "/dev/null")
            git(*g, "config", "gc.autoDetach", "false")

    def fetch_heads(self):
        """Fetch the branch heads from the remote. Return None, or the error text.

        A failed fetch is not fatal: the branches already in the mirror were pushed by the
        owner when they were fetched, so a commit on them is still an owner commit."""
        with self.lock:
            try:
                git(
                    "--git-dir", self.path, "fetch", "--quiet", "--prune", "--no-tags",
                    "--no-recurse-submodules", "--no-write-fetch-head", "origin", HEADS_REFSPEC,
                    timeout=300,
                )
                return None
            except (GitError, OSError) as e:
                return str(e)
            except Exception as e:  # subprocess.TimeoutExpired
                return f"fetch failed: {e}"

    # ------------------------------------------------------------------ trust ---

    def has_commit(self, sha):
        r = git("--git-dir", self.path, "cat-file", "-e", f"{sha}^{{commit}}", check=False)
        return r.returncode == 0

    def refs_containing(self, sha, *patterns):
        """Refs under ``patterns`` whose history includes ``sha`` (which must exist)."""
        text = out(
            "--git-dir", self.path, "for-each-ref", "--contains", sha,
            "--format=%(refname)", *patterns,
        )
        return [line for line in text.splitlines() if line]

    def branches_containing(self, sha):
        if not self.has_commit(sha):
            return []
        return self.refs_containing(sha, "refs/heads/")

    def require_branch_commit(self, sha):
        """Raise Untrusted unless ``sha`` is on one of the repo's own branches."""
        with self.lock:
            if not self.has_commit(sha):
                raise Untrusted(
                    f"{sha[:12]} is not on any branch of {self.repo}. This service fetches only "
                    "the repo's branch heads (refs/heads/*), never a commit by SHA, so a fork "
                    "PR's commit (refs/pull/*) is refused. Push it to a branch of the repo, "
                    "or send it as a bundle."
                )
            if not self.branches_containing(sha):
                raise Untrusted(
                    f"{sha[:12]} is known here but is not on any branch of {self.repo} "
                    "(only from an earlier bundle, or a branch that has since moved). Push it "
                    "to a branch, or send it as a bundle."
                )

    # ---------------------------------------------------------------- bundles ---

    def import_bundle(self, bundle_path, build_id, sha):
        """Check and fetch a bundle into refs/dev/<build_id>/<n>; ``sha`` must then be on
        those refs or on a branch. Return the refs/dev prefix.

        Raises BadInput (malformed, or sha not in it), MissingPrerequisites."""
        try:
            b = parse_header(bundle_path)
        except Exception as e:
            raise BadInput(f"bundle: {e}") from None
        with self.lock:
            missing, untrusted = [], []
            for p in b.prerequisites:
                if not self.has_commit(p):
                    missing.append(p)
                elif not self.branches_containing(p):
                    untrusted.append(p)
            if missing or untrusted:
                parts = []
                if missing:
                    parts.append("not in the mirror: " + ", ".join(m[:12] for m in missing))
                if untrusted:
                    parts.append(
                        "in the mirror but on no branch: " + ", ".join(u[:12] for u in untrusted)
                    )
                raise MissingPrerequisites(
                    f"bundle prerequisites must be commits on a branch of {self.repo} ("
                    + "; ".join(parts)
                    + "). Build the bundle against a pushed branch, e.g. "
                    "`git bundle create x.bundle origin/staging..HEAD`."
                )
            r = git("--git-dir", self.path, "bundle", "verify", "--quiet", bundle_path, check=False)
            if r.returncode != 0:
                raise BadInput(f"bundle failed verification: {r.stderr.strip()[:400]}")
            prefix = f"refs/dev/{build_id}"
            specs = [f"+{name}:{prefix}/{i}" for i, (_, name) in enumerate(b.refs)]
            try:
                git(
                    "-c", "fetch.fsckObjects=true", "--git-dir", self.path, "fetch", "--quiet",
                    "--no-tags", "--no-recurse-submodules", "--no-write-fetch-head",
                    str(bundle_path), *specs,
                )
            except GitError as e:
                self.delete_refs(prefix)
                raise BadInput(f"bundle could not be unbundled: {e.stderr[:400]}") from None
            ok = self.has_commit(sha) and (
                self.refs_containing(sha, f"{prefix}/", "refs/heads/")
            )
            if not ok:
                self.delete_refs(prefix)
                raise BadInput(f"{sha[:12]} is in neither the bundle nor a branch of {self.repo}")
            return prefix

    def delete_refs(self, prefix):
        with self.lock:
            refs = out("--git-dir", self.path, "for-each-ref", "--format=%(refname)", f"{prefix}/")
            for ref in refs.splitlines():
                git("--git-dir", self.path, "update-ref", "-d", ref, check=False)

    # --------------------------------------------------------------- hand-off ---

    def write_checkout(self, sha, dest, dev_prefix=None):
        """A checkout of ``sha`` with history at ``dest``: the branch heads (and the bundle's
        refs) fetched by NAME from the mirror, never by SHA, then a detached checkout.

        Built in the api's private storage and then copied to ``dest`` (symlinks copied as
        links, never followed): no git command runs on the shared job volume, which the build
        can write to (a planted filter or config there would run in this container)."""
        dest = Path(dest)
        stage = self.path.parent / "stage" / dest.parent.name
        with self.lock:
            shutil.rmtree(stage, ignore_errors=True)
            stage.parent.mkdir(parents=True, exist_ok=True)
            try:
                git("init", "--quiet", stage)
                git("-C", stage, "config", "core.hooksPath", "/dev/null")
                specs = ["+refs/heads/*:refs/remotes/origin/*"]
                if dev_prefix:
                    specs.append(f"+{dev_prefix}/*:refs/remotes/dev/*")
                git(
                    "-C", stage, "fetch", "--quiet", "--no-tags", "--no-recurse-submodules",
                    "--no-write-fetch-head", str(self.path.resolve()), *specs,
                )
                git("-C", stage, "checkout", "--quiet", "--force", "--detach", sha)
                head = out("-C", stage, "rev-parse", "HEAD")
                if head != sha:
                    raise RuntimeError(f"checkout of {sha} produced {head}")
                shutil.copytree(stage, dest, symlinks=True)
            finally:
                shutil.rmtree(stage, ignore_errors=True)

    # ------------------------------------------------------------------ checks ---

    def gitlink(self, sha, path):
        """The commit a submodule at ``path`` is pinned to in ``sha``'s tree, or None."""
        text = out("--git-dir", self.path, "ls-tree", sha, "--", path)
        for line in text.splitlines():
            meta, _, name = line.partition("\t")
            parts = meta.split()
            if name == path and len(parts) == 3 and parts[:2] == ["160000", "commit"]:
                return parts[2]
        return None

    def write_tree(self, sha, dest):
        """``sha``'s tree (no history, no .git) at ``dest``, by `git archive` from the mirror:
        unpacked in the api's private storage with tarfile's "data" filter (no absolute or
        escaping paths, no links out of the tree, no devices), then copied to ``dest``."""
        import subprocess
        import tarfile

        dest = Path(dest)
        stage = self.path.parent / "stage-tree" / dest.parent.name
        with self.lock:
            shutil.rmtree(stage, ignore_errors=True)
            stage.mkdir(parents=True)
            tar_path = stage.with_suffix(".tar")
            try:
                cmd = ["git"]
                for c in gitutil.SAFE_CONFIG:
                    cmd += ["-c", c]
                cmd += ["--git-dir", str(self.path), "archive", "--format=tar", sha]
                with open(tar_path, "wb") as f:
                    r = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE,
                                       env=gitutil.safe_env(), timeout=600)
                if r.returncode != 0:
                    err = r.stderr.decode(errors="replace")
                    raise GitError(["archive", sha], r.returncode, err)
                with tarfile.open(tar_path) as tar:
                    tar.extractall(stage, filter="data")
                shutil.copytree(stage, dest, symlinks=True)
            finally:
                shutil.rmtree(stage, ignore_errors=True)
                tar_path.unlink(missing_ok=True)
