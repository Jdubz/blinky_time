from __future__ import annotations

from typing import Any

from fastapi import APIRouter, HTTPException

from ..device.device import Device, DeviceState
from .deps import get_fleet
from .models import (
    CommandResponse, DeviceResponse, OtaRequest, OtaResponse,
    ReleaseRequest, SettingValueRequest, StatusResponse,
)

router = APIRouter(tags=["devices"])


def _get_device_or_404(device_id: str) -> Device:
    device = get_fleet().get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")
    return device


@router.get("/devices")
async def list_devices() -> list[DeviceResponse]:
    """List all known devices."""
    return [DeviceResponse(**d.to_dict()) for d in get_fleet().get_all_devices()]


@router.get("/devices/{device_id}")
async def get_device(device_id: str) -> DeviceResponse:
    """Get info for a specific device."""
    device = _get_device_or_404(device_id)
    return DeviceResponse(**device.to_dict())


@router.get("/devices/{device_id}/settings")
async def get_settings(device_id: str) -> list[dict[str, Any]]:
    """Get all settings for a device."""
    device = _get_device_or_404(device_id)
    return await device.protocol.get_settings()


@router.get("/devices/{device_id}/settings/{category}")
async def get_settings_by_category(device_id: str, category: str) -> list[dict[str, Any]]:
    """Get settings for a device filtered by category."""
    device = _get_device_or_404(device_id)
    all_settings = await device.protocol.get_settings()
    return [s for s in all_settings if s.get("cat") == category]


@router.put("/devices/{device_id}/settings/{name}")
async def set_setting(device_id: str, name: str, body: SettingValueRequest) -> CommandResponse:
    """Set a single setting value."""
    device = _get_device_or_404(device_id)
    resp = await device.protocol.set_setting(name, body.value)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/save")
async def save_settings(device_id: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.save_settings()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/load")
async def load_settings(device_id: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.load_settings()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/defaults")
async def restore_defaults(device_id: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
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


@router.post("/devices/{device_id}/ota")
async def ota_upload(device_id: str, body: OtaRequest) -> OtaResponse:
    """Upload firmware to a device via OTA (UF2 for serial, BLE DFU for BLE).

    The server handles the entire flow:
    1. Sends bootloader command via its existing transport
    2. Disconnects the transport
    3. Detects UF2 drive / establishes BLE DFU connection
    4. Copies firmware / performs DFU transfer
    5. Waits for device to reboot
    6. Reconnects automatically via fleet manager

    No port contention — the server owns the connection throughout.
    """
    import os
    device = _get_device_or_404(device_id)

    if device.state != DeviceState.CONNECTED:
        raise HTTPException(409, f"Device not connected (state={device.state.value})")

    # Validate firmware path
    if not os.path.isfile(body.firmware_path):
        raise HTTPException(400, f"Firmware file not found: {body.firmware_path}")

    transport_type = device.transport.transport_type
    platform = device.platform

    if platform != "nrf52840":
        raise HTTPException(400, f"OTA not yet supported for platform: {platform}")

    if transport_type == "serial":
        from ..ota.uf2_upload import upload_uf2

        # Blackout auto-reconnect while we do the upload
        fleet = get_fleet()
        fleet._reconnect_blackout[device_id] = __import__("time").monotonic() + 120

        result = await upload_uf2(
            serial_port=device.port,
            firmware_path=body.firmware_path,
            send_command=device.protocol.send_command,
            disconnect=device.transport.disconnect,
        )

        # Clear blackout — fleet manager will auto-reconnect on next cycle
        fleet._reconnect_blackout.pop(device_id, None)

        if result["status"] == "ok":
            return OtaResponse(**result)
        else:
            raise HTTPException(500, result["message"])

    elif transport_type == "ble":
        raise HTTPException(501, "BLE DFU upload not yet implemented in server")

    else:
        raise HTTPException(400, f"OTA not supported for transport: {transport_type}")
