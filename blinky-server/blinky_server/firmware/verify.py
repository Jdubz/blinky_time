"""Phase-5: progressive verify sub-state machine.

After the writer says "Wrote N/N bytes," the orchestrator transitions
the job to VERIFYING and hands it to ``run_verify``. This module's job
is to *observe* the device's path back to a responsive app — never to
declare failure on a wall-clock timer.

The model:
    AWAITING_REBOOT — looking for a USB re-enumeration event since
        ``job.write_completed_at``, OR an AdaDFU advert dropping off.
    AWAITING_APP_BOOT — re-enum seen; waiting for serial CDC to come back.
    AWAITING_HANDSHAKE — CDC up; waiting for ``json info`` to respond.
    AWAITING_VERSION_MATCH — handshake got; comparing reported version
        to ``job.expected_version`` (skipped when expected_version is None).
    VERIFIED — all four signals confirmed; ``run_verify`` returns.

Signals are abstracted behind a ``VerifySignals`` protocol so the same
state machine can run against real ``Device`` lookups (Phase 7) and
against mocks (these tests). The orchestrator wires the production
signals at integration time.

There is NO wall-clock failure in this module. ``run_verify`` runs
until the job is VERIFIED (returns) or until the orchestrator cancels
the task. The job's sub-state and ``error`` field describe progress;
``GET /api/flash-jobs/{id}`` shows everything an operator needs.

Anomaly detectors (Phase 6) attach to this loop as a separate pass —
they observe the same signals but emit warnings without changing state.
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import Any, Protocol

from .flash_job import FlashJob, FlashJobState, VerifySubState
from .utils import extract_version

log = logging.getLogger(__name__)


class VerifySignals(Protocol):
    """The signals ``run_verify`` polls. Implementations:
      - Phase 7 production: ``FleetManagerVerifySignals`` (Device-backed).
      - Tests: ``MockVerifySignals`` (controlled by the test).

    All methods are async because real signals involve I/O (USB sysfs
    reads, serial reads). The state machine awaits each cycle.
    """

    async def has_re_enumerated_since(self, device_id: str, since: float) -> bool:
        """Has the device's USB enumeration changed since ``since``?

        True iff a new USB device-number / new ``/dev/ttyACM*`` link has
        appeared for this serial number after ``since``. This is the
        canonical "the device rebooted" signal; we don't trust serial
        responsiveness alone because the bootloader's CDC may stay open
        across the bootloader → app transition.
        """

    async def is_serial_connected(self, device_id: str) -> bool:
        """Is the server's serial transport currently connected?"""

    async def get_handshake_info(self, device_id: str) -> dict[str, Any] | None:
        """Send ``json info`` and return the parsed response, or None.

        None on: no serial transport, write failure, no reply within the
        normal command timeout, JSON parse failure. The handler treats
        any None as "not yet responsive" and keeps polling.
        """


async def run_verify(
    job: FlashJob,
    signals: VerifySignals,
    *,
    poll_interval_s: float = 1.0,
    progress_log_every_s: float = 15.0,
) -> None:
    """Run the verify state machine until ``VerifySubState.VERIFIED``.

    Does NOT transition the job to ``COMPLETED`` — that's the
    orchestrator's call, so the orchestrator can do final cleanup
    (release lock, stamp dedup table) atomically.

    Caller cancels the underlying task if they want to stop polling
    early (e.g., operator abort). Cancellation raises
    ``asyncio.CancelledError`` out of this coroutine, which the
    orchestrator catches and translates into ``ABANDONED``.

    Pre-condition: ``job.state == VERIFYING`` and ``job.write_completed_at``
    is set (raises ``AssertionError`` otherwise — a code bug in the caller).
    """
    assert job.state is FlashJobState.VERIFYING, (
        f"run_verify expected job in VERIFYING state, got {job.state.value}"
    )
    assert job.write_completed_at is not None, (
        "run_verify: write_completed_at not set; cannot anchor re-enum signal"
    )

    # The reboot signal is anchored at the moment the write finished. A
    # re-enumeration that happened *before* that moment isn't evidence
    # the new firmware booted.
    reboot_since = job.write_completed_at

    # Start in AWAITING_REBOOT. The state machine advances on signal
    # observation; a very-fast boot (write → enum → handshake within one
    # poll cycle) skips intermediate sub-states naturally.
    job.set_verify_sub_state(VerifySubState.AWAITING_REBOOT)

    # Track when we entered the current sub-state, for progress logging.
    sub_state_entered_at = time.time()
    last_logged_at: float = 0.0
    current_sub = VerifySubState.AWAITING_REBOOT

    while True:
        # --- 1) AWAITING_REBOOT ----------------------------------------
        if current_sub is VerifySubState.AWAITING_REBOOT and await signals.has_re_enumerated_since(
            job.device_id, reboot_since
        ):
            current_sub = VerifySubState.AWAITING_APP_BOOT
            job.set_verify_sub_state(current_sub)
            sub_state_entered_at = time.time()
            last_logged_at = 0.0

        # --- 2) AWAITING_APP_BOOT --------------------------------------
        if current_sub is VerifySubState.AWAITING_APP_BOOT and await signals.is_serial_connected(
            job.device_id
        ):
            current_sub = VerifySubState.AWAITING_HANDSHAKE
            job.set_verify_sub_state(current_sub)
            sub_state_entered_at = time.time()
            last_logged_at = 0.0

        # --- 3) AWAITING_HANDSHAKE -------------------------------------
        if current_sub is VerifySubState.AWAITING_HANDSHAKE:
            info = await signals.get_handshake_info(job.device_id)
            if info is not None:
                # Got a response. Either advance to version-match (if
                # expected_version was set) or jump straight to VERIFIED.
                if job.expected_version is None:
                    current_sub = VerifySubState.VERIFIED
                    job.set_verify_sub_state(current_sub)
                    return
                current_sub = VerifySubState.AWAITING_VERSION_MATCH
                job.set_verify_sub_state(current_sub)
                sub_state_entered_at = time.time()
                last_logged_at = 0.0
                # Don't fall through — re-fetch on next iteration to make
                # the version-check happen in its own poll. Keeps timing
                # predictable + lets a test mock vary the version between
                # the handshake and the match poll.
                await asyncio.sleep(poll_interval_s)
                continue

        # --- 4) AWAITING_VERSION_MATCH ---------------------------------
        if current_sub is VerifySubState.AWAITING_VERSION_MATCH:
            info = await signals.get_handshake_info(job.device_id)
            if info is not None:
                reported = _extract_version(info)
                if reported == job.expected_version:
                    current_sub = VerifySubState.VERIFIED
                    job.set_verify_sub_state(current_sub)
                    return

        # --- Progress log ----------------------------------------------
        now = time.time()
        elapsed_in_sub = now - sub_state_entered_at
        if (
            elapsed_in_sub >= progress_log_every_s
            and (now - last_logged_at) >= progress_log_every_s
        ):
            log.info(
                "verify job=%s device=%s sub=%s elapsed_in_sub=%.0fs",
                job.job_id,
                job.device_id,
                current_sub.value,
                elapsed_in_sub,
            )
            last_logged_at = now

        await asyncio.sleep(poll_interval_s)


def _extract_version(info: dict[str, Any]) -> str | None:
    """Pull the firmware version string out of a ``json info`` response.

    Delegates to ``firmware.utils.extract_version`` — the canonical
    version-key list lives there and is shared with ``anomalies.py``.
    PR 142 review flagged this previously-duplicated copy as a sync hazard.
    """
    return extract_version(info)
