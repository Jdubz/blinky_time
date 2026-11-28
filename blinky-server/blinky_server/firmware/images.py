"""Immutable firmware images, and which of them may be auto-recovery images (#251).

Before #251 the server kept one shared ``firmware.hex`` that every upload
overwrote, and auto-recovery pointed at that path. An upload, even one whose
deploy then stopped, silently changed what recovery would flash to the
whitelisted devices, and nothing checked that it was a stable release.

Now every image is stored once under its sha256 (``images/<sha256>.hex``, or ``.bin`` for an
ESP32-S3 merged flash image) and never rewritten, with a small metadata file beside it. Its
platform is read from its bytes (``platform_of``), never from a name: the flash orchestrator
refuses an image whose platform isn't the chip's. An image is
*stable* only when this server verified it: its bytes match the hex of a
promoted stable GitHub release (``hosttools/firmware_release.py download``, the
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
# An ESP image header (ESP-IDF esp_app_format.h): magic 0xE9, chip id (u16 LE) at byte 12.
ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9
SUFFIX = {"nrf52840": ".hex", "esp32s3": ".bin"}
# Keep this many images; the one armed for recovery is never pruned.
KEEP_IMAGES = 10


@dataclass(frozen=True)
class FirmwareImage:
    path: Path
    sha256: str
    version: str | None
    stable: bool  # bytes verified against a promoted stable release by this server
    source: str  # who stored it: "upload", "deploy", ...
    platform: str = "nrf52840"  # from its bytes (platform_of)
    # The same build's image for another platform, stored beside it: {platform: path}. A
    # release or dev build carries the nRF52840 hex and, once built, the ESP32-S3 image; a
    # deploy names the hex and each chip takes its own platform's image (for_platform).
    companions: dict[str, str] | None = None


def images_dir() -> Path:
    d = firmware_dir() / IMAGES_SUBDIR
    d.mkdir(parents=True, exist_ok=True)
    return d


def platform_of(content: bytes) -> str | None:
    """The platform an image is for, from its bytes: an Intel HEX file is the nRF52840's (the
    only hex this project builds), an ESP image at offset 0 with chip id 9 the ESP32-S3's
    (the merged flash image; hosttools/esp32_flash.py checks its structure before writing).
    None: neither (an ESP image for another chip included)."""
    if content[:1] == b":":
        return "nrf52840"
    if len(content) >= 24 and content[0] == ESP_IMAGE_MAGIC:
        chip = int.from_bytes(content[12:14], "little")
        return "esp32s3" if chip == ESP32S3_CHIP_ID else None
    return None


def platform_of_file(path: str | Path) -> str | None:
    """``platform_of`` for a file; None if it can't be read."""
    try:
        with open(path, "rb") as f:
            return platform_of(f.read(64))
    except OSError:
        return None


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
    content: bytes,
    *,
    version: str | None,
    stable: bool,
    source: str,
    keep: str | list[str] | None = None,
) -> FirmwareImage:
    """Store ``content`` immutably and return its record.

    The same bytes always land at the same path. Storing them again never
    rewrites the image, and only ever upgrades its metadata to stable (a
    stable verification can't be undone by a later unverified upload of the
    same bytes). ``keep``: a path the pruning must not remove (the image
    armed for recovery; a list: each platform's).
    """
    platform = platform_of(content) or "nrf52840"  # unrecognised: kept as before (a .hex)
    digest = sha256_bytes(content)
    hex_path = images_dir() / f"{digest}{SUFFIX[platform]}"
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
    companions = prev.get("companions") if isinstance(prev.get("companions"), dict) else None
    meta: dict[str, Any] = {
        "sha256": digest,
        "version": meta_version,
        "stable": meta_stable,
        "source": meta_source,
        "platform": platform,
        "stored_at": datetime.datetime.now(datetime.UTC).isoformat(),
    }
    if companions:
        meta["companions"] = companions
    _meta_path(hex_path).write_text(json.dumps(meta))
    kept = [keep] if isinstance(keep, str) else list(keep or [])
    _prune(keep={hex_path, *(Path(k) for k in kept if k)})
    return FirmwareImage(
        hex_path, digest, meta_version, meta_stable, meta_source, platform, companions
    )


def link(primary: Path, other: Path) -> None:
    """Record ``other`` (an image in the store) as ``primary``'s companion for its platform:
    the same build, for another chip. Refuses two images of one platform or of different
    versions, so a chip never gets another build than the one the deploy named."""
    a, b = load(primary), load(other)
    if a is None or b is None:
        raise ValueError(f"link: {primary if a is None else other} is not an image in the store")
    if a.platform == b.platform:
        raise ValueError(f"link: both images are {a.platform}")
    if a.version != b.version:
        raise ValueError(f"link: versions differ ({a.version} vs {b.version})")
    meta = _read_meta(a.path) or {}
    companions = dict(meta.get("companions") or {})
    companions[b.platform] = str(b.path)
    meta["companions"] = companions
    _meta_path(a.path).write_text(json.dumps(meta))
    os.utime(b.path)  # used with its primary: pruned no earlier


def for_platform(path: str | Path, platform: str) -> FirmwareImage | None:
    """The image a chip of ``platform`` takes when a flash names ``path``: the image itself if
    it is that platform's, else its companion for it (verified like any stored image). None if
    the build has no image for that platform."""
    image = load(path)
    if image is None:
        return None
    if image.platform == platform:
        return image
    other = (image.companions or {}).get(platform)
    if not other:
        return None
    found = load(other)
    if found is None or found.platform != platform or found.version != image.version:
        log.error("Firmware image %s: its %s companion %s is not usable", path, platform, other)
        return None
    return found


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
        if hex_path.resolve().parent != images_dir().resolve() or hex_path.suffix not in (
            ".hex",
            ".bin",
        ):
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
    platform = meta.get("platform") or ("esp32s3" if hex_path.suffix == ".bin" else "nrf52840")
    companions = meta.get("companions")
    return FirmwareImage(
        hex_path,
        expected,
        meta.get("version"),
        bool(meta.get("stable")),
        str(meta.get("source", "?")),
        str(platform),
        dict(companions) if isinstance(companions, dict) else None,
    )


def _prune(keep: set[Path]) -> None:
    images = sorted(
        [*images_dir().glob("*.hex"), *images_dir().glob("*.bin")],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
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

    Downloads and verifies the release with ``hosttools/firmware_release.py``
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
