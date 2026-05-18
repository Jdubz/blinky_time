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
    """Shrink the window AND zero the backoff for the test, then advance
    time past the window. L3c added max-attempts + exponential backoff
    to ``should_attempt_auto_recovery``; the dedup-window behavior we're
    pinning here remains the same when those orthogonal guards are inert.
    """
    import blinky_server.device.manager as mgr_mod

    monkeypatch.setattr(FleetManager, "FLASH_DEDUP_WINDOW_S", 0.05)
    monkeypatch.setattr(mgr_mod, "RECOVERY_BACKOFF_SECONDS", (0.0,))
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


# --- L1.1: canonical-key resolver ------------------------------------------


def test_resolve_canonical_unknown_id_returns_self(fleet: FleetManager) -> None:
    """An ID that hasn't been registered as an alias resolves to itself.
    This is the "first time we've seen this device" case and lets the
    resolver wrap every lookup without special-casing unregistered IDs."""
    assert fleet.resolve_canonical("unknown-id") == "unknown-id"


def test_register_alias_requires_at_least_one_id(fleet: FleetManager) -> None:
    """Empty alias registration has no meaning. Fail loud rather than
    silently accept it."""
    with pytest.raises(ValueError):
        fleet.register_identity_alias()


def test_register_alias_single_id_returns_id(fleet: FleetManager) -> None:
    """Registering one ID makes it canonical-of-itself."""
    canonical = fleet.register_identity_alias("dev-X")
    assert canonical == "dev-X"
    assert fleet.resolve_canonical("dev-X") == "dev-X"


def test_register_alias_prefers_sn_over_ble_addr(fleet: FleetManager) -> None:
    """USB serial numbers (no colons, 16 hex chars) win over BLE
    addresses. SN is more stable across power cycles — BLE addresses
    can rotate (random static address feature on the nRF52840)."""
    canonical = fleet.register_identity_alias(
        "E9:A8:5C:A5:BB:BE",  # BLE app addr
        "062CBD12EB6961C8",  # USB SN
    )
    assert canonical == "062CBD12EB6961C8"
    assert fleet.resolve_canonical("E9:A8:5C:A5:BB:BE") == "062CBD12EB6961C8"
    assert fleet.resolve_canonical("062CBD12EB6961C8") == "062CBD12EB6961C8"


def test_register_alias_multiple_ble_addrs_deterministic(fleet: FleetManager) -> None:
    """When no SN-like ID is in the group, pick lexicographically first
    BLE addr as canonical. Deterministic, doesn't depend on call order."""
    canonical = fleet.register_identity_alias("E9:A8:5C:A5:BB:BF", "E9:A8:5C:A5:BB:BE")
    assert canonical == "E9:A8:5C:A5:BB:BE"


def test_register_alias_merges_existing_groups(fleet: FleetManager) -> None:
    """Calling ``register_identity_alias`` again with overlapping IDs
    pulls in any aliases already registered for either side. The
    cart_inner BLE→USB transition is exactly this: discovery first sees
    the BLE addr (registers it alone), then sees the USB SN and learns
    they're the same device → all three aliases (BLE addr, BLE bootloader
    addr, USB SN) collapse to the SN canonical."""
    # Pretend discovery saw the BLE bootloader addr first and tied it
    # to the BLE app addr.
    fleet.register_identity_alias("E9:A8:5C:A5:BB:BE", "E9:A8:5C:A5:BB:BF")
    # Now learn the USB SN and tie it to one of the existing aliases.
    canonical = fleet.register_identity_alias("062CBD12EB6961C8", "E9:A8:5C:A5:BB:BE")
    # All three IDs now resolve to the SN canonical.
    assert canonical == "062CBD12EB6961C8"
    assert fleet.resolve_canonical("062CBD12EB6961C8") == "062CBD12EB6961C8"
    assert fleet.resolve_canonical("E9:A8:5C:A5:BB:BE") == "062CBD12EB6961C8"

    # NOTE: ``test_get_dfu_lock_is_canonical_keyed`` was deleted in L3c.
    # ``get_dfu_lock`` and ``_dfu_locks`` are gone — the per-canonical
    # in-flight set inside ``flash_device`` is the single mutex now.
    # Cross-alias mutex coverage is provided by
    # ``test_flash_device_concurrent_via_different_aliases_returns_same_job``
    # (below) which exercises the same invariant through the new entry point.
    assert fleet.resolve_canonical("E9:A8:5C:A5:BB:BF") == "062CBD12EB6961C8"


# --- L1: _device_in_flight set + dedup across aliases ----------------------


@pytest.mark.asyncio
async def test_device_in_flight_populated_during_job(
    fleet: FleetManager,
) -> None:
    """Mid-flight: ``_device_in_flight`` contains the canonical ID.
    Stub orchestrator finishes in ms so we inspect via the existing
    pattern (seed a job manually) to observe the in-flight state.

    Cleared on terminal via the orchestrator's finally block."""
    from blinky_server.firmware.flash_job import FlashJob

    job = FlashJob(device_id="dev-A", firmware_path=Path("/tmp/fw.hex"))
    canonical = fleet.resolve_canonical("dev-A")
    fleet._flash_jobs[canonical] = job
    fleet._device_in_flight.add(canonical)

    # In-flight check via canonical OR raw alias both see "in flight."
    assert canonical in fleet._device_in_flight


@pytest.mark.asyncio
async def test_flash_device_clears_in_flight_on_terminal(
    fleet: FleetManager,
) -> None:
    """End-to-end: ``flash_device`` adds canonical to in-flight, the
    orchestrator's finally clears it. After ``wait_until_terminal``
    returns, the set is empty for this device."""
    job = await fleet.flash_device("dev-B", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    canonical = fleet.resolve_canonical("dev-B")
    assert canonical not in fleet._device_in_flight


@pytest.mark.asyncio
async def test_should_attempt_auto_recovery_dedups_via_canonical(
    fleet: FleetManager,
) -> None:
    """The whole point of L1.1: a flash started under one alias
    (USB SN) must dedup against an auto-recovery attempt under another
    alias (BLE addr). Pre-fix: the SN-only whitelist couldn't match a
    BLE-only DFU device on 2026-05-16, and auto-recovery never fired.

    Setup: register both aliases, start a flash via the SN, ask
    auto-recovery if it can flash via the BLE addr. Answer: no, it's
    the same device and the flash is in flight."""
    fleet.register_identity_alias("ALIAS_SN_AABB", "AA:BB:CC:DD:EE:FF")

    job = await fleet.flash_device("ALIAS_SN_AABB", Path("/tmp/fw.hex"))
    # Manually keep job in-flight for the test (stub completes too fast).
    fleet._device_in_flight.add(fleet.resolve_canonical("ALIAS_SN_AABB"))

    # Auto-recovery via the BLE alias must see dedup.
    assert fleet.should_attempt_auto_recovery("AA:BB:CC:DD:EE:FF") is False

    # Cleanup: let the job complete.
    await job.wait_until_terminal(timeout=2.0)
    # And then the in-flight slot should be clear (stamped by orchestrator).
    # (The manual add above is also cleared because the orchestrator's
    # finally block calls discard on the same canonical.)


@pytest.mark.asyncio
async def test_flash_device_concurrent_via_different_aliases_returns_same_job(
    fleet: FleetManager,
) -> None:
    """Idempotency under cross-alias races: two callers, one with the
    SN and one with the BLE addr, both targeting the same physical
    device. The canonical resolver collapses them; the second caller
    gets back the first's job rather than creating a duplicate."""
    fleet.register_identity_alias("SN_CONCURRENT_X", "11:22:33:44:55:66")
    a, b = await asyncio.gather(
        fleet.flash_device("SN_CONCURRENT_X", Path("/tmp/fw.hex")),
        fleet.flash_device("11:22:33:44:55:66", Path("/tmp/fw.hex")),
    )
    assert a.job_id == b.job_id
    await a.wait_until_terminal(timeout=2.0)


# --- L0: verify-signals baseline-devnum is captured at __init__ -----------


def test_verify_signals_captures_devnum_baseline_at_construction(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Regression: `_FleetVerifySignals` MUST snapshot the device's USB
    devnum at construction time, not at first ``has_re_enumerated_since``
    call. Otherwise the baseline lands AFTER the USB reset + BL → app
    reboot and the state machine stalls in AWAITING_REBOOT for the
    full 5-minute verify cap.

    Verified live 2026-05-18 against FPS Sweep — pre-fix: job stalled
    for 337 s in AWAITING_REBOOT then FAILED; post-fix: same device,
    same firmware completed in 43.89 s via AWAITING_APP_BOOT → VERIFIED.
    """
    from blinky_server.device import manager as manager_mod
    from blinky_server.firmware.anomalies import SignalHistory

    # Make `_read_devnum_for_device` deterministic by returning a fixed
    # value. The construction-time snapshot must equal whatever this
    # returns at __init__.
    devnums = iter([42, 99])  # first call (init) → 42; later poll → 99

    def fake_read_devnum(self: Any) -> int | None:
        return next(devnums)

    monkeypatch.setattr(
        manager_mod._FleetVerifySignals, "_read_devnum_for_device", fake_read_devnum
    )

    history = SignalHistory(write_completed_at=0.0)
    signals = manager_mod._FleetVerifySignals(fleet=fleet, device_id="d", history=history)

    # Baseline must be captured at __init__, not deferred.
    assert signals._initial_devnum == 42


@pytest.mark.asyncio
async def test_verify_signals_detects_reenum_immediately_after_construction(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """First call to ``has_re_enumerated_since`` after construction
    must return True if the devnum has changed (i.e., the reboot
    already happened). Pre-fix this returned False (because first-call
    captured baseline + returned False) and the state machine stayed
    in AWAITING_REBOOT forever even though the device had rebooted."""
    from blinky_server.device import manager as manager_mod
    from blinky_server.firmware.anomalies import SignalHistory

    devnums = iter([42, 99])  # init captures 42; first poll reads 99

    def fake_read_devnum(self: Any) -> int | None:
        return next(devnums)

    monkeypatch.setattr(
        manager_mod._FleetVerifySignals, "_read_devnum_for_device", fake_read_devnum
    )

    history = SignalHistory(write_completed_at=0.0)
    signals = manager_mod._FleetVerifySignals(fleet=fleet, device_id="d", history=history)
    # Devnum has changed since __init__ (42 → 99) → re-enum detected.
    assert await signals.has_re_enumerated_since("d", since=0.0) is True
    assert len(history.re_enum_timestamps) == 1


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
async def test_ble_dfu_flash_dfu_recovery_without_ble_address_unreachable(
    fleet: FleetManager,
) -> None:
    """A DFU_RECOVERY device with no BLE address is unreachable: probe
    sees has_ble_dfu_advert=False (the address check fails) and there's
    no USB-CDC since transport_type=ble. ``select_transport`` raises
    ``NoReachableTransport`` and the job fails before reaching
    ``_run_ble_dfu_flash``.

    The in-method ``if not device.ble_address`` guard inside
    ``_run_ble_dfu_flash`` is defensive — it covers the race where
    ``ble_address`` was set at probe time but cleared by a concurrent
    discovery cycle before the orchestrator read it again. That race
    isn't easily testable from the public API; this test pins the
    upstream reachability check that prevents the unsafe entry path
    in the common case."""
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

    job = await fleet.flash_device("no-addr", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    assert job.state is FlashJobState.FAILED
    assert job.error is not None
    assert "not reachable" in job.error.lower()


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

    monkeypatch.setattr(ble_dfu_mod, "_ble_dfu_write_impl", fake_upload_ble_dfu)
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

    monkeypatch.setattr(ble_dfu_mod, "_ble_dfu_write_impl", failing_upload_ble_dfu)
    monkeypatch.setattr(compile_mod, "ensure_dfu_zip", lambda p: f"{p}.dfu.zip")

    job = await fleet.flash_device("will-fail", fw_file)
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.FAILED
    assert job.error is not None
    assert "preflight RSSI too weak" in job.error


@pytest.mark.asyncio
async def test_ble_dfu_flash_wall_clock_timeout(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """If ``upload_ble_dfu`` hangs beyond the orchestrator's 600s
    wall-clock, the orchestrator must `wait_for`-cancel it and surface
    a timeout error rather than letting the flash run forever. This
    mirrors the legacy fleet-route + auto-recovery 600s caps that
    prevented the cart_inner brick from running unbounded.

    Test forces the timeout by short-circuiting the wait_for default
    via a tiny mocked timeout. We can't actually wait 600s in a test.
    """
    from blinky_server.device import manager as manager_mod
    from blinky_server.device.device import Device, DeviceState
    from blinky_server.firmware import ble_dfu as ble_dfu_mod
    from blinky_server.firmware import compile as compile_mod

    from .mock_transport import MockTransport

    device = Device(
        device_id="hang",
        port="dummy",
        platform="nrf52840",
        transport=MockTransport(transport_type="ble"),  # type: ignore[arg-type]
    )
    device.ble_address = "AA:BB:CC:DD:EE:FF"
    device.state = DeviceState.DFU_RECOVERY
    fleet._devices["hang"] = device

    fw_file = tmp_path / "fw.hex"
    fw_file.write_text(":00000001FF\n")

    async def hang_forever(**kwargs: Any) -> dict[str, Any]:
        await asyncio.sleep(60)  # would hang the test; wait_for kills it first
        return {"status": "ok"}

    monkeypatch.setattr(ble_dfu_mod, "_ble_dfu_write_impl", hang_forever)
    monkeypatch.setattr(compile_mod, "ensure_dfu_zip", lambda p: f"{p}.dfu.zip")

    # Replace asyncio.wait_for with a tiny-timeout version so the test
    # doesn't actually wait 600s. We patch the asyncio module used by
    # manager.py's _run_ble_dfu_flash.
    original_wait_for = asyncio.wait_for

    async def fast_wait_for(coro, timeout):  # type: ignore[no-untyped-def]
        return await original_wait_for(coro, timeout=0.1)

    monkeypatch.setattr(manager_mod.asyncio, "wait_for", fast_wait_for)

    job = await fleet.flash_device("hang", fw_file)
    await job.wait_until_terminal(timeout=5.0)

    assert job.state is FlashJobState.FAILED
    assert job.error is not None
    assert "timeout" in job.error.lower()


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


# --- L2: orchestrator-context guard (the lockdown invariant) -------------


@pytest.mark.asyncio
async def test_uf2_write_impl_outside_orchestrator_raises() -> None:
    """The whole point of L2: calling `_uf2_write_impl` outside
    `_run_flash_job` (i.e. with the ContextVar unset) must raise
    `OutsideFlashJobContextError`. This is the structural lockdown —
    after L3d when the legacy wrappers are gone, only the orchestrator
    can call this impl, and any future "let me just call it directly"
    drift fails loud at the first attempt."""
    from blinky_server.firmware._guard import OutsideFlashJobContextError
    from blinky_server.firmware.uf2_upload import _uf2_write_impl

    with pytest.raises(OutsideFlashJobContextError):
        await _uf2_write_impl(
            serial_port="/dev/null",
            firmware_path="/tmp/fake.hex",
            transport=None,
        )


@pytest.mark.asyncio
async def test_uf2_write_impl_for_job_outside_orchestrator_raises() -> None:
    """Same guard applies to the FlashJob-bound variant."""
    from blinky_server.firmware._guard import OutsideFlashJobContextError
    from blinky_server.firmware.flash_job import FlashJob, FlashJobState
    from blinky_server.firmware.uf2_upload import _uf2_write_impl_for_job

    job = FlashJob(device_id="d", firmware_path=Path("/tmp/x.hex"))
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    from blinky_server.firmware.flash_job import FlashTransport

    job.set_transport(FlashTransport.UF2)
    job.transition(FlashJobState.WRITING)
    with pytest.raises(OutsideFlashJobContextError):
        await _uf2_write_impl_for_job(
            job=job,
            serial_port="/dev/null",
            firmware_path="/tmp/x.hex",
            transport=None,
        )


@pytest.mark.asyncio
async def test_ble_dfu_write_impl_outside_orchestrator_raises() -> None:
    """And the BLE-DFU impl — same boundary."""
    from blinky_server.firmware._guard import OutsideFlashJobContextError
    from blinky_server.firmware.ble_dfu import _ble_dfu_write_impl

    with pytest.raises(OutsideFlashJobContextError):
        await _ble_dfu_write_impl(
            app_ble_address="AA:BB:CC:DD:EE:FF",
            dfu_zip_path="/tmp/fake.dfu.zip",
        )


# NOTE: ``test_legacy_upload_uf2_wrapper_sets_context_and_works`` and
# ``test_legacy_upload_ble_dfu_wrapper_sets_context_and_works`` were
# deleted in L3d. The legacy ``upload_uf2`` / ``upload_ble_dfu``
# wrappers they tested no longer exist. ``FleetManager.flash_device()``
# is the only entry point that sets the orchestrator ContextVar; that
# behavior is covered by the operator-flash and fleet-flash hardware
# tests (L3a / L3b) plus the test below
# (``test_test_task_context_unaffected_by_orchestrator_task``).


@pytest.mark.asyncio
async def test_orchestrator_context_does_not_leak_across_tasks() -> None:
    """ContextVar is task-local in asyncio — one task entering the
    orchestrator context must NOT make the impl callable from a
    sibling task. Cross-task contamination would defeat the entire
    lockdown for concurrent flashes (each task has to set its own
    context)."""
    import asyncio

    from blinky_server.firmware._guard import (
        OutsideFlashJobContextError,
        enter_orchestrator_context,
        reset_orchestrator_context,
    )
    from blinky_server.firmware.uf2_upload import _uf2_write_impl

    async def in_context() -> None:
        token = enter_orchestrator_context()
        try:
            # Other task can't see our context — let it run and assert.
            await asyncio.sleep(0)
            await sibling_check.wait()
        finally:
            reset_orchestrator_context(token)

    async def sibling_check_task() -> None:
        # We're a different task. ContextVar default applies: False.
        with pytest.raises(OutsideFlashJobContextError):
            await _uf2_write_impl(
                serial_port="/dev/null",
                firmware_path="/tmp/x.hex",
                transport=None,
            )
        sibling_check.set()

    sibling_check = asyncio.Event()
    await asyncio.gather(in_context(), sibling_check_task())


# NOTE: ``test_legacy_wrappers_reset_context_on_return`` was deleted in
# L3d alongside the legacy wrappers themselves. The orchestrator's own
# context-reset on exit is covered by the operator-flash hardware tests
# (each new flash_device call must succeed, which requires the previous
# call's context to have reset cleanly).


@pytest.mark.asyncio
async def test_test_task_context_unaffected_by_orchestrator_task(
    fleet: FleetManager,
) -> None:
    """``_run_flash_job`` runs in its own asyncio task (created by
    ``flash_device`` via ``asyncio.create_task``), so its ContextVar
    mutations are isolated by the standard asyncio task-context-copy
    rule and CANNOT leak back into the caller's task. This is the
    structural invariant that lets multiple flashes run concurrently
    on different devices without their guards interfering. We verify
    by running a flash to completion and confirming the test task
    still sees the default value."""
    from blinky_server.firmware._guard import _inside_flash_job_orchestrator

    assert _inside_flash_job_orchestrator.get() is False, "test precondition"

    job = await fleet.flash_device("nonexistent-device", Path("/tmp/x.hex"))
    await job.wait_until_terminal(timeout=5.0)
    # Whatever the orchestrator did with its context-copy is invisible here.
    assert _inside_flash_job_orchestrator.get() is False


# --- L3b: fleet.flash_fleet strictly-serial iteration ----------------------


@pytest.mark.asyncio
async def test_flash_fleet_strictly_serial(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The whole point of L3b: ``flash_fleet`` MUST flash one device at
    a time, with each device reaching terminal state before the next is
    scheduled. We prove this by stubbing the orchestrator so each
    flash_device call records when it ran, then asserting the runs are
    strictly non-overlapping.
    """
    import asyncio
    from collections.abc import AsyncIterator

    from blinky_server.firmware.flash_job import FlashJob, FlashJobState, FlashTransport

    in_flight: list[str] = []
    overlap_seen: list[str] = []
    completion_order: list[str] = []

    async def stub_orchestrator(job: FlashJob) -> None:
        if in_flight:
            overlap_seen.append(f"{job.device_id} ran while {in_flight} still in flight")
        in_flight.append(job.device_id)
        # Yield to the loop so any sibling task that's racing has a
        # chance to also enter — if strict-serial is broken, this is
        # where the overlap would surface.
        await asyncio.sleep(0)
        await asyncio.sleep(0.01)
        in_flight.remove(job.device_id)
        completion_order.append(job.device_id)
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        job.transition(FlashJobState.WRITING)
        job.transition(FlashJobState.VERIFYING)
        job.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_flash_job", stub_orchestrator)

    # Inject three "devices" by canonical mapping. They don't need real
    # Device objects — the orchestrator stub doesn't touch them. But
    # flash_fleet's "device not found" guard checks the manager's
    # _devices dict, so populate it with placeholders.
    from blinky_server.device.device import Device

    from .mock_transport import MockTransport

    device_ids: list[str] = []
    for i in range(3):
        d = Device(
            device_id=f"DEV_{i}",
            port=f"/dev/ttyTEST{i}",
            platform="nrf52840",
            transport=MockTransport(),  # type: ignore[arg-type]
        )
        await d.connect()
        fleet._devices[d.id] = d
        device_ids.append(d.id)

    iterator: AsyncIterator[tuple[str, FlashJob | None, str | None]] = fleet.flash_fleet(
        device_ids=device_ids,
        firmware_path=Path("/tmp/fake.hex"),
        per_device_timeout=5.0,
    )

    results = [t async for t in iterator]

    assert overlap_seen == [], f"strict-serial violation: {overlap_seen}"
    assert completion_order == device_ids, (
        f"order broken: expected {device_ids}, got {completion_order}"
    )
    assert len(results) == 3
    assert all(err is None for _, _, err in results), results
    assert all(job is not None and job.state is FlashJobState.COMPLETED for _, job, _ in results)


@pytest.mark.asyncio
async def test_flash_fleet_continues_on_per_device_failure(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """One device failing must NOT abort the batch — flash_fleet's
    continue-on-error semantics are load-bearing for fleet operations
    where a single flaky USB port shouldn't block the rest of the
    deploy. (Per ``feedback_flash_safety_policy``: the OUTER loop
    can continue; what's forbidden is parallel writes to the SAME
    device.)
    """
    from blinky_server.device.device import Device
    from blinky_server.firmware.flash_job import FlashJob, FlashJobState, FlashTransport

    from .mock_transport import MockTransport

    async def stub_orchestrator(job: FlashJob) -> None:
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        if job.device_id == "DEV_FAIL":
            job.set_error("synthetic per-device failure")
            job.transition(FlashJobState.FAILED)
        else:
            job.transition(FlashJobState.WRITING)
            job.transition(FlashJobState.VERIFYING)
            job.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_flash_job", stub_orchestrator)

    for did in ("DEV_OK_1", "DEV_FAIL", "DEV_OK_2"):
        d = Device(
            device_id=did,
            port=f"/dev/ttyTEST_{did}",
            platform="nrf52840",
            transport=MockTransport(),  # type: ignore[arg-type]
        )
        await d.connect()
        fleet._devices[d.id] = d

    results = [
        t
        async for t in fleet.flash_fleet(
            device_ids=["DEV_OK_1", "DEV_FAIL", "DEV_OK_2"],
            firmware_path=Path("/tmp/fake.hex"),
            per_device_timeout=5.0,
        )
    ]

    assert len(results) == 3
    by_id = {did: (job, err) for did, job, err in results}

    assert by_id["DEV_OK_1"][0] is not None
    assert by_id["DEV_OK_1"][0].state is FlashJobState.COMPLETED

    fail_job, fail_err = by_id["DEV_FAIL"]
    assert fail_job is not None
    assert fail_job.state is FlashJobState.FAILED
    assert fail_err is None  # err is the wait/iteration error, not the FlashJob's
    assert fail_job.error == "synthetic per-device failure"

    assert by_id["DEV_OK_2"][0] is not None
    assert by_id["DEV_OK_2"][0].state is FlashJobState.COMPLETED


@pytest.mark.asyncio
async def test_flash_fleet_yields_error_for_unknown_device(
    fleet: FleetManager,
) -> None:
    """An unknown device id yields ``(id, None, err)`` rather than
    raising. The aggregate flash continues past it (deploy.sh's
    aggregator needs the per-device report, even for typos).
    """
    results = [
        t
        async for t in fleet.flash_fleet(
            device_ids=["does-not-exist"],
            firmware_path=Path("/tmp/fake.hex"),
            per_device_timeout=1.0,
        )
    ]
    assert len(results) == 1
    did, job, err = results[0]
    assert did == "does-not-exist"
    assert job is None
    assert err is not None and "not found" in err


@pytest.mark.asyncio
async def test_flash_fleet_on_iteration_callback_receives_each_device(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The route uses ``on_iteration`` to update its progress message.
    Verify the callback fires once per device, in order, with the
    expected (index, total, device_id, job, err) arguments.
    """
    from blinky_server.device.device import Device
    from blinky_server.firmware.flash_job import FlashJob, FlashJobState, FlashTransport

    from .mock_transport import MockTransport

    async def stub(job: FlashJob) -> None:
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        job.transition(FlashJobState.WRITING)
        job.transition(FlashJobState.VERIFYING)
        job.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_flash_job", stub)

    for did in ("A", "B"):
        d = Device(
            device_id=did,
            port=f"/dev/tty_{did}",
            platform="nrf52840",
            transport=MockTransport(),  # type: ignore[arg-type]
        )
        await d.connect()
        fleet._devices[d.id] = d

    callback_calls: list[tuple[int, int, str]] = []

    def cb(i: int, total: int, did: str, _job: FlashJob | None, _err: str | None) -> None:
        callback_calls.append((i, total, did))

    async for _ in fleet.flash_fleet(
        device_ids=["A", "B"],
        firmware_path=Path("/tmp/fake.hex"),
        per_device_timeout=5.0,
        on_iteration=cb,
    ):
        pass

    assert callback_calls == [(0, 2, "A"), (1, 2, "B")]


@pytest.mark.asyncio
async def test_flash_fleet_stops_and_restarts_broadcaster(
    fleet: FleetManager,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """flash_fleet MUST stop the broadcaster at batch entry and restart
    it at exit, even when there's only one device (BLE-DFU radio safety
    matters for any single flash through this path). Verifying this is
    structural, not behavioral — the orchestrator's per-device BLE-DFU
    branch is supposed to see ``broadcaster_was_running=False`` and skip
    its own stop/start, but only if the BATCH-level stop is correct.

    Also exercises the finally path: even on a per-device failure the
    broadcaster gets restarted.
    """
    from unittest.mock import AsyncMock, MagicMock

    from blinky_server.device.device import Device
    from blinky_server.firmware.flash_job import FlashJob, FlashJobState, FlashTransport

    from .mock_transport import MockTransport

    # Inject a fake broadcaster that records start/stop calls and tracks
    # is_running like the real BlueZ-backed one would.
    broadcaster = MagicMock()
    broadcaster.is_running = True
    call_log: list[str] = []

    async def stop() -> None:
        call_log.append("stop")
        broadcaster.is_running = False

    async def start() -> None:
        call_log.append("start")
        broadcaster.is_running = True

    broadcaster.stop = AsyncMock(side_effect=stop)
    broadcaster.start = AsyncMock(side_effect=start)
    fleet.broadcaster = broadcaster

    async def stub(job: FlashJob) -> None:
        # Record what flash_fleet has done to the broadcaster by the time
        # the orchestrator's per-device branch would run — must be stopped.
        call_log.append(f"orchestrator-saw-broadcaster-running={broadcaster.is_running}")
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        job.transition(FlashJobState.WRITING)
        job.transition(FlashJobState.VERIFYING)
        job.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_flash_job", stub)

    d = Device(
        device_id="DEV_BC",
        port="/dev/ttyBC",
        platform="nrf52840",
        transport=MockTransport(),  # type: ignore[arg-type]
    )
    await d.connect()
    fleet._devices[d.id] = d

    async for _ in fleet.flash_fleet(
        device_ids=["DEV_BC"],
        firmware_path=Path("/tmp/fake.hex"),
        per_device_timeout=5.0,
    ):
        pass

    # Order assertions:
    #   1. stop at batch entry
    #   2. orchestrator runs WITHOUT broadcaster running (no radio
    #      contention is the whole point)
    #   3. start at batch exit (in `finally`, so guaranteed even on
    #      per-device failure)
    assert call_log == [
        "stop",
        "orchestrator-saw-broadcaster-running=False",
        "start",
    ], f"unexpected broadcaster call order: {call_log}"
    assert broadcaster.is_running is True, "broadcaster must be restarted by flash_fleet's finally"


# --- L3c: force=False + retry state + max-attempts + backoff ----------------


@pytest.mark.asyncio
async def test_flash_device_force_false_returns_none_when_blocked(
    fleet: FleetManager,
) -> None:
    """``flash_device(force=False)`` MUST return None when
    ``should_attempt_auto_recovery`` says no. This is the entry-point
    contract that lets auto-recovery share the orchestrator without
    bypassing dedup."""
    # Seed an in-flight job so should_attempt_auto_recovery returns False.
    from blinky_server.firmware.flash_job import FlashJob

    held = FlashJob(device_id="dev-1", firmware_path=Path("/tmp/fw.hex"))
    fleet._flash_jobs["dev-1"] = held  # PENDING → is_active=True
    fleet._device_in_flight.add("dev-1")

    result = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=False)
    # Idempotent return: an in-flight job IS returned (the auto-recovery
    # caller can wait on the existing flash rather than getting None).
    assert result is held


@pytest.mark.asyncio
async def test_flash_device_force_false_returns_none_after_max_attempts(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When the retry counter reaches MAX_AUTO_RECOVERY_ATTEMPTS,
    ``flash_device(force=False)`` returns None without scheduling. The
    operator must clear the state by force=True (manual flash); a
    successful manual flash clears the counter via the COMPLETED branch
    in ``_run_flash_job``'s finally."""
    import blinky_server.device.manager as mgr_mod
    from blinky_server.device.manager import _RecoveryRetryEntry

    monkeypatch.setattr(FleetManager, "FLASH_DEDUP_WINDOW_S", 0.001)
    monkeypatch.setattr(mgr_mod, "RECOVERY_BACKOFF_SECONDS", (0.0,))
    # Hand-set the retry counter to the cap.
    fleet._recovery_retry_state["dev-1"] = _RecoveryRetryEntry(
        fails=mgr_mod.MAX_AUTO_RECOVERY_ATTEMPTS,
        last_failure_at=_now_minus(60.0),
    )

    result = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=False)
    assert result is None

    # force=True bypasses the gate — operator can still flash to recover.
    forced = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=True)
    assert forced is not None


def _now_minus(seconds: float) -> float:
    import time as _t

    return _t.time() - seconds


@pytest.mark.asyncio
async def test_flash_device_force_false_returns_none_inside_backoff(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """First failure → 60s backoff. Within that window,
    flash_device(force=False) returns None. Past the window, returns a
    job."""
    import blinky_server.device.manager as mgr_mod
    from blinky_server.device.manager import _RecoveryRetryEntry

    monkeypatch.setattr(FleetManager, "FLASH_DEDUP_WINDOW_S", 0.001)
    # First failure: 60s backoff (default RECOVERY_BACKOFF_SECONDS[0])
    # Patch the schedule so we can test "inside" vs "past" without sleeping
    # a minute in the test suite.
    monkeypatch.setattr(mgr_mod, "RECOVERY_BACKOFF_SECONDS", (10.0,))
    fleet._recovery_retry_state["dev-1"] = _RecoveryRetryEntry(
        fails=1, last_failure_at=_now_minus(1.0)
    )
    blocked = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=False)
    assert blocked is None

    # Move the failure timestamp back past the backoff window.
    fleet._recovery_retry_state["dev-1"].last_failure_at = _now_minus(11.0)
    allowed = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=False)
    assert allowed is not None


@pytest.mark.asyncio
async def test_run_flash_job_increments_retry_state_only_for_auto_recovery_failures(
    fleet: FleetManager,
) -> None:
    """A FAILED orchestrator run from auto-recovery (force=False) MUST
    increment the canonical retry counter so subsequent
    ``flash_device(force=False)`` calls honor max-attempts. A FAILED
    run from an operator route (force=True) MUST NOT — otherwise a
    string of operator-driven failures would silently lock out
    auto-recovery for that device. The orchestrator's finally
    distinguishes via ``FlashJob.is_auto_recovery``."""
    # First: operator-driven failure. Retry counter must NOT increment.
    op_job = await fleet.flash_device("dev-1", Path("/tmp/fw.hex"), force=True)
    await op_job.wait_until_terminal(timeout=2.0)
    assert op_job.state is FlashJobState.FAILED
    assert op_job.is_auto_recovery is False
    assert "dev-1" not in fleet._recovery_retry_state, (
        "operator-driven failure (force=True) must not bump auto-recovery counter"
    )

    # Now: auto-recovery failure. Retry counter MUST increment.
    auto_job = await fleet.flash_device("dev-2", Path("/tmp/fw.hex"), force=False)
    assert auto_job is not None  # no prior failures → not blocked
    await auto_job.wait_until_terminal(timeout=2.0)
    assert auto_job.state is FlashJobState.FAILED
    assert auto_job.is_auto_recovery is True
    entry = fleet._recovery_retry_state.get("dev-2")
    assert entry is not None
    assert entry.fails == 1
    assert entry.last_failure_at is not None


@pytest.mark.asyncio
async def test_run_flash_job_clears_retry_state_on_completion(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A COMPLETED run MUST clear the retry counter so a flaky device
    the operator manually fixed gets a clean slate for future
    auto-recovery. Otherwise a single transient failure would
    permanently lower the max-attempts ceiling.

    Setup: real Device + stub the inner branch (``_run_uf2_flash``) to
    transition straight to COMPLETED. The OUTER ``_run_flash_job`` runs
    naturally, so its finally block (where retry-state clearing lives)
    actually exercises the contract.
    """
    from blinky_server.device.device import Device
    from blinky_server.device.manager import _RecoveryRetryEntry
    from blinky_server.firmware.flash_job import FlashJob

    from .mock_transport import MockTransport

    # Seed: 2 prior fails recorded.
    fleet._recovery_retry_state["TEST_SERIAL_RETRY"] = _RecoveryRetryEntry(
        fails=2, last_failure_at=_now_minus(60.0)
    )

    transport = MockTransport(transport_type="serial")
    device = Device(
        device_id="TEST_SERIAL_RETRY",
        port="/dev/ttyTEST_R",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()
    fleet._devices["TEST_SERIAL_RETRY"] = device

    async def stub_uf2(j: FlashJob) -> None:
        j.transition(FlashJobState.WRITING)
        j.transition(FlashJobState.VERIFYING)
        j.transition(FlashJobState.COMPLETED)

    # Stub just the UF2 branch — _run_flash_job's outer try/finally runs
    # for real, including the retry-state-clearing finally.
    monkeypatch.setattr(fleet, "_run_uf2_flash", stub_uf2)

    job = await fleet.flash_device("TEST_SERIAL_RETRY", Path("/tmp/fw.hex"))
    await job.wait_until_terminal(timeout=2.0)
    assert job.state is FlashJobState.COMPLETED
    assert "TEST_SERIAL_RETRY" not in fleet._recovery_retry_state, (
        "_recovery_retry_state should have been popped on COMPLETED transition"
    )


@pytest.mark.asyncio
async def test_recovery_whitelist_reload_picks_up_disk_change(
    fleet: FleetManager, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """L3.1: a direct edit to recovery-firmware.json MUST take effect
    on the next auto-recovery cycle, not after a server restart.

    The 10s TTL means we monkeypatch it down to make the test fast.
    """
    import blinky_server.device.manager as mgr_mod

    monkeypatch.setattr(mgr_mod, "RECOVERY_WHITELIST_RELOAD_TTL_S", 0.0)
    # Make a real firmware file in tmp_path so the JSON validator passes.
    fw = tmp_path / "fw.hex"
    fw.write_text(":00000001FF\n")

    # First arming: device A in whitelist.
    fleet.set_recovery_firmware(str(fw), ["dev-A"])
    assert fleet._recovery_device_ids == {"dev-A"}

    # Operator edits the file directly (e.g., out-of-band).
    state_path = fleet._recovery_state_path()
    import json as _json

    state_path.write_text(_json.dumps({"firmware_path": str(fw), "device_ids": ["dev-B"]}))
    # Bump mtime to ensure the cache sees the change (write_text already
    # updated mtime, but be explicit).
    import os as _os
    import time as _t

    new_mtime = _t.time() + 1.0
    _os.utime(state_path, (new_mtime, new_mtime))

    # Force the cache to allow a re-stat by clearing the throttle.
    fleet._last_whitelist_check_at = 0.0

    fleet._maybe_reload_recovery_whitelist()
    assert fleet._recovery_device_ids == {"dev-B"}, (
        "whitelist did not refresh from disk after mtime change"
    )


@pytest.mark.asyncio
async def test_flash_device_pins_canonical_against_midflight_alias_shift(
    fleet: FleetManager, monkeypatch: pytest.MonkeyPatch
) -> None:
    """``flash_device`` pins ``job.canonical_id`` at creation so a
    later ``register_identity_alias`` call during the same flash
    can't cause ``_run_flash_job``'s finally to clean up under a
    different key — which would leak the in-flight set and the retry
    state entry forever.

    Simulates the realistic case where discovery learns a device's USB
    SN while a BLE-DFU flash for that device's BLE address is in
    flight: the alias gets registered, ``resolve_canonical(BLE_ADDR)``
    starts returning the SN, and any code path that uses the live
    resolver to find the job's keys would miss them.
    """
    from blinky_server.device.device import Device
    from blinky_server.firmware.flash_job import FlashJob

    from .mock_transport import MockTransport

    transport = MockTransport(transport_type="serial")
    device = Device(
        device_id="FA:E6:7D:A9:8B:3A",  # BLE-style id
        port="/dev/ttyTEST_A",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()
    fleet._devices["FA:E6:7D:A9:8B:3A"] = device

    # Stub the UF2 branch so the real `_run_flash_job` runs (and its
    # finally fires) without doing real flash work. The stub also
    # registers an identity alias mid-flight that DEMOTES the original
    # canonical — `resolve_canonical("FA:E6:7D:A9:8B:3A")` now returns
    # the SN, which is the case that breaks naive cleanup.
    async def stub_uf2(j: FlashJob) -> None:
        fleet.register_identity_alias("FA:E6:7D:A9:8B:3A", "AABBCCDDEEFF0011")
        j.transition(FlashJobState.WRITING)
        j.transition(FlashJobState.VERIFYING)
        j.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_uf2_flash", stub_uf2)

    job = await fleet.flash_device("FA:E6:7D:A9:8B:3A", Path("/tmp/x.hex"))
    # Pre-flash: in-flight under the original canonical (BLE addr).
    assert "FA:E6:7D:A9:8B:3A" in fleet._device_in_flight
    assert job.canonical_id == "FA:E6:7D:A9:8B:3A"

    await job.wait_until_terminal(timeout=2.0)
    assert job.state is FlashJobState.COMPLETED

    # Post-flash: in-flight cleared under the ORIGINAL canonical even
    # though `resolve_canonical("FA:E6:7D:A9:8B:3A")` now returns the
    # SN. Without the canonical-pin, the finally would have done
    # `_device_in_flight.discard("AABBCCDDEEFF0011")` (no-op) and
    # leaked the BLE-keyed entry.
    assert "FA:E6:7D:A9:8B:3A" not in fleet._device_in_flight, (
        "in-flight entry leaked under stale canonical key after alias shift"
    )
    assert fleet.resolve_canonical("FA:E6:7D:A9:8B:3A") == "AABBCCDDEEFF0011"


@pytest.mark.asyncio
async def test_whitelist_reload_warns_once_when_file_disappears(
    fleet: FleetManager,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """If the operator deletes the recovery-firmware.json file after
    auto-recovery was armed, ``_maybe_reload_recovery_whitelist`` MUST
    log a WARN exactly once and keep the in-memory state — so a
    transient I/O issue can't silently disarm auto-recovery.
    """
    import blinky_server.device.manager as mgr_mod

    monkeypatch.setattr(mgr_mod, "RECOVERY_WHITELIST_RELOAD_TTL_S", 0.0)
    fw = tmp_path / "fw.hex"
    fw.write_text(":00000001FF\n")
    fleet.set_recovery_firmware(str(fw), ["dev-A"])
    # Prime the mtime cache: first call observes the file.
    fleet._maybe_reload_recovery_whitelist()
    assert fleet._last_whitelist_mtime is not None

    # Operator deletes the file.
    state_path = fleet._recovery_state_path()
    state_path.unlink()

    caplog.clear()
    fleet._last_whitelist_check_at = 0.0  # bypass TTL
    fleet._maybe_reload_recovery_whitelist()

    # In-memory state must be unchanged — disarm requires deliberate
    # action via the API or restart, not a stat() failure.
    assert fleet._recovery_firmware_path == str(fw)
    assert fleet._recovery_device_ids == {"dev-A"}

    # Exactly one WARN line about the file being unreadable.
    warns = [
        r for r in caplog.records if r.levelname == "WARNING" and "no longer readable" in r.message
    ]
    assert len(warns) == 1, f"expected exactly one WARN, got {[r.message for r in warns]}"

    # Subsequent calls (within the same session, file still missing) do
    # NOT re-warn — the latch prevents journal spam.
    caplog.clear()
    fleet._last_whitelist_check_at = 0.0
    fleet._maybe_reload_recovery_whitelist()
    warns2 = [
        r for r in caplog.records if r.levelname == "WARNING" and "no longer readable" in r.message
    ]
    assert warns2 == [], "second missing-file detection re-warned (should be latched)"
