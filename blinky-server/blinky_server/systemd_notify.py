"""Minimal sd_notify implementation — no new dependency.

systemd ``Type=notify`` services tell systemd they're ready and alive via
``$NOTIFY_SOCKET``. The protocol is a few well-known KEY=VALUE messages
sent as datagrams to a unix socket. The `sdnotify` PyPI package wraps
exactly this; we inline it to keep dependencies minimal.

Outside systemd (no $NOTIFY_SOCKET set), every call is a no-op — safe to
use unconditionally during dev/test.

See ``man sd_notify(3)`` for the full protocol.
"""

from __future__ import annotations

import logging
import os
import socket

log = logging.getLogger(__name__)


def _send(message: str) -> None:
    addr = os.environ.get("NOTIFY_SOCKET")
    if not addr:
        return  # Not running under systemd Type=notify; do nothing.
    # Abstract sockets are prefixed with @ in $NOTIFY_SOCKET; convert to
    # the kernel's null-byte form.
    if addr.startswith("@"):
        addr = "\0" + addr[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            sock.sendto(message.encode("utf-8"), addr)
    except OSError as exc:
        # Notify-socket failures are non-fatal — service must keep running.
        # Log loudly though; a silent failure here means systemd watchdog
        # is silently inactive, which defeats the whole purpose.
        log.warning("sd_notify(%r) failed: %s", message, exc)


def ready() -> None:
    """Tell systemd the service is fully started.

    With Type=notify, systemd holds dependent units until this fires, so
    call it once after the API is actually listening + the fleet loop has
    started, not just after Python imports finish.
    """
    _send("READY=1\n")


def watchdog() -> None:
    """Heartbeat to systemd's WatchdogSec=. Call from the background loop
    after each successful cycle. Missing pings for WatchdogSec/2 makes
    systemd kill (then Restart=) the process."""
    _send("WATCHDOG=1\n")


def status(message: str) -> None:
    """Short status string surfaced by ``systemctl status``."""
    # Strip newlines — protocol is line-delimited.
    safe = message.replace("\n", " ").replace("\r", " ")
    _send(f"STATUS={safe}\n")


def stopping() -> None:
    """Tell systemd we're shutting down cleanly (suppresses 'lost watchdog
    contact' warnings during graceful stop)."""
    _send("STOPPING=1\n")


def watchdog_sec() -> float | None:
    """Return WatchdogSec= from systemd (in seconds), or None if unset.

    systemd sets ``$WATCHDOG_USEC`` in the service environment when
    ``WatchdogSec=`` is configured. The loop uses this to decide how
    often to ping; if it doesn't fire often enough, systemd will kill us.
    """
    raw = os.environ.get("WATCHDOG_USEC")
    if not raw:
        return None
    try:
        return int(raw) / 1_000_000.0
    except ValueError:
        log.warning("Bad WATCHDOG_USEC=%r", raw)
        return None
