"""Persistent filesystem paths used across the server.

State that must survive reboots lives under ``~/.local/share/blinky-server/``
(XDG_DATA_HOME if set). Anything under ``/tmp`` is lost on reboot — never
put scene definitions, uploaded firmware, or recovery pointers there if
the cart needs to come back up unattended.

Each ``*_dir()`` function caches its result so repeated calls don't issue
a ``mkdir`` syscall every time — these are called from hot paths
(``_firmware_meta_path`` / ``_firmware_hex_path`` on every metadata
read/write). PR #140 review.
"""

from __future__ import annotations

import os
from pathlib import Path

# Module-level caches. Populated lazily by the corresponding function.
# Tests that monkeypatch ``HOME`` or ``XDG_DATA_HOME`` after import should
# call ``_clear_cache()`` to force re-resolution.
_data_dir: Path | None = None
_firmware_dir: Path | None = None
_scenes_dir: Path | None = None


def _clear_cache() -> None:
    """Reset cached paths. Test-only — production code never calls this."""
    global _data_dir, _firmware_dir, _scenes_dir
    _data_dir = None
    _firmware_dir = None
    _scenes_dir = None


def data_dir() -> Path:
    """Root of persistent server state.

    Created on first access. All callers should build subpaths off this
    rather than reaching for ``Path.home()`` themselves — keeps the layout
    discoverable and the XDG override honored in one place.
    """
    global _data_dir
    if _data_dir is None:
        base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
        d = Path(base) / "blinky-server"
        d.mkdir(parents=True, exist_ok=True)
        _data_dir = d
    return _data_dir


def firmware_dir() -> Path:
    """Uploaded firmware + metadata. Stable across reboots so a device
    stuck in DFU bootloader can auto-recover even after a power cycle."""
    global _firmware_dir
    if _firmware_dir is None:
        d = data_dir() / "firmware"
        d.mkdir(parents=True, exist_ok=True)
        _firmware_dir = d
    return _firmware_dir


def scenes_dir() -> Path:
    """Named fleet scenes."""
    global _scenes_dir
    if _scenes_dir is None:
        d = data_dir() / "scenes"
        d.mkdir(parents=True, exist_ok=True)
        _scenes_dir = d
    return _scenes_dir
