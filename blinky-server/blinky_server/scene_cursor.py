"""Persistent cursor for the scene library.

Tracks the *last-applied* scene **by name** so that adds, deletes, and
reorders of the scene library don't randomly shift the cursor relative
to the operator's mental model:

  - Press "next" → cursor moves to the alphabetically-next scene.
  - Rename the current scene → cursor stays on it (we re-find by name).
  - Delete the current scene → cursor restarts from the beginning of
    the list on the next "next" press.
  - Reorder scenes by renaming → no effect; the cursor finds its name
    in the new order.

Stored as a tiny JSON file in the scenes data dir for the same reasons
scenes themselves persist there (atomic replace via temp + rename,
survives server restart, inspectable by hand). Concurrent writers
clobber each other but the file is one line of JSON so a half-written
read is implausible; readers fall back to "no cursor" if the file is
malformed, which is the correct behaviour anyway.

The cursor is set by:

  - POST /api/scenes/next            (advances + applies)
  - POST /api/scenes/previous        (rewinds + applies)
  - POST /api/scenes/{name}/apply    (also updates cursor so the next
                                       /next press follows from what
                                       was last applied via the Hub UI)
"""

from __future__ import annotations

import contextlib
import json
import logging
import os
from pathlib import Path

from .paths import scenes_dir

log = logging.getLogger(__name__)

_CURSOR_FILENAME = ".cursor.json"


def _cursor_path() -> Path:
    return scenes_dir() / _CURSOR_FILENAME


def get_current() -> str | None:
    """Return the last-applied scene name, or None if no scene has been
    applied yet on this install (or the cursor file is malformed)."""
    try:
        data = json.loads(_cursor_path().read_text())
    except (OSError, json.JSONDecodeError):
        return None
    name = data.get("current")
    return name if isinstance(name, str) and name else None


def set_current(name: str | None) -> None:
    """Persist the last-applied scene name. ``None`` clears the cursor.

    Atomic replace so a crash mid-write can't leave a half-truncated
    file that ``get_current()`` would then read as garbage."""
    path = _cursor_path()
    tmp = path.with_suffix(path.suffix + ".tmp")
    try:
        tmp.write_text(json.dumps({"current": name}))
        os.replace(tmp, path)
    except OSError as exc:
        log.warning("Failed to persist scene cursor: %s", exc)
        with contextlib.suppress(OSError):
            tmp.unlink(missing_ok=True)
