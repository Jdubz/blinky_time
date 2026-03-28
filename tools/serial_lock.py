"""Serial port lock — mutual exclusion for serial port access.

Multiple tools (blinky-server, blinky-serial-mcp, uf2_upload.py) compete
for serial ports. This module implements per-port file-based locking using
atomic O_CREAT|O_EXCL creation, matching the audio-lock pattern.

Lock directory: /tmp/blinky-serial/
Lock file:      /tmp/blinky-serial/<port_basename>.lock
Contents:       JSON {pid, tool, port, purpose, acquired, hold_until}

A lock is stale when the owning PID is dead (signal 0 → ESRCH).
hold_until is informational — locks are only auto-cleaned on dead PID.
"""

from __future__ import annotations

import errno
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

LOCK_DIR = Path("/tmp/blinky-serial")


def _normalize_port(port: str) -> str:
    """Resolve symlinks and extract basename (e.g., 'ttyACM0')."""
    p = Path(port)
    try:
        p = p.resolve()
    except OSError:
        pass
    return p.name


def _lock_path(port: str) -> Path:
    return LOCK_DIR / f"{_normalize_port(port)}.lock"


def _pid_alive(pid: int) -> bool:
    """Check if a process is alive via signal 0."""
    try:
        os.kill(pid, 0)
        return True
    except OSError as e:
        if e.errno == errno.ESRCH:
            return False
        # EPERM = process exists but we can't signal it
        return True


def acquire(port: str, tool: str, purpose: str,
            hold_seconds: int = 60) -> bool:
    """Acquire a lock on a serial port.

    Uses atomic file creation (O_CREAT|O_EXCL) so two processes racing
    will never both succeed. If a stale lock is detected (dead PID), it
    is removed and acquisition retried once.

    Returns True if acquired, False if held by a live process.
    """
    LOCK_DIR.mkdir(exist_ok=True)
    lock_file = _lock_path(port)

    now = datetime.now(timezone.utc)
    hold_until = datetime.fromtimestamp(
        time.time() + hold_seconds, tz=timezone.utc
    )
    info = {
        "pid": os.getpid(),
        "tool": tool,
        "port": str(Path(port).resolve()) if Path(port).exists() else port,
        "purpose": purpose,
        "acquired": now.isoformat(),
        "hold_until": hold_until.isoformat(),
    }

    try:
        fd = os.open(str(lock_file), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        os.write(fd, json.dumps(info).encode())
        os.close(fd)
        return True
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise

    # Lock file exists — check if holder is alive
    try:
        raw = lock_file.read_text()
        holder = json.loads(raw)
        holder_pid = holder.get("pid", 0)

        if not _pid_alive(holder_pid):
            # Stale lock — remove and retry once
            try:
                lock_file.unlink()
            except OSError:
                pass  # Race with another cleaner
            return acquire(port, tool, purpose, hold_seconds)

        # Process is alive — lock is legitimately held
        return False
    except (OSError, json.JSONDecodeError):
        # Can't read lock file — treat as held (conservative)
        return False


def release(port: str) -> None:
    """Release a lock on a serial port. Safe to call if not held."""
    lock_file = _lock_path(port)
    try:
        # Only remove if we own it
        raw = lock_file.read_text()
        holder = json.loads(raw)
        if holder.get("pid") == os.getpid():
            lock_file.unlink()
    except (OSError, json.JSONDecodeError):
        pass


def force_release(port: str) -> bool:
    """Remove lock regardless of holder. For manual recovery."""
    lock_file = _lock_path(port)
    try:
        lock_file.unlink()
        return True
    except OSError:
        return False


def is_locked(port: str) -> tuple[bool, dict | None]:
    """Check if a port is locked. Auto-cleans stale locks (dead PID).

    Returns (locked: bool, holder_info: dict | None).
    """
    lock_file = _lock_path(port)
    try:
        raw = lock_file.read_text()
        holder = json.loads(raw)
        holder_pid = holder.get("pid", 0)

        if not _pid_alive(holder_pid):
            # Stale — clean up
            try:
                lock_file.unlink()
            except OSError:
                pass
            return (False, None)

        return (True, holder)
    except (OSError, json.JSONDecodeError):
        return (False, None)
