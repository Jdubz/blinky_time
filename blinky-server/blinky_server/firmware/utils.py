"""Shared helpers for the firmware subsystem.

Single home for utilities used by more than one module under
``blinky_server/firmware/``. The first occupant is the version-key
lookup that used to be duplicated between ``verify.py`` and
``anomalies.py`` — the canonical key list is load-bearing, so a
change to it must land in both places, exactly the kind of implicit
coupling that drifts. One source of truth here.
"""

from __future__ import annotations

import logging
from typing import Any

log = logging.getLogger(__name__)


# Firmware build vintages have used several keys for the version string;
# every consumer must accept all of them. Order doesn't matter — first
# non-empty hit wins. Add to this tuple if a new firmware revision
# starts emitting under a different key. When/if the firmware settles
# on a canonical key, prune the others here and the warning below will
# stop firing.
VERSION_KEYS: tuple[str, ...] = ("version", "firmware_version", "fw_version", "build")


def extract_version(info: dict[str, Any]) -> str | None:
    """Pull the firmware version string out of a ``json info`` response.

    Returns the first non-empty value found under any known key, or
    ``None`` if the dict has no recognized version field. When ``info``
    is non-empty but lacks every known key, logs at ``debug`` so the
    "we just shipped a build that reports under a new key" case is
    discoverable from journalctl without having to add the warning at
    every call site.
    """
    for key in VERSION_KEYS:
        v = info.get(key)
        if isinstance(v, str) and v:
            return v
    if info:
        log.debug(
            "handshake info has no recognized version key (tried %s); keys present: %s",
            list(VERSION_KEYS),
            list(info.keys()),
        )
    return None
