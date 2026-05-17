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
clobber each other (last writer wins), but ``set_current`` uses
``os.replace(tmp, path)`` so a torn read is impossible by construction
— the readable file is always either the previous fully-written
version or the new fully-written version. Readers still fall back to
"no cursor" if the file is malformed, which is the correct behaviour
for the unlikely case of an external hand-edit gone wrong.

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
    except FileNotFoundError:
        # The common case for a fresh install — no cursor yet. Don't log.
        return None
    except (OSError, json.JSONDecodeError) as exc:
        # File exists but is unreadable/corrupt. Something wrote a bad
        # file — that's surprising enough to warrant a WARN (not debug)
        # so an operator chasing a "next press restarts from the
        # beginning" report sees the cause without having to enable
        # debug logging first. FileNotFoundError stays silent above
        # (the fresh-install common case). PR 142 review (claude[bot]
        # item #5, second review).
        log.warning("scene cursor unreadable, treating as absent: %s", exc)
        return None
    name = data.get("current")
    return name if isinstance(name, str) and name else None


def set_current(name: str | None) -> None:
    """Persist the last-applied scene name. ``None`` clears the cursor.

    Atomic replace so a crash mid-write can't leave a half-truncated
    file that ``get_current()`` would then read as garbage.

    Tmp-file naming: ``.cursor.json`` + ``.tmp`` = ``.cursor.json.tmp``,
    which still starts with ``.``. This is load-bearing — the scenes
    directory is enumerated by ``scenes.list_scenes`` for the scene
    library; if the tmp file (or the cursor file itself) didn't start
    with a dot, ``list_scenes`` would try to parse it as a scene and
    fail. Tested by ``test_list_scenes_skips_dotfiles`` in tests/. If
    you change ``_CURSOR_FILENAME`` to something not dot-prefixed, also
    add an explicit denylist in ``list_scenes``. PR 142 review."""
    path = _cursor_path()
    tmp = path.with_suffix(path.suffix + ".tmp")
    try:
        tmp.write_text(json.dumps({"current": name}))
        os.replace(tmp, path)
    except OSError as exc:
        log.warning("Failed to persist scene cursor: %s", exc)
        with contextlib.suppress(OSError):
            tmp.unlink(missing_ok=True)
