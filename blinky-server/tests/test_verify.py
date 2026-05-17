"""Phase-5 tests: progressive verify sub-state machine.

Pure-logic coverage of the ``run_verify`` state machine. Signals are
mocked via a controllable ``MockVerifySignals`` — the test sets which
signals report True and watches the state machine advance.

No hardware. No FleetManager integration yet (Phase 7 wires it).
"""

from __future__ import annotations

import asyncio
import contextlib
from pathlib import Path
from typing import Any

import pytest

from blinky_server.firmware.flash_job import (
    FlashJob,
    FlashJobState,
    FlashTransport,
    VerifySubState,
)
from blinky_server.firmware.verify import run_verify


class MockVerifySignals:
    """Tests drive the state machine by mutating these attributes."""

    def __init__(self) -> None:
        self.re_enumerated: bool = False
        self.serial_connected: bool = False
        self.handshake_info: dict[str, Any] | None = None
        # Spying — useful for verifying we don't call signals unnecessarily.
        self.calls: list[str] = []

    async def has_re_enumerated_since(self, device_id: str, since: float) -> bool:
        self.calls.append("re_enum")
        return self.re_enumerated

    async def is_serial_connected(self, device_id: str) -> bool:
        self.calls.append("is_serial")
        return self.serial_connected

    async def get_handshake_info(self, device_id: str) -> dict[str, Any] | None:
        self.calls.append("handshake")
        return self.handshake_info


# --- helpers ----------------------------------------------------------------


def _job_in_verifying() -> FlashJob:
    """Construct a job advanced to VERIFYING with write_completed_at set."""
    job = FlashJob(device_id="d-1", firmware_path=Path("/f"))
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    job.set_transport(FlashTransport.UF2)
    job.transition(FlashJobState.WRITING)
    job.transition(FlashJobState.VERIFYING)
    assert job.write_completed_at is not None
    return job


# --- pre-condition guards --------------------------------------------------


@pytest.mark.asyncio
async def test_run_verify_asserts_state_is_verifying() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    signals = MockVerifySignals()
    with pytest.raises(AssertionError):
        await run_verify(job, signals)


# --- happy paths -----------------------------------------------------------


@pytest.mark.asyncio
async def test_fast_boot_to_verified_no_version_check() -> None:
    """All signals already true on first poll → reaches VERIFIED in
    one cycle. No expected_version → no version-match step."""
    job = _job_in_verifying()
    signals = MockVerifySignals()
    signals.re_enumerated = True
    signals.serial_connected = True
    signals.handshake_info = {"version": "b166"}

    await asyncio.wait_for(run_verify(job, signals, poll_interval_s=0.01), timeout=1.0)

    assert job.verify_sub_state is VerifySubState.VERIFIED


@pytest.mark.asyncio
async def test_version_match_required_when_expected_set() -> None:
    """expected_version set → must reach AWAITING_VERSION_MATCH and
    confirm the reported version before VERIFIED."""
    job = _job_in_verifying()
    job.expected_version = "b166-fd3b4729-dirty"
    signals = MockVerifySignals()
    signals.re_enumerated = True
    signals.serial_connected = True
    signals.handshake_info = {"version": "b166-fd3b4729-dirty"}

    await asyncio.wait_for(run_verify(job, signals, poll_interval_s=0.01), timeout=1.0)

    assert job.verify_sub_state is VerifySubState.VERIFIED


@pytest.mark.asyncio
async def test_version_mismatch_stays_in_version_match_sub_state() -> None:
    """Wrong version reported → state machine stays in
    AWAITING_VERSION_MATCH, does NOT transition to VERIFIED or FAILED."""
    job = _job_in_verifying()
    job.expected_version = "b166-fd3b4729-dirty"
    signals = MockVerifySignals()
    signals.re_enumerated = True
    signals.serial_connected = True
    signals.handshake_info = {"version": "b165-old-dirty"}  # wrong!

    task = asyncio.create_task(run_verify(job, signals, poll_interval_s=0.01))
    # Let it run a few cycles, then cancel and inspect.
    await asyncio.sleep(0.1)
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task

    assert job.verify_sub_state is VerifySubState.AWAITING_VERSION_MATCH
    assert job.state is FlashJobState.VERIFYING  # no fail, no complete


# --- step-by-step progression ----------------------------------------------


@pytest.mark.asyncio
async def test_progresses_through_sub_states_as_signals_unlock() -> None:
    """Walk the signals on one at a time and confirm each unlocks the
    next sub-state. This is the spec for "no skipped steps when signals
    arrive sequentially."""
    job = _job_in_verifying()
    signals = MockVerifySignals()
    sub_states_seen: list[VerifySubState] = []

    # Patch set_verify_sub_state to record every transition.
    original = job.set_verify_sub_state

    def recording(sub: VerifySubState) -> None:
        sub_states_seen.append(sub)
        original(sub)

    job.set_verify_sub_state = recording  # type: ignore[method-assign]

    task = asyncio.create_task(run_verify(job, signals, poll_interval_s=0.01))
    # Cycle 1: nothing → stays AWAITING_REBOOT
    await asyncio.sleep(0.04)
    assert job.verify_sub_state is VerifySubState.AWAITING_REBOOT

    # Cycle 2: enable re-enum → advances to AWAITING_APP_BOOT
    signals.re_enumerated = True
    await asyncio.sleep(0.04)
    assert job.verify_sub_state is VerifySubState.AWAITING_APP_BOOT

    # Cycle 3: enable serial → advances to AWAITING_HANDSHAKE
    signals.serial_connected = True
    await asyncio.sleep(0.04)
    assert job.verify_sub_state is VerifySubState.AWAITING_HANDSHAKE

    # Cycle 4: provide handshake → no expected_version → VERIFIED
    signals.handshake_info = {"version": "b166"}
    await asyncio.wait_for(task, timeout=1.0)

    assert job.verify_sub_state is VerifySubState.VERIFIED
    assert VerifySubState.AWAITING_REBOOT in sub_states_seen
    assert VerifySubState.AWAITING_APP_BOOT in sub_states_seen
    assert VerifySubState.AWAITING_HANDSHAKE in sub_states_seen
    assert VerifySubState.VERIFIED in sub_states_seen


# --- never-fails-on-its-own ------------------------------------------------


@pytest.mark.asyncio
async def test_stuck_signals_never_fail_the_job() -> None:
    """Core design property: verify never times itself out. A device
    that's silent forever stays in AWAITING_REBOOT forever — the
    orchestrator decides when to give up, not us."""
    job = _job_in_verifying()
    signals = MockVerifySignals()  # all signals false

    task = asyncio.create_task(run_verify(job, signals, poll_interval_s=0.01))
    # Run for a longer-than-any-test-needs window. Job must NOT terminate.
    with pytest.raises(asyncio.TimeoutError):
        await asyncio.wait_for(asyncio.shield(task), timeout=0.15)
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task

    assert job.state is FlashJobState.VERIFYING
    assert job.verify_sub_state is VerifySubState.AWAITING_REBOOT
    # No anomaly added by run_verify (Phase 6 owns anomaly detection).
    assert job.anomalies == []


# --- handshake-without-version-key path ------------------------------------


@pytest.mark.asyncio
async def test_handshake_response_without_version_field_advances_anyway() -> None:
    """If expected_version is set but the device's handshake response
    has no recognizable version field, the version-match step stays
    AWAITING_VERSION_MATCH (correct: we don't have evidence of the
    right firmware yet). Without expected_version, an empty-ish info
    dict is sufficient."""
    # Without expected_version: any non-None handshake completes.
    job1 = _job_in_verifying()
    signals1 = MockVerifySignals()
    signals1.re_enumerated = True
    signals1.serial_connected = True
    signals1.handshake_info = {"connected": True}  # no version key
    await asyncio.wait_for(run_verify(job1, signals1, poll_interval_s=0.01), timeout=1.0)
    assert job1.verify_sub_state is VerifySubState.VERIFIED

    # With expected_version: missing version field keeps us awaiting.
    job2 = _job_in_verifying()
    job2.expected_version = "b166"
    signals2 = MockVerifySignals()
    signals2.re_enumerated = True
    signals2.serial_connected = True
    signals2.handshake_info = {"connected": True}  # no version key
    task = asyncio.create_task(run_verify(job2, signals2, poll_interval_s=0.01))
    await asyncio.sleep(0.1)
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task
    assert job2.verify_sub_state is VerifySubState.AWAITING_VERSION_MATCH


# --- version-field key variants --------------------------------------------


@pytest.mark.asyncio
@pytest.mark.parametrize("key", ["version", "firmware_version", "fw_version", "build"])
async def test_version_extracted_from_any_known_key(key: str) -> None:
    """Firmware vintages have used different keys; ``_extract_version``
    accepts all of them so a build that uses ``firmware_version``
    instead of ``version`` still verifies."""
    job = _job_in_verifying()
    job.expected_version = "b166"
    signals = MockVerifySignals()
    signals.re_enumerated = True
    signals.serial_connected = True
    signals.handshake_info = {key: "b166"}
    await asyncio.wait_for(run_verify(job, signals, poll_interval_s=0.01), timeout=1.0)
    assert job.verify_sub_state is VerifySubState.VERIFIED


# --- cancellation -----------------------------------------------------------


@pytest.mark.asyncio
async def test_cancellation_leaves_job_in_verifying() -> None:
    """Orchestrator cancels the task → CancelledError propagates;
    job stays in VERIFYING (orchestrator decides ABANDONED/FAILED)."""
    job = _job_in_verifying()
    signals = MockVerifySignals()

    task = asyncio.create_task(run_verify(job, signals, poll_interval_s=0.01))
    await asyncio.sleep(0.02)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert job.state is FlashJobState.VERIFYING
