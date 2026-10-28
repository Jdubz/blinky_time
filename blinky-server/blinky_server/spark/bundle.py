"""The code bundle a training `gpu_task` runs: `ml-training/` at a commit, as a reproducible tar.gz.

The Spark returns the bundle's sha256 as `code_sha256` in the job result, and that hash is the
model's provenance: it says exactly which code trained it. So the bundle is built from a commit,
never from the working tree, and two builds of the same commit are byte-identical (sorted entries,
fixed mtime/owner/modes, gzip without a timestamp). A tree with uncommitted changes under
`ml-training/` is refused rather than silently training the last commit.
"""

from __future__ import annotations

import gzip
import hashlib
import io
import subprocess
import tarfile
from dataclasses import dataclass
from pathlib import Path

from .client import SparkError

SUBDIR = "ml-training"

# The platform's cap on expansion (my-spark gpu_task, `code` is code, not data).
MAX_EXPANDED_BYTES = 256 * 1024 * 1024


@dataclass(frozen=True)
class Bundle:
    path: Path
    sha256: str
    commit: str
    names: frozenset[str]


def _git(repo: Path, *args: str) -> bytes:
    try:
        r = subprocess.run(["git", *args], cwd=repo, capture_output=True, check=True, timeout=120)
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        detail = getattr(e, "stderr", b"") or b""
        raise SparkError(f"git {' '.join(args)}: {e} {detail.decode(errors='replace')}") from None
    return r.stdout


def build(repo: Path, out_dir: Path) -> Bundle:
    dirty = _git(repo, "status", "--porcelain", "--untracked-files=no", "--", SUBDIR).decode()
    if dirty.strip():
        raise SparkError(
            f"{SUBDIR}/ has uncommitted changes; commit them so the bundle (and the model's "
            f"code_sha256) is the code that trains:\n{dirty.rstrip()}"
        )
    commit = _git(repo, "rev-parse", "HEAD").decode().strip()
    # `HEAD:ml-training` archives the subtree with paths relative to it, so the entry point is
    # `spark_entry.py` and configs are `configs/<name>.yaml` inside the bundle.
    archive = _git(repo, "archive", "--format=tar", f"HEAD:{SUBDIR}")
    buf = io.BytesIO()
    with (
        tarfile.open(fileobj=io.BytesIO(archive)) as raw,
        tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar,
    ):
        members = sorted(
            (m for m in raw.getmembers() if m.isfile() or m.issym()), key=lambda m: m.name
        )
        expanded = sum(m.size for m in members)
        if expanded > MAX_EXPANDED_BYTES:
            raise SparkError(f"bundle expands to {expanded} bytes, over the platform's 256 MiB cap")
        for m in members:
            info = tarfile.TarInfo(m.name)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if m.issym():
                info.type, info.linkname, info.mode = tarfile.SYMTYPE, m.linkname, 0o777
                tar.addfile(info)
                continue
            info.size = m.size
            info.mode = 0o755 if m.mode & 0o111 else 0o644
            tar.addfile(info, raw.extractfile(m))
    gz = io.BytesIO()
    with gzip.GzipFile(fileobj=gz, mode="wb", mtime=0, filename="") as f:
        f.write(buf.getvalue())
    data = gz.getvalue()

    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"blinky-ml-{commit[:12]}.tar.gz"
    path.write_bytes(data)
    return Bundle(
        path, hashlib.sha256(data).hexdigest(), commit, frozenset(m.name for m in members)
    )
