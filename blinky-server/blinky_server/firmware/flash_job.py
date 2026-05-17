"""Single, atomic flash attempt — state machine + progress tracking.

A ``FlashJob`` owns one device for the duration of one flash attempt.
``FleetManager`` enforces "at most one active ``FlashJob`` per device" via
per-device locks, and dedups would-be auto-recoveries against
recently-finished jobs. Both invariants close the cascade bug from
2026-05-17 (see [[project-deploy-flash-cascade-bug]] in auto-memory),
where UF2 success + verification-timeout-mislabelled-as-failure caused
``_background_loop`` to independently start a BLE-DFU and re-write the
firmware the UF2 path had just landed.

Transport selection happens once at job start and is final — the job
does **not** silently fall back to a different transport mid-flight.
Per ``feedback_flash_safety_policy``, USB is preferred and BLE-DFU is
the explicit fallback only when no USB-CDC reaches the device.

The verify phase is a sub-state machine, not a wall-clock timer. Each
``VerifySubState`` advances on a real signal (USB re-enumeration, app
handshake, version match). The overall ``VERIFYING`` state never
auto-flips to ``FAILED`` on a deadline — operators see progress through
the sub-states and choose whether to abort. Anomaly detectors raise
observable warnings (boot loop, quarantine, stale firmware, etc.) but
don't terminate the job on their own.
"""

from __future__ import annotations

import asyncio
import enum
import logging
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)


class FlashJobState(enum.Enum):
    """Top-level lifecycle states.

    Transitions are strictly enforced (see ``_ALLOWED_TRANSITIONS``). Terminal
    states (``COMPLETED`` / ``FAILED`` / ``ABANDONED``) reject all transitions
    — a terminal job is read-only; a new attempt requires a new ``FlashJob``.
    """

    PENDING = "pending"
    SELECTING_TRANSPORT = "selecting_transport"
    WRITING = "writing"
    VERIFYING = "verifying"
    COMPLETED = "completed"
    FAILED = "failed"
    ABANDONED = "abandoned"


class FlashTransport(enum.Enum):
    """Which physical path the write goes over. Chosen once, final."""

    UF2 = "uf2"
    BLE_DFU = "ble_dfu"


class VerifySubState(enum.Enum):
    """Progressive verify state. Each step advances on a real device signal.

    Order is conceptual but **not strictly enforced** — a fast boot can skip
    intermediate sub-states (e.g., the very first handshake response could
    carry the correct version, taking us straight to ``VERIFIED``).
    """

    AWAITING_REBOOT = "awaiting_reboot"
    AWAITING_APP_BOOT = "awaiting_app_boot"
    AWAITING_HANDSHAKE = "awaiting_handshake"
    AWAITING_VERSION_MATCH = "awaiting_version_match"
    VERIFIED = "verified"


_ALLOWED_TRANSITIONS: dict[FlashJobState, frozenset[FlashJobState]] = {
    FlashJobState.PENDING: frozenset(
        {FlashJobState.SELECTING_TRANSPORT, FlashJobState.FAILED, FlashJobState.ABANDONED}
    ),
    FlashJobState.SELECTING_TRANSPORT: frozenset(
        {FlashJobState.WRITING, FlashJobState.FAILED, FlashJobState.ABANDONED}
    ),
    FlashJobState.WRITING: frozenset(
        {FlashJobState.VERIFYING, FlashJobState.FAILED, FlashJobState.ABANDONED}
    ),
    FlashJobState.VERIFYING: frozenset(
        {FlashJobState.COMPLETED, FlashJobState.FAILED, FlashJobState.ABANDONED}
    ),
    FlashJobState.COMPLETED: frozenset(),
    FlashJobState.FAILED: frozenset(),
    FlashJobState.ABANDONED: frozenset(),
}

TERMINAL_STATES: frozenset[FlashJobState] = frozenset(
    {FlashJobState.COMPLETED, FlashJobState.FAILED, FlashJobState.ABANDONED}
)


class InvalidTransition(Exception):
    """Raised when ``transition()`` is asked for a disallowed state move.

    Always indicates a code bug in the orchestrator — never a user-input or
    runtime-data problem. Don't catch and silently recover; surface it.
    """


class NoReachableTransport(Exception):
    """No physical path reaches this device right now — cannot flash.

    Raised by ``select_transport`` when both USB-CDC (app-mode) and
    BLE-DFU (bootloader AdaDFU advertisement) are absent. The right
    response is to defer the flash and surface the unreachability to the
    operator, not to retry blindly. Common reasons:
      - device is in bootloader CDC-DFU mode with neither MSC nor OTA
        (the "stuck after 1200-baud touch" state we hit on 2026-05-17)
      - device is powered off
      - device is too far for the cart-side hci0 radio AND no USB cable
    """


@dataclass(frozen=True)
class TransportProbe:
    """Snapshot of which interfaces can reach the device *right now*.

    The orchestrator builds one of these at job start (during
    ``SELECTING_TRANSPORT``) from the current ``Device`` / discovery
    state, then hands it to ``select_transport``. Snapshotting up-front
    (rather than re-probing inside the selector) keeps the decision
    auditable: the exact inputs that drove the choice are logged with
    the job.
    """

    # True iff the device has an *app-mode* USB-CDC connection — i.e. the
    # firmware's ``SerialConsole`` is responsive, so the orchestrator can
    # issue ``bootloader`` to drop the device into UF2 MSC mode for UF2
    # writes. A bootloader-CDC-DFU enumeration (the post-1200-baud-touch
    # state) does NOT count: the bootloader's CDC doesn't speak our
    # SerialConsole protocol.
    has_usb_app: bool

    # True iff the device's bootloader BLE address is currently
    # advertising the AdaDFU service. This is the prerequisite for the
    # BLE-DFU path.
    has_ble_dfu_advert: bool


def select_transport(probe: TransportProbe) -> FlashTransport:
    """Pick the flash transport for a job. Pinned for the job's lifetime.

    Decision rule (per ``feedback_flash_safety_policy``: USB > BLE):
      1. USB-CDC app present → UF2 (fast, no radio contention with the
         broadcaster, no BlueZ slot management).
      2. Otherwise, AdaDFU advert present → BLE-DFU.
      3. Otherwise → raise ``NoReachableTransport`` — the orchestrator
         must not silently retry; the operator decides.

    The "both present" case picks UF2 by rule 1 — there is intentionally
    no policy lever to override that here. The cascade bug we're closing
    out (see [[project-deploy-flash-cascade-bug]]) was caused by the two
    transports racing; collapsing the choice to USB-when-available is
    exactly the point.
    """
    if probe.has_usb_app:
        return FlashTransport.UF2
    if probe.has_ble_dfu_advert:
        return FlashTransport.BLE_DFU
    raise NoReachableTransport(
        "Device is not reachable: no USB-CDC app handshake and no AdaDFU "
        "advertisement. Cannot flash. Defer until one transport reappears."
    )


@dataclass
class FlashJob:
    """One device, one flash attempt.

    Construct in ``PENDING``. Drive through the state machine via
    ``transition()``. The instance is the source of truth for "what's
    happening with this device's flash right now" — including for API
    polling, log lines, and the auto-recovery dedup check.

    Async-aware: ``wait_for_change()`` lets pollers wake on any mutation
    without busy-looping. The internal event is recreated as needed so a
    ``FlashJob`` constructed outside an event loop (e.g. in a sync test)
    still works once a loop appears.
    """

    device_id: str
    firmware_path: Path
    # Pinned at construction so the verify-version-match step has something
    # to compare device's `json info` output against. If ``None``, the
    # version-match sub-state is skipped (verify completes on handshake).
    expected_version: str | None = None

    job_id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])

    state: FlashJobState = FlashJobState.PENDING
    transport: FlashTransport | None = None
    verify_sub_state: VerifySubState | None = None
    anomalies: list[str] = field(default_factory=list)

    # Use a lambda for default_factory (not bare ``time.time``) so unit-test
    # monkeypatches of ``time.time`` reach this field's initializer. Bare
    # ``time.time`` would capture the original callable at class-definition
    # time, leaving ``created_at`` un-mocked even when other ``time.time()``
    # calls in this module are mocked.
    created_at: float = field(default_factory=lambda: time.time())
    started_at: float | None = None
    write_completed_at: float | None = None
    verified_at: float | None = None
    finished_at: float | None = None  # set when entering a terminal state

    bytes_written: int = 0
    bytes_total: int = 0
    error: str | None = None

    # Bumped on every mutation so pollers can detect "anything changed
    # since the last snapshot" without diffing the whole struct. Pairs
    # with ``_change_event`` for async waits.
    _seq: int = 0
    # Lazily created on first use so a ``FlashJob`` constructed in sync
    # code (tests, scripts) doesn't bind to a loop that isn't running.
    _change_event: asyncio.Event | None = field(default=None, repr=False, compare=False)

    # ------------------------------------------------------------------ #
    # State transitions
    # ------------------------------------------------------------------ #

    def transition(self, new_state: FlashJobState) -> None:
        """Move to ``new_state`` or raise ``InvalidTransition``.

        Side effects: stamps the appropriate timestamp (``started_at`` on
        first move to WRITING, ``write_completed_at`` on VERIFYING,
        ``verified_at`` on COMPLETED, ``finished_at`` on any terminal),
        bumps ``_seq``, signals waiters.
        """
        if new_state not in _ALLOWED_TRANSITIONS[self.state]:
            raise InvalidTransition(
                f"Cannot transition {self.state.value} → {new_state.value} "
                f"(allowed from {self.state.value}: "
                f"{sorted(s.value for s in _ALLOWED_TRANSITIONS[self.state])})"
            )
        now = time.time()
        # Timestamp stamping is tied to the *destination* state, not the
        # source — this way a job that restarts the writer (which today
        # can't happen but might in v2) doesn't reset ``started_at``.
        if new_state == FlashJobState.WRITING and self.started_at is None:
            self.started_at = now
        elif new_state == FlashJobState.VERIFYING:
            self.write_completed_at = now
        elif new_state == FlashJobState.COMPLETED:
            self.verified_at = now
            self.finished_at = now
        elif new_state in (FlashJobState.FAILED, FlashJobState.ABANDONED):
            self.finished_at = now
        self.state = new_state
        self._notify_change()

    def set_transport(self, transport: FlashTransport) -> None:
        """Pin the transport. Callable exactly once, in ``SELECTING_TRANSPORT``.

        Per ``feedback_flash_safety_policy``, transport is final once set.
        Re-setting raises ``InvalidTransition`` so a bug that tries to fall
        back from UF2 → BLE-DFU mid-job is caught loud, not silent.
        """
        if self.transport is not None:
            raise InvalidTransition(
                f"transport already set to {self.transport.value}; refusing to change"
            )
        if self.state != FlashJobState.SELECTING_TRANSPORT:
            raise InvalidTransition(
                f"set_transport only valid in SELECTING_TRANSPORT (current: {self.state.value})"
            )
        self.transport = transport
        self._notify_change()

    def set_verify_sub_state(self, sub: VerifySubState) -> None:
        """Advance the verify sub-state. Only valid in ``VERIFYING``."""
        if self.state != FlashJobState.VERIFYING:
            raise InvalidTransition(
                f"verify_sub_state only valid during VERIFYING (current: {self.state.value})"
            )
        self.verify_sub_state = sub
        self._notify_change()

    def add_anomaly(self, name: str) -> None:
        """Record an anomaly. Idempotent — repeats don't grow the list.

        Anomalies are observable warnings (boot loop, quarantine, stale
        firmware, etc.). They never auto-fail the job; the orchestrator
        decides what an anomaly means in context. Operators see the
        distinct types, not counts of repeats, so dedup is safe.
        """
        if name not in self.anomalies:
            self.anomalies.append(name)
            self._notify_change()

    def record_progress(self, bytes_written: int, bytes_total: int) -> None:
        """Update write progress. Only meaningful during WRITING."""
        if self.state != FlashJobState.WRITING:
            raise InvalidTransition(
                f"record_progress only valid during WRITING (current: {self.state.value})"
            )
        if bytes_written < 0 or bytes_total < 0 or bytes_written > bytes_total:
            raise ValueError(
                f"invalid progress: bytes_written={bytes_written}, bytes_total={bytes_total}"
            )
        self.bytes_written = bytes_written
        self.bytes_total = bytes_total
        self._notify_change()

    def set_error(self, msg: str) -> None:
        """Attach an error message. Does NOT change state — caller must
        also ``transition(FlashJobState.FAILED)`` if appropriate.

        Separated from ``transition()`` so a non-fatal warning (e.g., a
        verify sub-state that timed out without failing) can attach a
        description without forcing a terminal state.

        Errors are the primary post-mortem signal, so log loudly on an
        overwrite — silently losing the first error string is the kind
        of thing that turns a 5-minute debug into a 5-hour one.
        """
        if self.error is not None and self.error != msg:
            log.warning(
                "flash job %s error overwritten: was %r, now %r",
                self.job_id,
                self.error,
                msg,
            )
        self.error = msg
        self._notify_change()

    # ------------------------------------------------------------------ #
    # Inspection
    # ------------------------------------------------------------------ #

    @property
    def is_terminal(self) -> bool:
        return self.state in TERMINAL_STATES

    @property
    def is_active(self) -> bool:
        return not self.is_terminal

    @property
    def duration_s(self) -> float:
        # `finished_at` is set by the state-machine transition into any
        # terminal state, so it's non-None whenever is_terminal is true.
        # Mypy can't see the invariant — the explicit None check keeps it
        # happy AND defends against a future bug that breaks the link.
        end = (
            self.finished_at if (self.is_terminal and self.finished_at is not None) else time.time()
        )
        return end - self.created_at

    @property
    def seq(self) -> int:
        """Monotonic mutation counter. Pollers compare to detect any change."""
        return self._seq

    def to_dict(self) -> dict[str, Any]:
        """JSON-serializable snapshot. Used by ``GET /api/jobs/{id}``."""
        return {
            "job_id": self.job_id,
            "device_id": self.device_id,
            "firmware_path": str(self.firmware_path),
            "expected_version": self.expected_version,
            "state": self.state.value,
            "transport": self.transport.value if self.transport else None,
            "verify_sub_state": (self.verify_sub_state.value if self.verify_sub_state else None),
            "anomalies": list(self.anomalies),
            "bytes_written": self.bytes_written,
            "bytes_total": self.bytes_total,
            "error": self.error,
            "created_at": self.created_at,
            "started_at": self.started_at,
            "write_completed_at": self.write_completed_at,
            "verified_at": self.verified_at,
            "finished_at": self.finished_at,
            "duration_s": round(self.duration_s, 2),
            "seq": self._seq,
            "is_terminal": self.is_terminal,
        }

    # ------------------------------------------------------------------ #
    # Async coordination
    # ------------------------------------------------------------------ #

    async def wait_until_terminal(self, timeout: float | None = None) -> bool:
        """Block until the job reaches a terminal state (or ``timeout``).

        ``timeout`` is an *absolute* elapsed cap from this call's entry,
        not a per-change reset. A job that produces several non-terminal
        transitions before finishing (PENDING → SELECTING → WRITING →
        VERIFYING …) won't extend the deadline on each one — the caller
        gets back control no later than ``timeout`` seconds after the
        call started. ``timeout=None`` waits indefinitely.

        Returns True if terminal, False if the timeout fired first. Common
        use: auto-recovery and deploy.sh both want "wait for this flash to
        be done, no matter what done means" without writing their own
        polling loop.
        """
        if timeout is None:
            while not self.is_terminal:
                snap = self._seq
                await self.wait_for_change(since_seq=snap, timeout=None)
            return True

        deadline = time.monotonic() + timeout
        while not self.is_terminal:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return self.is_terminal
            snap = self._seq
            new_seq = await self.wait_for_change(since_seq=snap, timeout=remaining)
            if new_seq == snap:
                # Per-iteration wait_for_change returned without a change
                # — either the deadline elapsed or the spurious-wake path
                # in wait_for_change fired. Re-check is_terminal once
                # more (a transition could have just landed) and exit.
                return self.is_terminal
        return True

    async def wait_for_change(self, since_seq: int, timeout: float | None = None) -> int:
        """Wait until ``seq`` advances past ``since_seq``. Returns current ``seq``.

        Designed for long-poll endpoints: caller snapshots ``seq``, sends the
        snapshot to the client, on next poll passes the same ``seq`` and
        blocks here until something changes (or timeout). Avoids both
        busy-polling and missed-edge bugs that come from raw Event.wait().

        Returns the current ``seq`` whether it changed or the timeout fired
        — caller can compare to ``since_seq`` to distinguish.
        """
        while self._seq <= since_seq:
            event = self._ensure_event()
            try:
                await asyncio.wait_for(event.wait(), timeout=timeout)
            except TimeoutError:
                return self._seq
        return self._seq

    def _ensure_event(self) -> asyncio.Event:
        if self._change_event is None:
            self._change_event = asyncio.Event()
        return self._change_event

    def _notify_change(self) -> None:
        self._seq += 1
        if self._change_event is not None:
            # Set + immediate clear: wakes everyone currently waiting, but
            # the next call to ``wait_for_change`` starts cleanly. The seq
            # comparison is the source of truth, so we don't depend on the
            # event being set "stickily" between mutations.
            self._change_event.set()
            self._change_event.clear()
