"""Immutable firmware images, and which of them may be auto-recovery images (#251).

Before #251 the server kept one shared ``firmware.hex`` that every upload
overwrote, and auto-recovery pointed at that path. An upload, even one whose
deploy then stopped, silently changed what recovery would flash to the
whitelisted devices, and nothing checked that it was a stable release.

Now every image is stored once under its sha256 (``images/<sha256>.hex``) and
never rewritten, with a small metadata file beside it. An image is
*stable* only when this server verified it: its bytes match the hex of a
promoted stable GitHub release (``tools/firmware_release.py download``, the
check deploy.sh and the deploy pipeline use). Only stable images can arm
auto-recovery (``FleetManager.set_recovery_firmware``), and the binding
records the sha256, which is re-checked before any recovery flash.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import logging
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..paths import firmware_dir

log = logging.getLogger(__name__)

IMAGES_SUBDIR = "images"
# Keep this many images; the one armed for recovery is never pruned.
KEEP_IMAGES = 10


@dataclass(frozen=True)
class FirmwareImage:
    path: Path
    sha256: str
    version: str | None
    stable: bool  # bytes verified against a promoted stable release by this server
    source: str  # who stored it: "upload", "deploy", ...


def images_dir() -> Path:
    d = firmware_dir() / IMAGES_SUBDIR
    d.mkdir(parents=True, exist_ok=True)
    return d


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def _meta_path(hex_path: Path) -> Path:
    return hex_path.with_suffix(".json")


def store(
    content: bytes, *, version: str | None, stable: bool, source: str, keep: str | None = None
) -> FirmwareImage:
    """Store ``content`` immutably and return its record.

    The same bytes always land at the same path. Storing them again never
    rewrites the image, and only ever upgrades its metadata to stable (a
    stable verification can't be undone by a later unverified upload of the
    same bytes). ``keep``: a path the pruning must not remove (the image
    armed for recovery).
    """
    digest = sha256_bytes(content)
    hex_path = images_dir() / f"{digest}.hex"
    if not hex_path.exists():
        # Write-then-rename: the final path only ever holds complete bytes.
        fd, tmp = tempfile.mkstemp(dir=hex_path.parent, suffix=".tmp")
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(content)
            os.replace(tmp, hex_path)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
        os.chmod(hex_path, 0o444)
    else:
        os.utime(hex_path)  # recently used: pruning goes oldest-first
    prev = _read_meta(hex_path) or {}
    prev_version = prev.get("version")
    meta_version: str | None = version or (prev_version if isinstance(prev_version, str) else None)
    meta_stable = bool(stable or prev.get("stable"))
    meta_source = source if (not prev or stable) else str(prev.get("source", source))
    meta = {
        "sha256": digest,
        "version": meta_version,
        "stable": meta_stable,
        "source": meta_source,
        "stored_at": datetime.datetime.now(datetime.UTC).isoformat(),
    }
    _meta_path(hex_path).write_text(json.dumps(meta))
    _prune(keep={hex_path, *([Path(keep)] if keep else [])})
    return FirmwareImage(hex_path, digest, meta_version, meta_stable, meta_source)


def _read_meta(hex_path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(_meta_path(hex_path).read_text())
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def load(path: str | Path) -> FirmwareImage | None:
    """The record for an image in the store, or None if ``path`` isn't one,
    or its bytes no longer match the sha256 it's named after."""
    hex_path = Path(path)
    try:
        if hex_path.resolve().parent != images_dir().resolve() or hex_path.suffix != ".hex":
            return None
    except OSError:
        return None
    meta = _read_meta(hex_path)
    if not meta or not hex_path.is_file():
        return None
    expected = hex_path.stem
    if meta.get("sha256") != expected or sha256_file(hex_path) != expected:
        log.error("Firmware image %s does not match its sha256: refusing to use it", hex_path)
        return None
    return FirmwareImage(
        hex_path,
        expected,
        meta.get("version"),
        bool(meta.get("stable")),
        str(meta.get("source", "?")),
    )


def _prune(keep: set[Path]) -> None:
    images = sorted(images_dir().glob("*.hex"), key=lambda p: p.stat().st_mtime, reverse=True)
    keep_resolved = {k.resolve() for k in keep}
    for old in images[KEEP_IMAGES:]:
        if old.resolve() in keep_resolved:
            continue
        try:
            os.chmod(old, 0o644)
            old.unlink()
            _meta_path(old).unlink(missing_ok=True)
        except OSError:
            log.warning("Could not prune old firmware image %s", old)


def verify_stable(content: bytes, tag: str, download_dir: Path) -> tuple[bool, str]:
    """Is ``content`` exactly the hex of stable release ``tag``?

    Downloads and verifies the release with ``tools/firmware_release.py``
    (deploy_pipeline.fetch_stable_release), then compares sha256. Returns
    (ok, reason); never raises for a non-match or an unreachable GitHub.
    """
    from .deploy_pipeline import DeployError, fetch_stable_release

    try:
        rel = fetch_stable_release(tag, download_dir)
    except DeployError as e:
        return False, str(e)
    released = sha256_file(Path(rel["hex"]))
    if released != sha256_bytes(content):
        return False, f"bytes differ from stable release {tag}'s hex"
    return True, f"matches stable release {rel['tag']} ({rel['version']})"
