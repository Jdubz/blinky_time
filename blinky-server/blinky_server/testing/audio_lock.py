"""Audio lock — mutual exclusion for audio playback.

All devices share the same room, so only one process may play audio at a time.
File-based lock using atomic O_CREAT|O_EXCL, compatible with the TypeScript
and CJS implementations at /tmp/blinky-audio.lock.
"""

from __future__ import annotations

import atexit
import errno
import json
import logging
import os
import signal
from types import FrameType
from typing import Any

log = logging.getLogger(__name__)

LOCK_PATH = "/tmp/blinky-audio.lock"
_lock_held = False


def acquire_audio_lock(device_ids: list[str] | None = None) -> bool:
    """Acquire the audio lock. Returns True on success, False if held by another live process."""
    global _lock_held
    try:
        fd = os.open(LOCK_PATH, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        info = {
            "pid": os.getpid(),
            "ports": device_ids or [],
            "started": __import__("datetime").datetime.now().isoformat(),
        }
        os.write(fd, json.dumps(info).encode())
        os.close(fd)
        _lock_held = True
        return True
    except FileExistsError:
        # Lock file exists — check if holder is alive
        try:
            with open(LOCK_PATH) as f:
                info = json.load(f)
            try:
                os.kill(info["pid"], 0)  # Check existence
            except OSError as e:
                if e.errno == errno.ESRCH:
                    # No such process — stale lock, remove and retry once
                    try:
                        os.unlink(LOCK_PATH)
                    except OSError:
                        pass
                    return acquire_audio_lock(device_ids)
                # EPERM = process exists but owned by another user — lock is valid
            # Process alive — lock legitimately held
            log.warning(
                "Audio lock held by PID %d on %s (started %s)",
                info["pid"],
                info.get("ports", []),
                info.get("started", "?"),
            )
            return False
        except Exception:
            log.warning("Audio lock exists at %s but could not read it", LOCK_PATH)
            return False


def release_audio_lock() -> None:
    """Release the audio lock. Safe to call even if not held."""
    global _lock_held
    if not _lock_held:
        return
    try:
        os.unlink(LOCK_PATH)
    except OSError:
        pass
    _lock_held = False


def is_audio_locked() -> tuple[bool, dict[str, Any] | None]:
    """Check if the audio lock is held. Cleans up stale locks."""
    try:
        with open(LOCK_PATH) as f:
            info = json.load(f)
        try:
            os.kill(info["pid"], 0)
        except OSError as e:
            if e.errno == errno.ESRCH:
                # Stale lock — dead process
                try:
                    os.unlink(LOCK_PATH)
                except OSError:
                    pass
                return False, None
            # EPERM = process exists, lock is valid
        return True, info
    except (FileNotFoundError, json.JSONDecodeError, KeyError):
        return False, None


def _cleanup() -> None:
    release_audio_lock()


atexit.register(_cleanup)

# Signal handlers — release lock on SIGINT/SIGTERM
_orig_sigint = signal.getsignal(signal.SIGINT)
_orig_sigterm = signal.getsignal(signal.SIGTERM)


def _sigint_handler(signum: int, frame: FrameType | None) -> None:
    _cleanup()
    if callable(_orig_sigint):
        _orig_sigint(signum, frame)
    else:
        raise KeyboardInterrupt


def _sigterm_handler(signum: int, frame: FrameType | None) -> None:
    _cleanup()
    raise SystemExit(128 + 15)


signal.signal(signal.SIGINT, _sigint_handler)
signal.signal(signal.SIGTERM, _sigterm_handler)
