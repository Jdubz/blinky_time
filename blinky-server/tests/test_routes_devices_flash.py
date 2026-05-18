"""L3a tests: ``POST /api/devices/{id}/flash`` routes through
``FleetManager.flash_device()``.

These tests prove the L3a migration contract: the route MUST go
through the orchestrator (so the lockdown's per-device protections
apply) rather than calling the legacy ``firmware.upload_firmware``
dispatcher directly. The legacy path is gone from this route entirely
— ``upload_firmware`` is no longer imported here. After L3b migrates
``POST /api/fleet/flash`` the same way and L3c migrates auto-recovery,
L3d deletes ``upload_firmware`` outright.
"""

from __future__ import annotations

from collections.abc import AsyncGenerator
from pathlib import Path
from typing import Any

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api.app import create_app
from blinky_server.api.deps import require_api_key, require_deploy_tool, set_fleet
from blinky_server.device.device import Device, DeviceState
from blinky_server.device.manager import FleetManager
from blinky_server.firmware.flash_job import FlashJob, FlashJobState, FlashTransport

from .mock_transport import MockTransport

# NOTE: ``conftest.py`` provides an autouse ``_isolated_data_dir`` fixture
# that redirects ``paths.data_dir()`` to a per-test tmp dir for every
# test. Without it, tests calling ``fleet.set_recovery_firmware()``
# (directly or via a flash route) would leak into the developer's real
# ``~/.local/share/blinky-server/recovery-firmware.json`` and the
# running blinky-server would read that leak on boot. Verified by
# accident on 2026-05-18.


@pytest_asyncio.fixture
async def api_with_device() -> AsyncGenerator[tuple[AsyncClient, FleetManager, Device], None]:
    """Client + fleet + a single CONNECTED nrf52840 device.

    Auth gates bypassed so tests focus on flash-route business logic.
    The device's transport is a ``MockTransport`` (no real hardware)
    so anything the orchestrator does locally (e.g. transport.disconnect)
    is a no-op.
    """
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    transport = MockTransport()
    device = Device(
        device_id="L3A_TEST_DEV",
        port="/dev/ttyTEST0",
        platform="nrf52840",
        transport=transport,  # type: ignore[arg-type]
    )
    await device.connect()
    device.state = DeviceState.CONNECTED
    fleet._devices[device.id] = device

    set_fleet(fleet)
    app = create_app()
    app.dependency_overrides[require_api_key] = lambda: None
    app.dependency_overrides[require_deploy_tool] = lambda: None
    http_transport = ASGITransport(app=app)  # type: ignore[arg-type]
    async with AsyncClient(transport=http_transport, base_url="http://test") as client:
        yield client, fleet, device

    await device.disconnect()
    set_fleet(None)  # type: ignore[arg-type]


@pytest.fixture
def fw_file(tmp_path: Path) -> Path:
    """A valid-looking firmware file under /tmp (in the route's
    allowed-dirs whitelist)."""
    p = tmp_path / "fw.hex"
    p.write_text(":00000001FF\n")
    return p


async def test_route_routes_through_orchestrator(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
    fw_file: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The whole point of L3a: the route MUST go through
    ``FleetManager.flash_device()`` (i.e. ``_run_flash_job``), not the
    legacy ``firmware.upload_firmware`` dispatcher.

    We prove it by spying on ``_run_flash_job`` — the orchestrator's
    entry point is the only code that sets the lockdown ContextVar +
    drives the FlashJob state machine. If the route bypassed the
    orchestrator, this spy would never see the call.
    """
    client, fleet, device = api_with_device
    run_calls: list[FlashJob] = []

    original = fleet._run_flash_job

    async def spy(job: FlashJob) -> None:
        run_calls.append(job)
        # Drive the job to a terminal state without doing actual flash work,
        # so the route's wait_until_terminal returns.
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        job.transition(FlashJobState.WRITING)
        job.transition(FlashJobState.VERIFYING)
        job.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_flash_job", spy)

    resp = await client.post(
        f"/api/devices/{device.id}/flash",
        json={"firmware_path": str(fw_file)},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["status"] == "ok"
    assert "uf2" in body["message"].lower()
    assert body["elapsed_s"] >= 0

    assert len(run_calls) == 1, (
        f"_run_flash_job called {len(run_calls)} times; "
        "route did NOT route through the orchestrator"
    )
    assert run_calls[0].device_id == device.id

    # Restore for the fixture teardown
    monkeypatch.setattr(fleet, "_run_flash_job", original)


async def test_route_surfaces_orchestrator_failure_as_500(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
    fw_file: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When the orchestrator transitions FAILED, the route surfaces the
    job's error as HTTP 500. The route MUST NOT translate this to a 200
    OK — silent failure would defeat the whole point of the lockdown."""
    client, fleet, device = api_with_device

    async def failing_orchestrator(job: FlashJob) -> None:
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        job.set_error("synthetic failure for test")
        job.transition(FlashJobState.FAILED)

    monkeypatch.setattr(fleet, "_run_flash_job", failing_orchestrator)

    resp = await client.post(
        f"/api/devices/{device.id}/flash",
        json={"firmware_path": str(fw_file)},
    )
    assert resp.status_code == 500
    assert "synthetic failure" in resp.json()["detail"]


async def test_route_rejects_firmware_outside_allowed_dirs(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
) -> None:
    """Firmware paths outside ``/tmp`` and ``$HOME`` are rejected
    before the orchestrator is even reached — closes the
    arbitrary-path probe vector."""
    client, _, device = api_with_device
    resp = await client.post(
        f"/api/devices/{device.id}/flash",
        json={"firmware_path": "/etc/passwd"},
    )
    assert resp.status_code == 400
    assert "allowed directory" in resp.json()["detail"]


async def test_route_rejects_missing_firmware_file(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
    tmp_path: Path,
) -> None:
    """A path inside an allowed dir but with no file on disk → 400."""
    client, _, device = api_with_device
    missing = tmp_path / "does_not_exist.hex"
    resp = await client.post(
        f"/api/devices/{device.id}/flash",
        json={"firmware_path": str(missing)},
    )
    assert resp.status_code == 400
    assert "not found" in resp.json()["detail"]


async def test_route_rejects_device_in_wrong_state(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
    fw_file: Path,
) -> None:
    """A device that's not CONNECTED / DFU_RECOVERY / PRESENT can't be
    flashed — return 409 BEFORE attempting to schedule the job."""
    client, _, device = api_with_device
    device.state = DeviceState.DISCONNECTED
    resp = await client.post(
        f"/api/devices/{device.id}/flash",
        json={"firmware_path": str(fw_file)},
    )
    assert resp.status_code == 409
    assert "not connected" in resp.json()["detail"]


async def test_route_arms_recovery_whitelist_for_target_only(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
    fw_file: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The route must arm the auto-recovery whitelist for ONLY the
    target device, so a future DFU bounce doesn't auto-flash unrelated
    devices. This was a load-bearing protection in the legacy route
    (per ``feedback_flash_safety_policy``) and it survives the L3a
    migration."""
    client, fleet, device = api_with_device

    # Stub the orchestrator so the route returns quickly.
    async def fast_terminal(job: FlashJob) -> None:
        job.transition(FlashJobState.SELECTING_TRANSPORT)
        job.set_transport(FlashTransport.UF2)
        job.transition(FlashJobState.WRITING)
        job.transition(FlashJobState.VERIFYING)
        job.transition(FlashJobState.COMPLETED)

    monkeypatch.setattr(fleet, "_run_flash_job", fast_terminal)

    captured: list[tuple[str, list[str]]] = []
    original_set = fleet.set_recovery_firmware

    def spy_set(firmware: str, device_ids: list[str] | None = None) -> Any:
        captured.append((firmware, list(device_ids or [])))
        return original_set(firmware, device_ids)

    monkeypatch.setattr(fleet, "set_recovery_firmware", spy_set)

    resp = await client.post(
        f"/api/devices/{device.id}/flash",
        json={"firmware_path": str(fw_file)},
    )
    assert resp.status_code == 200, resp.text
    assert captured == [(str(fw_file), [device.id])], (
        f"route must arm the whitelist with the target device only (got {captured!r})"
    )


async def test_route_blocks_when_legacy_dfu_lock_held(
    api_with_device: tuple[AsyncClient, FleetManager, Device],
    fw_file: Path,
) -> None:
    """Transitional protection (L3a → L3c): the route still acquires the
    legacy DFU lock, which is what auto-recovery currently checks before
    starting a parallel BLE-DFU. If something already holds the lock,
    the route MUST return 409 rather than starting a racing flash.

    This test goes away in L3c when auto-recovery migrates to
    ``flash_device()`` and ``_dfu_locks`` is deleted (canonical
    in-flight set takes over)."""
    client, fleet, device = api_with_device
    lock = fleet.get_dfu_lock(device.id)
    await lock.acquire()
    try:
        resp = await client.post(
            f"/api/devices/{device.id}/flash",
            json={"firmware_path": str(fw_file)},
        )
        assert resp.status_code == 409
        assert "DFU already in progress" in resp.json()["detail"]
    finally:
        lock.release()
