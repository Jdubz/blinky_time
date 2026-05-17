"""Phase-1 unit tests for ``FlashJob``: state machine, transitions, async waits.

No hardware, no wiring, no FleetManager. Pure-logic coverage. Adding these
first (Phase 1 of the flash-job rewrite) before touching anything that
actually flashes — see [[project-deploy-flash-cascade-bug]] in auto-memory.
"""

from __future__ import annotations

import asyncio
import time
from pathlib import Path

import pytest

from blinky_server.firmware.flash_job import (
    FlashJob,
    FlashJobState,
    FlashTransport,
    InvalidTransition,
    NoReachableTransport,
    TERMINAL_STATES,
    TransportProbe,
    VerifySubState,
    select_transport,
)


# --- construction / defaults -------------------------------------------------


def test_new_job_starts_in_pending() -> None:
    job = FlashJob(device_id="abc", firmware_path=Path("/x/y.hex"))
    assert job.state is FlashJobState.PENDING
    assert job.transport is None
    assert job.verify_sub_state is None
    assert job.anomalies == []
    assert job.bytes_written == 0
    assert job.error is None
    assert job.started_at is None
    assert job.finished_at is None
    assert job.is_active and not job.is_terminal
    assert job.seq == 0


def test_job_id_is_unique_and_short() -> None:
    a = FlashJob(device_id="x", firmware_path=Path("/f"))
    b = FlashJob(device_id="x", firmware_path=Path("/f"))
    assert a.job_id != b.job_id
    # 12 hex chars per the uuid4().hex[:12] convention — keeps log lines short.
    assert len(a.job_id) == 12
    assert all(c in "0123456789abcdef" for c in a.job_id)


# --- happy-path transition chain --------------------------------------------


def test_full_happy_path_transitions() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    job.set_transport(FlashTransport.UF2)
    job.transition(FlashJobState.WRITING)
    assert job.started_at is not None
    job.record_progress(50, 100)
    job.record_progress(100, 100)
    job.transition(FlashJobState.VERIFYING)
    assert job.write_completed_at is not None
    job.set_verify_sub_state(VerifySubState.AWAITING_REBOOT)
    job.set_verify_sub_state(VerifySubState.VERIFIED)
    job.transition(FlashJobState.COMPLETED)
    assert job.is_terminal
    assert job.verified_at is not None
    assert job.finished_at is not None
    assert job.duration_s >= 0


# --- invalid transitions ----------------------------------------------------


@pytest.mark.parametrize(
    "src,dst",
    [
        (FlashJobState.PENDING, FlashJobState.WRITING),
        (FlashJobState.PENDING, FlashJobState.VERIFYING),
        (FlashJobState.PENDING, FlashJobState.COMPLETED),
        (FlashJobState.SELECTING_TRANSPORT, FlashJobState.VERIFYING),
        (FlashJobState.SELECTING_TRANSPORT, FlashJobState.COMPLETED),
        (FlashJobState.WRITING, FlashJobState.COMPLETED),
        (FlashJobState.VERIFYING, FlashJobState.WRITING),
        (FlashJobState.VERIFYING, FlashJobState.SELECTING_TRANSPORT),
    ],
)
def test_invalid_transitions_raise(src: FlashJobState, dst: FlashJobState) -> None:
    job = _job_in_state(src)
    with pytest.raises(InvalidTransition):
        job.transition(dst)


@pytest.mark.parametrize("terminal", sorted(TERMINAL_STATES, key=lambda s: s.value))
@pytest.mark.parametrize(
    "to",
    [
        FlashJobState.PENDING,
        FlashJobState.SELECTING_TRANSPORT,
        FlashJobState.WRITING,
        FlashJobState.VERIFYING,
        FlashJobState.COMPLETED,
        FlashJobState.FAILED,
        FlashJobState.ABANDONED,
    ],
)
def test_terminal_states_reject_all_transitions(
    terminal: FlashJobState, to: FlashJobState
) -> None:
    job = _job_in_state(terminal)
    with pytest.raises(InvalidTransition):
        job.transition(to)


def test_failed_can_happen_from_any_active_state() -> None:
    """Hard error during any phase must be allowed to terminate the job."""
    for src in [
        FlashJobState.PENDING,
        FlashJobState.SELECTING_TRANSPORT,
        FlashJobState.WRITING,
        FlashJobState.VERIFYING,
    ]:
        job = _job_in_state(src)
        job.transition(FlashJobState.FAILED)
        assert job.state is FlashJobState.FAILED
        assert job.finished_at is not None


def test_abandoned_can_happen_from_any_active_state() -> None:
    """Operator abort must work from any non-terminal state."""
    for src in [
        FlashJobState.PENDING,
        FlashJobState.SELECTING_TRANSPORT,
        FlashJobState.WRITING,
        FlashJobState.VERIFYING,
    ]:
        job = _job_in_state(src)
        job.transition(FlashJobState.ABANDONED)
        assert job.state is FlashJobState.ABANDONED
        assert job.finished_at is not None


# --- transport selection ----------------------------------------------------


def test_set_transport_only_in_selecting_state() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    # PENDING: not allowed
    with pytest.raises(InvalidTransition):
        job.set_transport(FlashTransport.UF2)
    # SELECTING_TRANSPORT: allowed
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    job.set_transport(FlashTransport.UF2)
    assert job.transport is FlashTransport.UF2


def test_set_transport_is_final() -> None:
    """Per feedback_flash_safety_policy: USB > BLE, never mid-job fallback."""
    job = _job_in_state(FlashJobState.SELECTING_TRANSPORT)
    job.set_transport(FlashTransport.UF2)
    with pytest.raises(InvalidTransition):
        job.set_transport(FlashTransport.BLE_DFU)


# --- verify sub-state -------------------------------------------------------


def test_verify_sub_state_only_in_verifying() -> None:
    job = _job_in_state(FlashJobState.WRITING)
    with pytest.raises(InvalidTransition):
        job.set_verify_sub_state(VerifySubState.AWAITING_REBOOT)
    job.transition(FlashJobState.VERIFYING)
    job.set_verify_sub_state(VerifySubState.AWAITING_REBOOT)
    assert job.verify_sub_state is VerifySubState.AWAITING_REBOOT


def test_verify_sub_state_progresses_freely() -> None:
    """Sub-state order isn't strictly enforced — a fast boot can skip ahead."""
    job = _job_in_state(FlashJobState.VERIFYING)
    job.set_verify_sub_state(VerifySubState.AWAITING_REBOOT)
    # Big skip — direct to VERIFIED. Should be allowed; orchestrator owns ordering.
    job.set_verify_sub_state(VerifySubState.VERIFIED)
    assert job.verify_sub_state is VerifySubState.VERIFIED


# --- anomalies --------------------------------------------------------------


def test_add_anomaly_is_idempotent() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    job.add_anomaly("boot_loop_suspected")
    job.add_anomaly("boot_loop_suspected")
    job.add_anomaly("quarantine_triggered")
    assert job.anomalies == ["boot_loop_suspected", "quarantine_triggered"]


def test_anomaly_bumps_seq() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    s0 = job.seq
    job.add_anomaly("stale_firmware")
    assert job.seq > s0
    # Duplicate add: seq must NOT bump (otherwise pollers see false changes).
    s1 = job.seq
    job.add_anomaly("stale_firmware")
    assert job.seq == s1


# --- progress ---------------------------------------------------------------


def test_record_progress_only_in_writing() -> None:
    job = _job_in_state(FlashJobState.SELECTING_TRANSPORT)
    with pytest.raises(InvalidTransition):
        job.record_progress(10, 100)
    job.set_transport(FlashTransport.UF2)
    job.transition(FlashJobState.WRITING)
    job.record_progress(10, 100)
    assert job.bytes_written == 10
    assert job.bytes_total == 100


@pytest.mark.parametrize(
    "written,total",
    [(-1, 100), (10, -1), (101, 100)],
)
def test_record_progress_rejects_garbage(written: int, total: int) -> None:
    job = _job_in_state(FlashJobState.WRITING)
    with pytest.raises(ValueError):
        job.record_progress(written, total)


# --- timestamps -------------------------------------------------------------


def test_started_at_stamped_only_on_first_writing() -> None:
    """``started_at`` records the *first* WRITING entry. The current state
    machine has only one WRITING transition per job, but the guard is in
    place so a v2 with retries doesn't accidentally reset the stopwatch."""
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    assert job.started_at is None
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    job.set_transport(FlashTransport.UF2)
    job.transition(FlashJobState.WRITING)
    first_start = job.started_at
    assert first_start is not None
    # Can't re-enter WRITING from VERIFYING in the current state machine,
    # so we just confirm started_at hasn't been mutated by other transitions.
    job.transition(FlashJobState.VERIFYING)
    assert job.started_at == first_start


def test_write_completed_at_stamped_on_verifying() -> None:
    job = _job_in_state(FlashJobState.WRITING)
    assert job.write_completed_at is None
    job.transition(FlashJobState.VERIFYING)
    assert job.write_completed_at is not None


def test_terminal_states_stamp_finished_at() -> None:
    for terminal in TERMINAL_STATES:
        job = _job_in_state(FlashJobState.VERIFYING)
        job.transition(terminal)
        assert job.finished_at is not None


# --- duration ---------------------------------------------------------------


def test_duration_grows_while_active(monkeypatch: pytest.MonkeyPatch) -> None:
    fake_now = [1000.0]
    monkeypatch.setattr(time, "time", lambda: fake_now[0])
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    assert job.duration_s == 0.0
    fake_now[0] = 1005.5
    assert job.duration_s == pytest.approx(5.5)


def test_duration_frozen_after_terminal(monkeypatch: pytest.MonkeyPatch) -> None:
    fake_now = [1000.0]
    monkeypatch.setattr(time, "time", lambda: fake_now[0])
    job = _job_in_state(FlashJobState.VERIFYING)
    fake_now[0] = 1010.0
    job.transition(FlashJobState.FAILED)
    frozen = job.duration_s
    fake_now[0] = 9999.0  # wall clock moves on, but the job is done
    assert job.duration_s == frozen


# --- error ------------------------------------------------------------------


def test_set_error_does_not_change_state() -> None:
    """``set_error`` is for attaching a description; transitioning to FAILED
    is a separate, explicit step. This lets the orchestrator log a warning
    on a recoverable hiccup without forcing a terminal."""
    job = _job_in_state(FlashJobState.VERIFYING)
    job.set_error("verify sub-state still AWAITING_HANDSHAKE after 60s — soft")
    assert job.state is FlashJobState.VERIFYING
    assert job.error is not None


# --- to_dict ---------------------------------------------------------------


def test_to_dict_round_trips_via_json() -> None:
    """The API serializes via stdlib json — must not error on any field."""
    import json

    job = _job_in_state(FlashJobState.VERIFYING)
    job.bytes_written = 100
    job.bytes_total = 100
    job.add_anomaly("stale_firmware")
    job.set_verify_sub_state(VerifySubState.AWAITING_VERSION_MATCH)
    d = job.to_dict()
    # Must JSON-serialize cleanly.
    encoded = json.dumps(d)
    decoded = json.loads(encoded)
    # Spot-check the fields the API surface depends on.
    assert decoded["state"] == "verifying"
    assert decoded["verify_sub_state"] == "awaiting_version_match"
    assert decoded["anomalies"] == ["stale_firmware"]
    assert decoded["is_terminal"] is False
    assert decoded["seq"] >= 1


def test_to_dict_when_terminal() -> None:
    job = _job_in_state(FlashJobState.VERIFYING)
    job.transition(FlashJobState.COMPLETED)
    d = job.to_dict()
    assert d["is_terminal"] is True
    assert d["state"] == "completed"
    assert d["finished_at"] is not None


# --- seq monotonicity -------------------------------------------------------


def test_seq_increments_on_every_mutation() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    snaps = [job.seq]
    job.transition(FlashJobState.SELECTING_TRANSPORT); snaps.append(job.seq)
    job.set_transport(FlashTransport.UF2);             snaps.append(job.seq)
    job.transition(FlashJobState.WRITING);             snaps.append(job.seq)
    job.record_progress(10, 100);                      snaps.append(job.seq)
    job.record_progress(50, 100);                      snaps.append(job.seq)
    job.transition(FlashJobState.VERIFYING);           snaps.append(job.seq)
    job.set_verify_sub_state(VerifySubState.AWAITING_REBOOT); snaps.append(job.seq)
    job.add_anomaly("stale_firmware");                 snaps.append(job.seq)
    job.set_error("transient");                        snaps.append(job.seq)
    job.transition(FlashJobState.COMPLETED);           snaps.append(job.seq)
    # Strictly monotonic.
    assert snaps == sorted(snaps)
    assert len(set(snaps)) == len(snaps)


# --- async wait_for_change --------------------------------------------------


@pytest.mark.asyncio
async def test_wait_for_change_returns_on_mutation() -> None:
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    start_seq = job.seq

    async def trigger() -> None:
        await asyncio.sleep(0.02)
        job.transition(FlashJobState.SELECTING_TRANSPORT)

    asyncio.create_task(trigger())
    new_seq = await job.wait_for_change(since_seq=start_seq, timeout=1.0)
    assert new_seq > start_seq


@pytest.mark.asyncio
async def test_wait_for_change_times_out_cleanly() -> None:
    """Timeout returns current seq, doesn't raise. Caller compares to detect."""
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    snap = job.seq
    result = await job.wait_for_change(since_seq=snap, timeout=0.05)
    assert result == snap  # no change observed


@pytest.mark.asyncio
async def test_wait_for_change_does_not_miss_pre_wait_mutation() -> None:
    """If the seq has already advanced past the caller's snapshot, return
    immediately — don't wait for *another* mutation.

    This is the classic 'missed edge' bug pattern. The seq comparison is
    the source of truth; the internal asyncio.Event is just a wake mechanism.
    """
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    snap = job.seq
    # Mutate BEFORE calling wait_for_change.
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    # Should return immediately with the new seq.
    result = await asyncio.wait_for(
        job.wait_for_change(since_seq=snap, timeout=5.0), timeout=0.5
    )
    assert result > snap


# --- transport selection (Phase 2) -----------------------------------------


def test_select_transport_prefers_usb_when_available() -> None:
    """USB > BLE per feedback_flash_safety_policy. Even when BLE is also
    reachable, USB wins — the cascade bug came from these paths racing."""
    probe = TransportProbe(has_usb_app=True, has_ble_dfu_advert=True)
    assert select_transport(probe) is FlashTransport.UF2


def test_select_transport_usb_only() -> None:
    probe = TransportProbe(has_usb_app=True, has_ble_dfu_advert=False)
    assert select_transport(probe) is FlashTransport.UF2


def test_select_transport_ble_only() -> None:
    """Falls back to BLE-DFU when USB-CDC app handshake is absent."""
    probe = TransportProbe(has_usb_app=False, has_ble_dfu_advert=True)
    assert select_transport(probe) is FlashTransport.BLE_DFU


def test_select_transport_raises_when_unreachable() -> None:
    probe = TransportProbe(has_usb_app=False, has_ble_dfu_advert=False)
    with pytest.raises(NoReachableTransport):
        select_transport(probe)


def test_transport_probe_is_frozen() -> None:
    """Selector decisions are auditable — the probe is immutable so a
    later mutation can't retroactively change the recorded inputs."""
    probe = TransportProbe(has_usb_app=True, has_ble_dfu_advert=False)
    with pytest.raises(Exception):  # FrozenInstanceError; subclass of AttributeError
        probe.has_usb_app = False  # type: ignore[misc]


# --- helpers ----------------------------------------------------------------


def _job_in_state(target: FlashJobState) -> FlashJob:
    """Build a job already advanced to ``target`` via the happy-path chain."""
    job = FlashJob(device_id="d", firmware_path=Path("/f"))
    if target == FlashJobState.PENDING:
        return job
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    if target == FlashJobState.SELECTING_TRANSPORT:
        return job
    job.set_transport(FlashTransport.UF2)
    job.transition(FlashJobState.WRITING)
    if target == FlashJobState.WRITING:
        return job
    job.transition(FlashJobState.VERIFYING)
    if target == FlashJobState.VERIFYING:
        return job
    # Terminal targets.
    job.transition(target)
    return job
