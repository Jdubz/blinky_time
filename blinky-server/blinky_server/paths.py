"""Persistent filesystem paths used across the server.

State that must survive reboots lives under ``~/.local/share/blinky-server/``
(XDG_DATA_HOME if set). Anything under ``/tmp`` is lost on reboot — never
put scene definitions, uploaded firmware, or recovery pointers there if
the cart needs to come back up unattended.
"""

from __future__ import annotations

import os
from pathlib import Path


def data_dir() -> Path:
    """Root of persistent server state.

    Created on first access. All callers should build subpaths off this
    rather than reaching for ``Path.home()`` themselves — keeps the layout
    discoverable and the XDG override honored in one place.
    """
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    d = Path(base) / "blinky-server"
    d.mkdir(parents=True, exist_ok=True)
    return d


def firmware_dir() -> Path:
    """Uploaded firmware + metadata. Stable across reboots so a device
    stuck in DFU bootloader can auto-recover even after a power cycle."""
    d = data_dir() / "firmware"
    d.mkdir(parents=True, exist_ok=True)
    return d


def scenes_dir() -> Path:
    """Named fleet scenes."""
    d = data_dir() / "scenes"
    d.mkdir(parents=True, exist_ok=True)
    return d
