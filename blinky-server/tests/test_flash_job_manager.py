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
from typing import Any

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


# --- L0: transport probe DFU_RECOVERY detection ----------------------------


def test_probe_detects_dfu_recovery_state(fleet: FleetManager) -> None:
    """A device parked in DFU_RECOVERY with a known BLE address must
    set ``has_ble_dfu_advert=True`` so ``select_transport`` picks BLE_DFU.
    Was stubbed to False before L0; the bug was that the new orchestrator
    couldn't reach BLE-DFU even when the device was the only thing on
    BLE-DFU (legacy `upload_firmware`'s DFU_RECOVERY fast path)."""
    from blinky_server.device.device import Device, DeviceState

    from .mock_transport import MockTransport

    device = Device(
        device_id="dfu-victim",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    device.ble_address = "E9:A8:5C:A5:BB:BE"
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["dfu-victim"] = device

    probe = fleet._build_transport_probe("dfu-victim")
    assert probe.has_usb_app is False
    assert probe.has_ble_dfu_advert is True


def test_probe_no_ble_dfu_when_state_not_dfu_recovery(fleet: FleetManager) -> None:
    """Connected BLE device that is NOT in bootloader: probe says no
    BLE-DFU available. Orchestrator's `_run_ble_dfu_flash` is responsible
    for issuing the `bootloader ble` NUS command if it's called anyway;
    the probe just reports current reachability."""
    from blinky_server.device.device import Device, DeviceState

    from .mock_transport import MockTransport

    device = Device(
        device_id="healthy-ble",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    device.ble_address = "AA:BB:CC:DD:EE:FF"
    device.state = DeviceState.PRESENT  # advertising but not in DFU
    fleet._devices["healthy-ble"] = device

    probe = fleet._build_transport_probe("healthy-ble")
    assert probe.has_ble_dfu_advert is False


def test_probe_no_ble_dfu_when_ble_address_unknown(fleet: FleetManager) -> None:
    """DFU_RECOVERY state but no captured BLE address → can't reach via
    BLE-DFU; probe must report False. (Discovery normally provides the
    address; this guards the corner case where state was set without it.)"""
    from blinky_server.device.device import Device, DeviceState

    from .mock_transport import MockTransport

    device = Device(
        device_id="no-addr",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    # ble_address intentionally left None (Device default)
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["no-addr"] = device

    probe = fleet._build_transport_probe("no-addr")
    assert probe.has_ble_dfu_advert is False


# --- L0: _run_ble_dfu_flash orchestrator -----------------------------------


@pytest.mark.asyncio
async def test_ble_dfu_flash_missing_ble_address_fails_cleanly(
    fleet: FleetManager,
) -> None:
    """Defensive guard: if for some reason the orchestrator reaches the
    BLE-DFU branch with a device that has no BLE address, fail loud
    rather than hand `None` to ``upload_ble_dfu``."""
    from blinky_server.device.device import Device, DeviceState

    from .mock_transport import MockTransport

    device = Device(
        device_id="no-addr",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    # ble_address intentionally left None (Device default)
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["no-addr"] = device

    # Force the BLE branch by setting state to DFU_RECOVERY (probe says
    # has_ble_dfu_advert=False because BLE addr is None, but the test
    # exercises the orchestrator's defensive guard for the case where
    # somehow we still got routed here).
    # ``select_transport`` will actually raise NoReachableTransport on
    # this combination, so what we're really validating is that the
    # error path produces a useful message either way.
    job = await fleet.flash_device("no-addr", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    assert job.state is FlashJobState.FAILED
    assert job.error is not None


@pytest.mark.asyncio
async def test_ble_dfu_flash_calls_upload_ble_dfu_for_dfu_recovery(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """A device in DFU_RECOVERY must invoke ``upload_ble_dfu`` WITHOUT any
    bootloader-entry callable (it's already in bootloader). Replicates
    the legacy ``upload_firmware`` DFU_RECOVERY short-circuit."""
    from blinky_server.device.device import Device, DeviceState
    from blinky_server.firmware import ble_dfu as ble_dfu_mod
    from blinky_server.firmware import compile as compile_mod

    from .mock_transport import MockTransport

    device = Device(
        device_id="dfu-victim",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    device.ble_address = "E9:A8:5C:A5:BB:BE"
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["dfu-victim"] = device

    fw_file = tmp_path / "fw.hex"
    fw_file.write_text(":00000001FF\n")

    captured_kwargs: dict[str, Any] = {}

    async def fake_upload_ble_dfu(**kwargs: Any) -> dict[str, Any]:
        captured_kwargs.update(kwargs)
        return {"status": "ok", "message": "mocked", "elapsed_s": 0.1}

    monkeypatch.setattr(ble_dfu_mod, "upload_ble_dfu", fake_upload_ble_dfu)
    monkeypatch.setattr(compile_mod, "ensure_dfu_zip", lambda p: f"{p}.dfu.zip")

    job = await fleet.flash_device("dfu-victim", fw_file)
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.COMPLETED, f"got {job.state}, error={job.error}"
    assert job.transport is FlashTransport.BLE_DFU
    # Critical: NO bootloader-entry callable for the DFU_RECOVERY case.
    assert captured_kwargs["enter_bootloader_via_serial"] is None
    assert captured_kwargs["enter_bootloader_via_ble"] is None
    assert captured_kwargs["app_ble_address"] == "E9:A8:5C:A5:BB:BE"


@pytest.mark.asyncio
async def test_ble_dfu_flash_fails_on_upload_error(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """``upload_ble_dfu`` returning ``status != "ok"`` must transition
    the FlashJob to FAILED with the upstream message attached."""
    from blinky_server.device.device import Device, DeviceState
    from blinky_server.firmware import ble_dfu as ble_dfu_mod
    from blinky_server.firmware import compile as compile_mod

    from .mock_transport import MockTransport

    device = Device(
        device_id="will-fail",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    device.ble_address = "11:22:33:44:55:66"
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["will-fail"] = device

    fw_file = tmp_path / "fw.hex"
    fw_file.write_text(":00000001FF\n")

    async def failing_upload_ble_dfu(**kwargs: Any) -> dict[str, Any]:
        return {"status": "error", "message": "preflight RSSI too weak", "elapsed_s": 1.2}

    monkeypatch.setattr(ble_dfu_mod, "upload_ble_dfu", failing_upload_ble_dfu)
    monkeypatch.setattr(compile_mod, "ensure_dfu_zip", lambda p: f"{p}.dfu.zip")

    job = await fleet.flash_device("will-fail", fw_file)
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.FAILED
    assert job.error is not None
    assert "preflight RSSI too weak" in job.error


@pytest.mark.asyncio
async def test_ble_dfu_flash_zip_build_failure_fails_cleanly(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """If ``ensure_dfu_zip`` raises, the orchestrator must surface it as
    a job error rather than letting the exception escape and crash the
    background task."""
    from blinky_server.device.device import Device, DeviceState
    from blinky_server.firmware import compile as compile_mod

    from .mock_transport import MockTransport

    device = Device(
        device_id="zip-fail",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    device.ble_address = "11:22:33:44:55:66"
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["zip-fail"] = device

    fw_file = tmp_path / "fw.hex"
    fw_file.write_text(":00000001FF\n")

    def explode(p: str) -> str:
        raise RuntimeError("adafruit-nrfutil not on PATH")

    monkeypatch.setattr(compile_mod, "ensure_dfu_zip", explode)

    job = await fleet.flash_device("zip-fail", fw_file)
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.FAILED
    assert job.error is not None
    assert "DFU zip build failed" in job.error
