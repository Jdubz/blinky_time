from __future__ import annotations

import asyncio
import logging
from typing import Any

from fastapi import APIRouter, Depends, HTTPException

from ..device.device import Device, DeviceState
from ..firmware import upload_firmware
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

    Automatically selects the upload method based on device transport:
      - Serial devices: UF2 (USB mass storage), BLE DFU fallback
      - BLE devices: BLE DFU with bootloader entry over BLE
      - DFU recovery: BLE DFU direct (device already in bootloader)

    Serial UF2 is always preferred over wireless methods.
    """
    from pathlib import Path

    device = _get_device_or_404(device_id)

    # PRESENT BLE devices are reachable for flash: we just-in-time connect
    # for the flash via the existing BleTransport, with the broadcaster
    # paused for the duration (single-radio adapters refuse central role
    # while advertising).
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
    # Per-device flash arms auto-recovery for THIS device only — the explicit
    # whitelist keeps a future DFU bounce from auto-flashing unrelated devices.
    fleet.set_recovery_firmware(str(firmware), [device.id])

    # Hold time: UF2 takes ~60s, BLE DFU takes ~8 min
    is_ble_only = (
        device.state == DeviceState.DFU_RECOVERY or device.transport.transport_type == "ble"
    )
    hold_time = 600 if is_ble_only else 120

    # Acquire per-device DFU lock — prevents concurrent DFU from auto-recovery
    dfu_lock = fleet.get_dfu_lock(device_id)
    if dfu_lock.locked():
        raise HTTPException(409, f"DFU already in progress for device {device_id}")

    async with dfu_lock:
        # UF2 flash causes a USB bus reset that disrupts ALL serial devices on
        # the same hub. Hold reconnect on sibling serial devices so the manager
        # doesn't try to reconnect them while the bus is unstable.
        # Sibling holds are inside the lock context so they're always cleaned
        # up by the finally block, even on unexpected exceptions.
        is_serial_flash = device.transport.transport_type == "serial"
        sibling_ids: list[str] = []
        if is_serial_flash:
            for dev in fleet.get_all_devices():
                if dev.id != device.id and dev.transport.transport_type == "serial":
                    fleet.hold_reconnect(dev.id, hold_time)
                    sibling_ids.append(dev.id)

        fleet.hold_reconnect(device_id, hold_time)
        fleet.pause_discovery()
        # Stop the broadcaster: BLE DFU needs central GATT, which the
        # BCM43455 can't do while advertising. Restart it after the flash.
        broadcaster_was_running = fleet.broadcaster is not None and fleet.broadcaster.is_running
        if broadcaster_was_running:
            try:
                assert fleet.broadcaster is not None  # narrowed by check above
                await fleet.broadcaster.stop()
            except Exception:
                log.exception("Failed to pause broadcaster cleanly; flash will still try")
            # P3 guard: assert broadcaster is actually stopped before we
            # enter DFU. If stop() failed silently the radio would still
            # be advertising while we try to act as central — the BCM43455
            # rejects that with bluez "Failed (0x03)" and we'd brick a
            # device. Don't start a destructive operation under uncertainty.
            if fleet.broadcaster is not None and fleet.broadcaster.is_running:
                raise HTTPException(
                    503,
                    "Broadcaster did not stop cleanly — refusing to start BLE flash "
                    "to avoid radio contention. Check logs and restart blinky-server.",
                )
        try:
            # P5: hard timeout on the whole flash. Observed transfer rate is
            # ~21s per 10% of the application image (542 KB), so ~3.5 min for
            # the transfer plus ~1 min for init/scan/cache. 600s is generous
            # (~2x worst case). Past that, abort cleanly with an error rather
            # than letting the operation run until something else (watchdog,
            # external timeout) tears it down destructively mid-write.
            result = await asyncio.wait_for(
                upload_firmware(device, str(firmware)),
                timeout=600.0,
            )
        except TimeoutError:
            log.error(
                "Flash of %s (%s) exceeded 600s — aborted. Device may be in DFU bootloader.",
                device_id[:12],
                device.device_name or "unknown",
            )
            result = {
                "status": "error",
                "message": "flash exceeded 600s timeout",
                "elapsed_s": 600.0,
            }
        finally:
            device.state = DeviceState.DISCONNECTED
            if is_serial_flash:
                # Wait for USB bus to stabilize after UF2 bootloader exit
                await asyncio.sleep(5)
            fleet.resume_discovery()
            fleet.resume_reconnect(device_id)
            for sid in sibling_ids:
                fleet.resume_reconnect(sid)
            if broadcaster_was_running and fleet.broadcaster is not None:
                try:
                    await fleet.broadcaster.start()
                except Exception:
                    log.exception(
                        "Failed to restart broadcaster after flash — fleet commands "
                        "will not reach BLE devices until the server is restarted"
                    )

    if result["status"] == "ok":
        return FlashResponse(**result)
    else:
        raise HTTPException(500, result["message"])
