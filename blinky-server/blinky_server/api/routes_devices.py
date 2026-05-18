from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, Depends, HTTPException

from ..device.device import Device, DeviceState
from ..firmware.flash_job import FlashJobState
from .deps import get_fleet, require_api_key, require_deploy_tool
from .models import (
    CommandResponse,
    DeviceResponse,
    FlashRequest,
    FlashResponse,
    ReleaseRequest,
    SettingValueRequest,
    StatusResponse,
)

log = logging.getLogger(__name__)
router = APIRouter(tags=["devices"])


def _get_device_or_404(device_id: str) -> Device:
    device = get_fleet().get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")
    return device


def _get_connected_device(device_id: str) -> Device:
    device = _get_device_or_404(device_id)
    if device.state != DeviceState.CONNECTED:
        raise HTTPException(
            409,
            f"Device {device_id[:12]} is not connected (state={device.state.value}). "
            + (
                "Use POST /devices/{id}/flash to recover."
                if device.state == DeviceState.DFU_RECOVERY
                else ""
            ),
        )
    return device


@router.get("/devices")
async def list_devices() -> list[DeviceResponse]:
    """List all known devices.

    BLE devices report ``state="present"`` and have most metadata fields
    null because we don't hold a connection to read them. Commands to BLE
    devices go through ``/api/fleet/*`` (broadcast), not per-device.
    """
    return [DeviceResponse(**d.to_dict()) for d in get_fleet().get_all_devices()]


@router.get("/devices/{device_id}")
async def get_device(device_id: str) -> DeviceResponse:
    """Get info for a specific device."""
    device = _get_device_or_404(device_id)
    return DeviceResponse(**device.to_dict())


@router.get("/devices/{device_id}/settings")
async def get_settings(device_id: str) -> list[dict[str, Any]]:
    """Get all settings for a device."""
    device = _get_connected_device(device_id)
    return await device.protocol.get_settings()


@router.get("/devices/{device_id}/settings/{category}")
async def get_settings_by_category(device_id: str, category: str) -> list[dict[str, Any]]:
    """Get settings for a device filtered by category."""
    device = _get_connected_device(device_id)
    all_settings = await device.protocol.get_settings()
    return [s for s in all_settings if s.get("cat") == category]


@router.put("/devices/{device_id}/settings/{name}")
async def set_setting(device_id: str, name: str, body: SettingValueRequest) -> CommandResponse:
    """Set a single setting value."""
    device = _get_connected_device(device_id)
    resp = await device.protocol.set_setting(name, body.value)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/save")
async def save_settings(device_id: str) -> CommandResponse:
    device = _get_connected_device(device_id)
    resp = await device.protocol.save_settings()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/load")
async def load_settings(device_id: str) -> CommandResponse:
    device = _get_connected_device(device_id)
    resp = await device.protocol.load_settings()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/defaults")
async def restore_defaults(device_id: str) -> CommandResponse:
    device = _get_connected_device(device_id)
    resp = await device.protocol.restore_defaults()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/release")
async def release_device(device_id: str, body: ReleaseRequest | None = None) -> StatusResponse:
    """Release a device (e.g., for firmware flashing)."""
    hold_seconds = body.hold_seconds if body else None
    ok = await get_fleet().release_device(device_id, hold_seconds=hold_seconds)
    if not ok:
        raise HTTPException(404, f"Device not found: {device_id}")
    return StatusResponse(status="released")


@router.post("/devices/{device_id}/reconnect")
async def reconnect_device(device_id: str) -> StatusResponse:
    """Reconnect a previously released device."""
    ok = await get_fleet().reconnect_device(device_id)
    if not ok:
        raise HTTPException(500, f"Failed to reconnect: {device_id}")
    return StatusResponse(status="connected")


@router.delete("/devices/{device_id}")
async def remove_device(device_id: str) -> StatusResponse:
    """Permanently remove a device from the fleet.

    Use when a device has been physically disconnected and should no
    longer appear in the device list. The device will be rediscovered
    automatically if it reappears on USB or BLE.
    """
    fleet = get_fleet()
    device = fleet.get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")
    await fleet.remove_device(device.id)
    return StatusResponse(status="removed")


@router.get("/fleet/status")
async def fleet_status() -> dict[str, Any]:
    """Fleet health summary — aggregate stats across all devices.

    Returns counts by state and transport, plus per-device health info
    (RSSI, last_seen, transport, staleness). Includes broadcaster health
    (BLE commands go through it) and background-loop health.
    """
    fleet = get_fleet()
    devices = fleet.get_all_devices()

    by_state: dict[str, int] = {}
    by_transport: dict[str, int] = {}
    stale_count = 0
    device_health: list[dict[str, Any]] = []

    for d in devices:
        state = d.state.value
        ttype = d.transport.transport_type
        by_state[state] = by_state.get(state, 0) + 1
        by_transport[ttype] = by_transport.get(ttype, 0) + 1
        if d.is_stale:
            stale_count += 1
        ago = d.last_seen_ago
        device_health.append(
            {
                "id": d.id,
                "name": d.device_name,
                "transport": ttype,
                "state": state,
                "rssi": d.rssi,
                "last_seen_ago": round(ago, 1) if ago is not None else None,
                "stale": d.is_stale,
                "version": d.version,
                "ble_address": d.ble_address,
            }
        )

    broadcaster_status = fleet.broadcaster.status() if fleet.broadcaster is not None else None
    return {
        "total": len(devices),
        "connected": by_state.get("connected", 0),
        "present": by_state.get("present", 0),
        "stale": stale_count,
        "dfu_recovery": by_state.get("dfu_recovery", 0),
        "by_state": by_state,
        "by_transport": by_transport,
        "loop": fleet.loop_health(),
        "broadcaster": broadcaster_status,
        "devices": device_health,
    }


@router.post("/fleet/discover")
async def fleet_discover() -> dict[str, Any]:
    """Trigger immediate device discovery scan.

    Runs BLE + serial discovery now instead of waiting for the 10s
    background loop. Returns newly discovered devices. Useful when
    adding a new device to the fleet.
    """
    fleet = get_fleet()
    before = set(fleet.devices.keys())
    await fleet.discover_now()
    after = set(fleet.devices.keys())
    new_ids = after - before

    new_devices = []
    for did in new_ids:
        dev = fleet.devices.get(did)
        if dev:
            new_devices.append(dev.to_dict())

    return {
        "status": "ok",
        "total": len(after),
        "new_devices": new_devices,
        "message": f"Found {len(new_ids)} new device(s)" if new_ids else "No new devices found",
    }


@router.post(
    "/devices/{device_id}/flash",
    dependencies=[Depends(require_api_key), Depends(require_deploy_tool)],
)
async def flash_device(device_id: str, body: FlashRequest) -> FlashResponse:
    """Upload firmware to a device using the best available method.

    L3a (FLASH_LOCKDOWN_PLAN.md): the route validates inputs + arms the
    auto-recovery whitelist, then delegates the actual write to
    ``FleetManager.flash_device()``. The orchestrator owns every
    per-device protection (canonical-key dedup, in-flight set, transport
    selection, BLE radio coordination, write, verify, cleanup). Direct
    calls to the legacy ``firmware.upload_firmware`` dispatcher are gone
    — the only path to a write impl is now ``_run_flash_job``.

    Automatic transport selection happens inside the orchestrator's
    ``select_transport`` based on a fresh probe of the device:
      - Serial reachable → UF2 (preferred; safer protocol, faster)
      - BLE DFU bootloader advert observed → BLE DFU direct
      - BLE app-mode → BLE DFU after issuing ``bootloader ble`` over NUS
    """
    from pathlib import Path

    device = _get_device_or_404(device_id)

    if device.state not in (
        DeviceState.CONNECTED,
        DeviceState.DFU_RECOVERY,
        DeviceState.PRESENT,
    ):
        raise HTTPException(409, f"Device not connected (state={device.state.value})")

    # Validate firmware path — restrict to allowed directories
    firmware = Path(body.firmware_path).resolve()
    allowed_dirs = [Path("/tmp"), Path.home()]
    if not any(firmware.is_relative_to(d) for d in allowed_dirs):
        raise HTTPException(400, f"Firmware path not in allowed directory: {firmware}")
    if not firmware.is_file():
        raise HTTPException(400, f"Firmware file not found: {firmware}")

    if device.platform != "nrf52840":
        raise HTTPException(400, f"Upload not yet supported for platform: {device.platform}")

    fleet = get_fleet()
    # Arm auto-recovery for THIS device only — the explicit whitelist keeps
    # a future DFU bounce from auto-flashing unrelated devices. The
    # orchestrator's canonical in-flight set + ``should_attempt_auto_recovery``
    # dedup window are what stop auto-recovery from racing this flash in
    # particular; the whitelist controls which devices it would TRY for.
    fleet.set_recovery_firmware(str(firmware), [device.id])

    # ── Transitional DFU lock (L3a → L3c) ────────────────────────────────
    # Until L3c migrates the auto-recovery branch to ``flash_device()``, it
    # still calls the legacy ``upload_ble_dfu`` wrapper directly. Holding
    # the legacy DFU lock here is what stops auto-recovery from starting a
    # parallel BLE-DFU on the same device while THIS flash is in-flight
    # (auto-recovery's own ``dfu_lock.locked()`` check skips when held).
    # After L3c, auto-recovery goes through ``flash_device()`` too and the
    # canonical in-flight set is sufficient; this whole block + the
    # ``_dfu_locks`` field disappear at that point.
    dfu_lock = fleet.get_dfu_lock(device_id)
    if dfu_lock.locked():
        raise HTTPException(409, f"DFU already in progress for device {device_id}")

    is_serial_flash = device.transport.transport_type == "serial"
    sibling_ids: list[str] = []

    async with dfu_lock:
        # ── Sibling USB-bus protection (UF2 only) ─────────────────────────
        # UF2 flash performs a USB bus reset (USBDEVFS_RESET on the target's
        # USB device path) that briefly disrupts ALL serial devices on the
        # same hub. Hold sibling auto-reconnect timers across the flash so
        # the manager doesn't redial them mid-bus-reset.
        #
        # We do NOT pause discovery or hold-reconnect the TARGET device:
        # the orchestrator's ``_FleetVerifySignals.is_serial_connected()``
        # poll requires ``device.transport.is_connected`` to become True
        # after the post-flash reboot, and that re-connection is driven by
        # the discovery loop. Live test 2026-05-18 on FPS Sweep with
        # discovery paused: verify stalled in AWAITING_APP_BOOT for the
        # full 5 min cap (the device WAS up, but its transport never
        # reconnected). With discovery running: verify converges in ~15s.
        #
        # The BLE-DFU path doesn't need anything from the route: the
        # orchestrator calls ``pause_discovery`` + ``hold_reconnect`` +
        # ``broadcaster.stop()`` inside ``_run_ble_dfu_flash`` for the
        # narrower window where they're actually needed (BLE radio
        # coordination) and releases them before its own verify scan.
        if is_serial_flash:
            for dev in fleet.get_all_devices():
                if dev.id != device.id and dev.transport.transport_type == "serial":
                    fleet.hold_reconnect(dev.id, 600)
                    sibling_ids.append(dev.id)

        try:
            # ── Single flash entry point ──────────────────────────────────
            # ``flash_device()`` returns a ``FlashJob`` immediately (it
            # schedules ``_run_flash_job`` as a background task). The
            # orchestrator's internal wall-clocks cap the run: ~600s for the
            # write + 300s for verify on UF2; 600s wait_for on
            # ``_ble_dfu_write_impl`` for the BLE path. The 900s outer cap
            # here is the route's safety net if the orchestrator itself
            # hangs past its own deadlines.
            job = await fleet.flash_device(
                device_id=device.id,
                firmware_path=firmware,
            )
            try:
                await job.wait_until_terminal(timeout=900.0)
            except TimeoutError as exc:
                raise HTTPException(
                    504,
                    f"flash job {job.job_id} did not reach terminal state within 900s",
                ) from exc

            if job.state is FlashJobState.COMPLETED:
                transport_name = job.transport.value if job.transport else "unknown"
                return FlashResponse(
                    status="ok",
                    message=f"Firmware uploaded via {transport_name}",
                    elapsed_s=round(job.duration_s, 2),
                )
            # FAILED or ABANDONED — surface the orchestrator's error
            raise HTTPException(
                500,
                job.error or f"flash {job.state.value} without specific error",
            )
        finally:
            for sid in sibling_ids:
                fleet.resume_reconnect(sid)
