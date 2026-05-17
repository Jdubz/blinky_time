"""Phase-3 tests: FleetManager flash-job scaffolding.

Exercises the lock + active-job table + dedup window mechanics with the
Phase-3 stub orchestrator. The stub transitions every job to FAILED with
an explicit "not yet wired" message after selecting the transport, so
each test runs in O(milliseconds) and doesn't need any hardware.

These tests are the foundation for Phase 8's auto-recovery dedup —
``should_attempt_auto_recovery`` and the dedup window need to be
provably correct *before* anything calls them.
"""

from __future__ import annotations

import asyncio
import time
from pathlib import Path

import pytest

from blinky_server.device.manager import FleetManager
from blinky_server.firmware.flash_job import (
    FlashJobState,
    FlashTransport,
)

# --- fixtures ---------------------------------------------------------------


@pytest.fixture
def fleet() -> FleetManager:
    """Bare FleetManager with no real transports — never starts the loop."""
    return FleetManager(enable_ble=False, enable_serial=False)


# --- flash_device basics ----------------------------------------------------


@pytest.mark.asyncio
async def test_flash_device_creates_job_and_registers_it(
    fleet: FleetManager,
) -> None:
    job = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    assert job.device_id == "dev-1"
    assert fleet.get_flash_job("dev-1") is job
    # The orchestrator runs as a background task — wait for it to finish.
    await job.wait_until_terminal(timeout=2.0)
    assert job.is_terminal


@pytest.mark.asyncio
async def test_flash_device_returns_existing_job_under_concurrency(
    fleet: FleetManager,
) -> None:
    """Two concurrent flash_device calls for the same device must produce
    one job, not two. This is the structural guarantee that closes the
    cascade-bug pattern: no path to "two writes for one device."
    """
    # Start two flash requests on the same device "at the same time" via
    # gather. The lock inside flash_device serializes them; the second one
    # must see is_active=True and return the first's job.
    a, b = await asyncio.gather(
        fleet.flash_device("dev-1", Path("/tmp/fw.hex")),
        fleet.flash_device("dev-1", Path("/tmp/fw.hex")),
    )
    assert a.job_id == b.job_id, f"expected same job, got {a.job_id} and {b.job_id}"
    await a.wait_until_terminal(timeout=2.0)


@pytest.mark.asyncio
async def test_flash_device_allows_concurrent_different_devices(
    fleet: FleetManager,
) -> None:
    """The per-device lock must NOT serialize flashes of different
    devices — that would defeat the strict-serial semantics chosen at
    the /fleet/flash level (the orchestrator there sequences devices
    explicitly; per-device locks are for guarding ONE device only)."""
    a, b = await asyncio.gather(
        fleet.flash_device("dev-1", Path("/tmp/fw.hex")),
        fleet.flash_device("dev-2", Path("/tmp/fw.hex")),
    )
    assert a.device_id == "dev-1"
    assert b.device_id == "dev-2"
    assert a.job_id != b.job_id
    await asyncio.gather(
        a.wait_until_terminal(timeout=2.0),
        b.wait_until_terminal(timeout=2.0),
    )


@pytest.mark.asyncio
async def test_flash_device_after_completion_creates_new_job(
    fleet: FleetManager,
) -> None:
    """Once a job is terminal, the next flash_device call creates a fresh
    job (different job_id). The dedup window only blocks auto-recovery
    via ``should_attempt_auto_recovery`` — explicit flashes always proceed.
    """
    first = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    await first.wait_until_terminal(timeout=2.0)
    second = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    assert first.job_id != second.job_id
    await second.wait_until_terminal(timeout=2.0)


# --- stub orchestrator behavior --------------------------------------------


@pytest.mark.asyncio
async def test_stub_fails_with_no_reachable_transport_when_device_absent(
    fleet: FleetManager,
) -> None:
    """No device → probe says nothing reachable → NoReachableTransport →
    job ends FAILED with a descriptive error."""
    job = await fleet.flash_device("unknown-device", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    assert job.state is FlashJobState.FAILED
    assert job.transport is None  # never got past selection
    assert job.error is not None
    assert "not reachable" in job.error.lower()


@pytest.mark.asyncio
async def test_uf2_path_picked_for_serial_device_without_firmware_file(
    fleet: FleetManager,
) -> None:
    """When a USB-app device IS reachable, ``_run_flash_job`` selects UF2
    and enters the WRITING phase. With a non-existent firmware file the
    subprocess wrapper fails fast with "firmware file not found" — proves
    the orchestrator passes selection and routes into the UF2 branch.

    Phase 7 replaced the "transport not yet wired" stub error from
    Phase 3 with the real UF2 wrapper, so this test now checks for the
    file-missing error instead.
    """
    from blinky_server.device.device import Device

    from .mock_transport import MockTransport

    transport = MockTransport(transport_type="serial")
    device = Device(
        device_id="TEST_SERIAL_001",
        port="/dev/ttyTEST0",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()
    fleet._devices["TEST_SERIAL_001"] = device
    job = await fleet.flash_device("TEST_SERIAL_001", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=5.0)
    assert job.state is FlashJobState.FAILED
    assert job.transport is FlashTransport.UF2  # selection happened
    assert job.error is not None
    # Reached the UF2 wrapper, which fails on missing firmware file.
    assert "firmware file not found" in job.error.lower()


# --- should_attempt_auto_recovery -----------------------------------------


@pytest.mark.asyncio
async def test_auto_recovery_allowed_for_fresh_device(
    fleet: FleetManager,
) -> None:
    """No history → auto-recovery allowed."""
    assert fleet.should_attempt_auto_recovery("dev-1") is True


@pytest.mark.asyncio
async def test_auto_recovery_blocked_while_flash_in_flight(
    fleet: FleetManager,
) -> None:
    """The stub finishes within milliseconds, so we need to inspect the
    state DURING the orchestrator's run. We do this by checking
    ``should_attempt_auto_recovery`` AFTER seeding ``_flash_jobs`` with
    a job that's manually held in PENDING — equivalent to mid-flight."""
    from blinky_server.firmware.flash_job import FlashJob

    job = FlashJob(device_id="dev-1", firmware_path=Path("/tmp/fw.hex"))
    fleet._flash_jobs["dev-1"] = job  # PENDING, is_active=True
    assert fleet.should_attempt_auto_recovery("dev-1") is False


@pytest.mark.asyncio
async def test_auto_recovery_blocked_within_dedup_window(
    fleet: FleetManager,
) -> None:
    job = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    # Just finished → within dedup window → blocked.
    assert fleet.should_attempt_auto_recovery("dev-1") is False


@pytest.mark.asyncio
async def test_auto_recovery_allowed_after_window_expires(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Shrink the window for the test, then advance time past it."""
    monkeypatch.setattr(FleetManager, "FLASH_DEDUP_WINDOW_S", 0.05)
    job = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    # Immediately after: blocked.
    assert fleet.should_attempt_auto_recovery("dev-1") is False
    await asyncio.sleep(0.1)  # past the window
    assert fleet.should_attempt_auto_recovery("dev-1") is True


@pytest.mark.asyncio
async def test_recent_flash_attempts_stamped_on_terminal(
    fleet: FleetManager,
) -> None:
    """Even a FAILED job populates ``_recent_flash_attempts`` — dedup must
    catch "I just failed at this" as much as "I just succeeded."""
    before = time.time()
    job = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    assert job.state is FlashJobState.FAILED
    after = time.time()
    stamped = fleet._recent_flash_attempts.get("dev-1")
    assert stamped is not None
    assert before <= stamped <= after


# --- list_flash_jobs --------------------------------------------------------


@pytest.mark.asyncio
async def test_list_flash_jobs_all_vs_active(fleet: FleetManager) -> None:
    a = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"))
    await a.wait_until_terminal(timeout=2.0)
    b = await fleet.flash_device("dev-2", Path("/tmp/fw.hex"))
    await b.wait_until_terminal(timeout=2.0)
    all_jobs = fleet.list_flash_jobs()
    assert len(all_jobs) == 2
    # Both are terminal at this point, so active should be empty.
    assert fleet.list_flash_jobs(active_only=True) == []
